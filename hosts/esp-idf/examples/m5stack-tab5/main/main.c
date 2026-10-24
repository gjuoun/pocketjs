#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"
#include "driver/ppa.h"
#include "esp_async_memcpy.h"
#include "esp_cache.h"
#include "esp_hosted.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_private/gdma.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pocketjs/esp32p4_ppa.h"
#include "pocketjs/guest.h"
#include "pocketjs/package.h"
#include "pocketjs/render_rgb565.h"
#include "pocketjs/runner.h"
#include "pocketjs/system.h"
#include "pocketjs/ui_core.h"
#include "pocketjs/ui_qjs.h"
#include "pocketjs_package_app.h"

static const char *TAG = "pocketjs_tab5";

/* Bytes per strip at most: 24 logical rows of the portrait panel at density
 * 2. A strip holds the whole logical rows that fit. */
#define TAB5_STRIP_BYTES (720U * 48U * sizeof(uint16_t))
/* Upper bound of strips in one frame: eight damage regions of up to 32
 * strips each (640 portrait rows / 24, 360 landscape rows / 13). */
#define TAB5_MAX_STRIPS (8U * 32U)
/* Copies queued at once on the mover's DMA channel, and panel rows per
 * copy: each copy allocates DMA descriptors for its bytes in internal RAM. */
#define TAB5_MOVE_BACKLOG 8U
#define TAB5_MOVE_ROWS 48U
/* Panel rows a move down must reach to copy rows in place: shorter moves
 * would need one copy per few rows and bounce through strips instead. */
#define TAB5_MIN_DIRECT_ROWS 32U
/* Event bit set once the frame's motion has moved its pixels. */
#define TAB5_MOVED_BIT BIT0
/* The PPA stops completing 90-degree presents of blocks 4 or 6 raster rows
 * tall while the panel scans out (measured on ESP32-P4 rev 1.3; 2 and 8 to
 * 28 rows complete), so no turned strip is shorter than this. */
#define TAB5_MIN_TURNED_ROWS 8U
/* The home button drawn below a picture shorter than the screen (or beside
 * one narrower than it): a square of oriented screen pixels, and the SELECT
 * bit it sends. */
#define TAB5_BUTTON_SIZE 88U
#define TAB5_BUTTON_SELECT 0x0001U
/* The embedded app is a launcher (Pocket Home): it installs apps from the
 * linked Mac and switches between them (pocketjs_system). */
#ifndef TAB5_LAUNCHER
#define TAB5_LAUNCHER 0
#endif
/* Where the launcher keeps installed apps: the microSD card when one is in
 * the slot, the `storage` flash partition otherwise. */
#define TAB5_STORE "/pocket"

/* Each core runs one lane with its own renderer and PPA clients. One strip
 * renders while the other is presented; `idle` is given when a strip's
 * presentation completes. */
typedef struct {
  pocketjs_rgb565_renderer_t *renderer;
  const pocketjs_rgb565_accelerator_t *accelerator;
  ppa_client_handle_t present;
  uint16_t *strips[2];
  SemaphoreHandle_t idle[2];
  uint32_t next_strip;
  esp_err_t result;
} tab5_lane_t;

typedef struct {
  esp_lcd_touch_handle_t touch;
  pocketjs_rgb565_target_t *target;
  uint16_t *framebuffer;
  tab5_lane_t lanes[2];
  /* One DMA channel presents both lanes' full-width strips: the chip has
   * three AXI channel pairs, and the third stays free for the SPI bus. */
  async_memcpy_handle_t copy;
  /* Strips of the frame being drawn; both lanes claim them in order. */
  const pocketjs_ui_frame_view_t *frame;
  pocketjs_rgb565_rect_t strips[TAB5_MAX_STRIPS];
  uint32_t strip_count;
  atomic_uint next_strip;
  uint32_t strip_rows;
  /* The bytes of one strip buffer: strip_rows logical rows. */
  size_t strip_bytes;
  TaskHandle_t helper;
  TaskHandle_t runner;
  /* Moves retained framebuffer pixels when a plan carries a motion: whole
   * rows through DMA channels, blocks through a PPA client. The helper moves
   * while the first lane renders; strips present after `TAB5_MOVED_BIT`. */
  ppa_client_handle_t mover;
  async_memcpy_handle_t mover_copy;
  SemaphoreHandle_t moved;
  SemaphoreHandle_t written;
  EventGroupHandle_t move_state;
  pocketjs_rgb565_motion_t motion;
  esp_err_t move_result;
  /* Landscape pictures turn 90 degrees counter-clockwise onto the 720x1280
   * portrait panel; portrait pictures keep their orientation. */
  bool landscape;
  /* Panel pixels per raster pixel: 1 when the raster density fills the
   * panel, 2 for an integer-fit picture rendered at half the density. */
  uint32_t scale;
  /* The picture's size and top-left corner on the screen as the picture is
   * oriented (1280x720 for landscape), in panel pixels. A picture smaller
   * than the screen sits in the top-left corner, with the home button in the
   * space left below or right of it. */
  uint32_t picture_width;
  uint32_t picture_height;
  uint32_t origin_x;
  uint32_t origin_y;
  uint16_t *button;
  bool button_shown;
  bool button_pressed;
  bool button_released;
  int64_t frame_started;
  uint64_t frame_us;
  uint32_t frame_max_us;
  /* Launcher mode: the task that switches guests, woken by the runner. */
  TaskHandle_t app_task;
  bool switch_signalled;
} tab5_host_t;

static uint32_t oriented_height(const tab5_host_t *host) {
  return host->landscape ? BSP_LCD_H_RES : BSP_LCD_V_RES;
}

/* The button's top-left corner on the oriented screen: centered in the space
 * below the picture, or right of it when the picture is as tall as the
 * screen. */
static void home_button_origin(const tab5_host_t *host, int32_t *x, int32_t *y) {
  const uint32_t screen_width = host->landscape ? BSP_LCD_V_RES : BSP_LCD_H_RES;
  const uint32_t bottom = host->origin_y + host->picture_height;
  if (bottom < oriented_height(host)) {
    *x = (int32_t)((screen_width - TAB5_BUTTON_SIZE) / 2U);
    *y = (int32_t)(bottom + (oriented_height(host) - bottom - TAB5_BUTTON_SIZE) / 2U);
    return;
  }
  const uint32_t right = host->origin_x + host->picture_width;
  *x = (int32_t)(right + (screen_width - right - TAB5_BUTTON_SIZE) / 2U);
  *y = (int32_t)((oriented_height(host) - TAB5_BUTTON_SIZE) / 2U);
}

/* The whole space below (or beside) the picture answers, so a thumb need
 * not aim. */
static bool on_home_button(const tab5_host_t *host, int32_t x, int32_t y) {
  if (host->origin_y + host->picture_height < oriented_height(host))
    return y >= (int32_t)(host->origin_y + host->picture_height);
  return x >= (int32_t)(host->origin_x + host->picture_width);
}

static uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Coverage of one pixel by the button's disc and by the rounded square
 * drawn on it, from 4x4 samples. */
static void button_coverage(float px, float py, float *disc, float *mark) {
  const float center = TAB5_BUTTON_SIZE / 2.0f;
  const float radius = 40.0f;
  const float half = 13.0f;
  const float corner = 5.5f;
  const float stroke = 2.5f;
  uint32_t in_disc = 0;
  uint32_t in_mark = 0;
  for (uint32_t sy = 0; sy < 4U; ++sy) {
    for (uint32_t sx = 0; sx < 4U; ++sx) {
      const float x = px + (sx + 0.5f) / 4.0f - center;
      const float y = py + (sy + 0.5f) / 4.0f - center;
      if (x * x + y * y <= radius * radius)
        ++in_disc;
      /* Signed distance to the rounded square's outline. */
      const float qx = fabsf(x) - (half - corner);
      const float qy = fabsf(y) - (half - corner);
      const float ox = qx > 0.0f ? qx : 0.0f;
      const float oy = qy > 0.0f ? qy : 0.0f;
      const float inside = fminf(fmaxf(qx, qy), 0.0f);
      const float distance = sqrtf(ox * ox + oy * oy) + inside - corner;
      if (fabsf(distance) <= stroke / 2.0f)
        ++in_mark;
    }
  }
  *disc = in_disc / 16.0f;
  *mark = in_mark / 16.0f;
}

/* Paint the home button in its pressed or resting state. The pixels go
 * through the mover's PPA client, which turns them onto the portrait panel
 * like a strip in landscape. */
static void paint_home_button(tab5_host_t *host) {
  const uint32_t size = TAB5_BUTTON_SIZE;
  const float face = host->button_pressed ? 70.0f : 30.0f;
  for (uint32_t y = 0; y < size; ++y) {
    for (uint32_t x = 0; x < size; ++x) {
      float disc;
      float mark;
      button_coverage((float)x, (float)y, &disc, &mark);
      const float level = disc * face + mark * (142.0f - face);
      host->button[y * size + x] = rgb565((uint32_t)level, (uint32_t)level,
                                          (uint32_t)(level * 1.03f));
    }
  }
  const size_t bytes = size * size * sizeof(uint16_t);
  esp_cache_msync(host->button, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  int32_t left;
  int32_t top;
  home_button_origin(host, &left, &top);
  ppa_srm_oper_config_t operation = {
      .in = {.buffer = host->button, .pic_w = size, .pic_h = size,
             .block_w = size, .block_h = size,
             .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
      .out = {.buffer = host->framebuffer,
              .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
              .pic_w = BSP_LCD_H_RES, .pic_h = BSP_LCD_V_RES,
              .block_offset_x = (uint32_t)left, .block_offset_y = (uint32_t)top,
              .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
      .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
      .scale_x = 1.0f,
      .scale_y = 1.0f,
      .mode = PPA_TRANS_MODE_BLOCKING,
  };
  if (host->landscape) {
    operation.out.block_offset_x = (uint32_t)top;
    operation.out.block_offset_y = BSP_LCD_V_RES - (uint32_t)left - size;
    operation.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
  }
  ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(host->mover, &operation));
}

static esp_err_t sample_input(pocketjs_ui_input_t *input, void *user_data) {
  static pocketjs_ui_touch_t touches[POCKETJS_UI_MAX_TOUCHES];
  tab5_host_t *host = user_data;
  host->frame_started = esp_timer_get_time();
  ESP_RETURN_ON_ERROR(esp_lcd_touch_read_data(host->touch), TAG, "touch read");
  esp_lcd_touch_point_data_t points[POCKETJS_UI_MAX_TOUCHES];
  uint8_t count = 0;
  ESP_RETURN_ON_ERROR(esp_lcd_touch_get_data(host->touch, points, &count,
                                             POCKETJS_UI_MAX_TOUCHES),
                      TAG, "touch data");
  const pocketjs_package_host_contract_t *contract =
      &pocketjs_package_app_contract;
  const uint32_t logical_width = contract->auxiliary_width != 0U
                                     ? contract->auxiliary_width
                                     : contract->logical_width;
  const uint32_t logical_height = contract->auxiliary_width != 0U
                                      ? contract->auxiliary_height
                                      : contract->logical_height;
  bool on_button = false;
  input->touch_count = 0;
  for (uint8_t index = 0; index < count; ++index) {
    const int32_t screen_x = host->landscape
                                 ? (int32_t)BSP_LCD_V_RES - 1 - points[index].y
                                 : points[index].x;
    const int32_t screen_y = host->landscape ? points[index].x : points[index].y;
    const int32_t x = screen_x - (int32_t)host->origin_x;
    const int32_t y = screen_y - (int32_t)host->origin_y;
    if (x < 0 || y < 0 || x >= (int32_t)host->picture_width ||
        y >= (int32_t)host->picture_height) {
      on_button |= host->button_shown && on_home_button(host, screen_x, screen_y);
      continue;
    }
    touches[input->touch_count++] = (pocketjs_ui_touch_t){
        .id = points[index].track_id,
        .x = (uint32_t)x * logical_width / host->picture_width,
        .y = (uint32_t)y * logical_height / host->picture_height,
    };
  }
  input->touches = touches;
  /* The button acts on release, like a key: one frame of SELECT. Outside
   * the launcher it summons the launcher instead. */
  input->buttons = host->button_released ? TAB5_BUTTON_SELECT : 0U;
  if (TAB5_LAUNCHER) {
    pocketjs_system_pump();
    if (input->buttons != 0U && !pocketjs_system_launcher_running()) {
      pocketjs_system_summon();
      input->buttons = 0U;
    }
    if (pocketjs_system_switch_pending() && !host->switch_signalled) {
      host->switch_signalled = true;
      xTaskNotifyGive(host->app_task);
    }
  }
  host->button_released = host->button_pressed && !on_button;
  if (on_button != host->button_pressed) {
    host->button_pressed = on_button;
    paint_home_button(host);
  }
  return ESP_OK;
}

static bool strip_presented(SemaphoreHandle_t idle) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(idle, &woken);
  return woken == pdTRUE;
}

static bool on_copy_done(async_memcpy_handle_t copy,
                         async_memcpy_event_t *event, void *user_data) {
  return strip_presented(user_data);
}

static bool on_srm_done(ppa_client_handle_t client, ppa_event_data_t *event,
                        void *user_data) {
  return strip_presented(user_data);
}

static bool on_move_copied(async_memcpy_handle_t copy,
                           async_memcpy_event_t *event, void *user_data) {
  return strip_presented(user_data);
}

/* Move full panel rows y0..y1 by dy in place. Copies run in queue order: a
 * move up copies top-down, every write landing on rows already read; a move
 * down copies bottom-up in chunks no taller than dy, so no chunk overwrites
 * a row not yet read. */
static esp_err_t copy_rows(tab5_host_t *host, uint32_t y0, uint32_t y1,
                           int32_t dy) {
  const size_t row_bytes = BSP_LCD_H_RES * sizeof(uint16_t);
  const uint32_t chunk =
      dy > 0 && (uint32_t)dy < TAB5_MOVE_ROWS ? (uint32_t)dy : TAB5_MOVE_ROWS;
  uint16_t *framebuffer = host->framebuffer;
  esp_err_t result = ESP_OK;
  uint32_t queued = 0;
  for (uint32_t done = 0; done < y1 - y0 && result == ESP_OK;) {
    const uint32_t rows = y1 - y0 - done < chunk ? y1 - y0 - done : chunk;
    const uint32_t y = dy > 0 ? y1 - done - rows : y0 + done;
    if (queued == TAB5_MOVE_BACKLOG) {
      xSemaphoreTake(host->moved, portMAX_DELAY);
      --queued;
    }
    result = esp_async_memcpy(host->mover_copy,
                              framebuffer + (size_t)y * BSP_LCD_H_RES,
                              framebuffer + (size_t)(y - dy) * BSP_LCD_H_RES,
                              rows * row_bytes, on_move_copied, host->moved);
    if (result == ESP_OK)
      ++queued;
    done += rows;
  }
  for (; queued > 0U; --queued)
    xSemaphoreTake(host->moved, portMAX_DELAY);
  return result;
}

/* Move full panel rows y0..y1 down by a short dy > 0, bottom-up in bands.
 * A band's source overlaps no band written before it, so each band is read
 * into one of the helper lane's strips and then written. The mover's
 * channel reads the next band while the lane's channel writes this one. */
static esp_err_t copy_rows_down(tab5_host_t *host, uint32_t y0, uint32_t y1,
                                uint32_t dy) {
  const size_t row_bytes = BSP_LCD_H_RES * sizeof(uint16_t);
  uint16_t *framebuffer = host->framebuffer;
  async_memcpy_handle_t writer = host->copy;
  const uint32_t band_rows = host->strip_bytes / row_bytes;
  const uint32_t count = (y1 - y0 + band_rows - 1U) / band_rows;
  uint32_t reads = 0;
  uint32_t writes = 0;
  esp_err_t result = ESP_OK;
  for (uint32_t band = 0; band < count && result == ESP_OK; ++band) {
    const uint32_t top =
        y1 > (band + 1U) * band_rows + y0 ? y1 - (band + 1U) * band_rows : y0;
    const uint32_t rows = y1 - band * band_rows - top;
    uint16_t *strip = host->lanes[1].strips[band & 1U];
    /* The strip is free once the band two before it was written. */
    for (; writes + 2U <= band; ++writes)
      xSemaphoreTake(host->written, portMAX_DELAY);
    result = esp_async_memcpy(host->mover_copy, strip,
                              framebuffer + (size_t)(top - dy) * BSP_LCD_H_RES,
                              rows * row_bytes, on_move_copied, host->moved);
    if (result != ESP_OK)
      break;
    ++reads;
    xSemaphoreTake(host->moved, portMAX_DELAY);
    result = esp_async_memcpy(writer, framebuffer + (size_t)top * BSP_LCD_H_RES,
                              strip, rows * row_bytes, on_move_copied,
                              host->written);
    if (result != ESP_OK)
      break;
  }
  for (; writes < reads; ++writes)
    xSemaphoreTake(host->written, portMAX_DELAY);
  return result;
}

/* Synchronous 2D copy between the framebuffer and a strip-sized buffer. */
static esp_err_t copy_block(tab5_host_t *host, void *to, uint32_t to_width,
                            uint32_t to_x, uint32_t to_y, const void *from,
                            uint32_t from_width, uint32_t from_x,
                            uint32_t from_y, uint32_t w, uint32_t h,
                            size_t to_bytes, uint32_t to_height,
                            uint32_t from_height) {
  const ppa_srm_oper_config_t operation = {
      .in = {.buffer = from, .pic_w = from_width, .pic_h = from_height,
             .block_w = w, .block_h = h, .block_offset_x = from_x,
             .block_offset_y = from_y, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
      .out = {.buffer = to, .buffer_size = to_bytes, .pic_w = to_width,
              .pic_h = to_height, .block_offset_x = to_x,
              .block_offset_y = to_y, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
      .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
      .scale_x = 1.0f,
      .scale_y = 1.0f,
      .mode = PPA_TRANS_MODE_BLOCKING,
  };
  return ppa_do_scale_rotate_mirror(host->mover, &operation);
}

/* Move a motion's pixels inside the panel framebuffer before any strip is
 * presented. Full-width vertical moves go through the DMA channels. Other
 * moves bounce bands through a strip of the helper's lane, which is idle
 * while it moves, with the PPA, reading a band before writing it, in the
 * order that keeps unread source rows. */
static esp_err_t move_pixels(tab5_host_t *host,
                             const pocketjs_ui_frame_view_t *frame,
                             const pocketjs_rgb565_motion_t *motion) {
  const uint32_t unit = frame->raster_density * host->scale;
  uint32_t x0 = host->origin_x + motion->destination.x * unit;
  uint32_t y0 = host->origin_y + motion->destination.y * unit;
  uint32_t x1 = x0 + motion->destination.width * unit;
  uint32_t y1 = y0 + motion->destination.height * unit;
  int32_t dx = motion->dx * (int32_t)unit;
  int32_t dy = motion->dy * (int32_t)unit;
  if (host->landscape) {
    /* Screen (x, y) is panel (y, 1280 - 1 - x). */
    const uint32_t px0 = y0, px1 = y1;
    y0 = BSP_LCD_V_RES - x1;
    y1 = BSP_LCD_V_RES - x0;
    x0 = px0;
    x1 = px1;
    const int32_t picture_dx = dx;
    dx = dy;
    dy = -picture_dx;
  }
  if (x0 == 0 && x1 == BSP_LCD_H_RES && dx == 0) {
    return dy < 0 || (uint32_t)dy >= TAB5_MIN_DIRECT_ROWS
               ? copy_rows(host, y0, y1, dy)
               : copy_rows_down(host, y0, y1, (uint32_t)dy);
  }
  uint16_t *band = host->lanes[1].strips[0];
  const size_t band_bytes = host->strip_bytes;
  const uint32_t w = x1 - x0;
  const uint32_t band_rows = band_bytes / (w * sizeof(uint16_t));
  for (uint32_t done = 0; done < y1 - y0;) {
    const uint32_t rows = y1 - y0 - done < band_rows ? y1 - y0 - done : band_rows;
    const uint32_t y = dy > 0 ? y1 - done - rows : y0 + done;
    const uint32_t from_y = y - dy;
    const uint32_t from_x = x0 - dx;
    const size_t frame_bytes =
        BSP_LCD_V_RES * BSP_LCD_H_RES * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(copy_block(host, band, w, 0, 0, host->framebuffer,
                                   BSP_LCD_H_RES, from_x, from_y, w, rows,
                                   band_bytes, rows, BSP_LCD_V_RES),
                        TAG, "move read");
    ESP_RETURN_ON_ERROR(copy_block(host, host->framebuffer, BSP_LCD_H_RES, x0,
                                   y, band, w, 0, 0, w, rows, frame_bytes,
                                   BSP_LCD_V_RES, rows),
                        TAG, "move write");
    done += rows;
  }
  return ESP_OK;
}

/* Wait until no strip of the lane is being presented. */
static void wait_presented(tab5_lane_t *lane) {
  for (size_t index = 0; index < 2U; ++index) {
    xSemaphoreTake(lane->idle[index], portMAX_DELAY);
    xSemaphoreGive(lane->idle[index]);
  }
}

static esp_err_t present_strip(tab5_host_t *host, tab5_lane_t *lane,
                               const pocketjs_ui_frame_view_t *frame,
                               pocketjs_rgb565_rect_t rect, uint32_t index) {
  const uint32_t density = frame->raster_density;
  const uint32_t width = frame->logical_width * density;
  const uint32_t x = rect.x * density;
  const uint32_t y = rect.y * density;
  const uint32_t w = rect.width * density;
  const uint32_t h = rect.height * density;
  const uint32_t scale = host->scale;
  if (!host->landscape && scale == 1U && w == width &&
      width == BSP_LCD_H_RES) {
    /* A full-width portrait strip is one contiguous run of panel rows. */
    return esp_async_memcpy(host->copy,
                            host->framebuffer + (host->origin_y + y) * width,
                            lane->strips[index],
                            (size_t)w * h * sizeof(uint16_t), on_copy_done,
                            lane->idle[index]);
  }
  ppa_srm_oper_config_t operation = {
      .in =
          {
              .buffer = lane->strips[index],
              .pic_w = width,
              .pic_h = h,
              .block_w = w,
              .block_h = h,
              .block_offset_x = x,
              .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
          },
      .out =
          {
              .buffer = host->framebuffer,
              .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
              .pic_w = BSP_LCD_H_RES,
              .pic_h = BSP_LCD_V_RES,
              .block_offset_x = host->origin_x + x * scale,
              .block_offset_y = host->origin_y + y * scale,
              .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
          },
      .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
      .scale_x = (float)scale,
      .scale_y = (float)scale,
      .mode = PPA_TRANS_MODE_NON_BLOCKING,
      .user_data = lane->idle[index],
  };
  if (host->landscape) {
    /* Screen (x, y) lands on panel (y, 1280 - 1 - x). */
    operation.out.block_offset_x = host->origin_y + y * scale;
    operation.out.block_offset_y =
        BSP_LCD_V_RES - host->origin_x - (x + w) * scale;
    operation.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
  }
  return ppa_do_scale_rotate_mirror(lane->present, &operation);
}

static esp_err_t run_lane(tab5_host_t *host, tab5_lane_t *lane) {
  const pocketjs_ui_frame_view_t *frame = host->frame;
  const size_t row_pixels = (size_t)frame->logical_width *
                            frame->raster_density * frame->raster_density;
  for (;;) {
    const uint32_t next = atomic_fetch_add(&host->next_strip, 1U);
    if (next >= host->strip_count)
      return ESP_OK;
    const pocketjs_rgb565_rect_t rect = host->strips[next];
    const uint32_t index = lane->next_strip;
    lane->next_strip ^= 1U;
    xSemaphoreTake(lane->idle[index], portMAX_DELAY);
    pocketjs_rgb565_render_stats_t stats = {.struct_size = sizeof(stats)};
    esp_err_t result = pocketjs_rgb565_render_strip(
        lane->renderer, frame, lane->strips[index], row_pixels * rect.height,
        rect, lane->accelerator, &stats);
    /* A strip may cover rows the motion still has to read. */
    xEventGroupWaitBits(host->move_state, TAB5_MOVED_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);
    if (result == ESP_OK)
      result = present_strip(host, lane, frame, rect, index);
    if (result != ESP_OK) {
      xSemaphoreGive(lane->idle[index]);
      atomic_store(&host->next_strip, host->strip_count);
      return result;
    }
  }
}

/* Move the frame's motion, if any, and open presentation. */
static void move_frame(tab5_host_t *host) {
  if ((xEventGroupGetBits(host->move_state) & TAB5_MOVED_BIT) != 0U)
    return;
  host->move_result = move_pixels(host, host->frame, &host->motion);
  xEventGroupSetBits(host->move_state, TAB5_MOVED_BIT);
}

static void helper_task(void *user_data) {
  tab5_host_t *host = user_data;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    move_frame(host);
    host->lanes[1].result = run_lane(host, &host->lanes[1]);
    xTaskNotifyGive(host->runner);
  }
}

static esp_err_t after_turn(const pocketjs_ui_frame_view_t *frame,
                            void *user_data) {
  tab5_host_t *host = user_data;
  pocketjs_rgb565_renderer_t *renderer = host->lanes[0].renderer;
  pocketjs_rgb565_damage_plan_t plan = {.struct_size = sizeof(plan)};
  pocketjs_rgb565_motion_t motion = {.struct_size = sizeof(motion)};
  ESP_RETURN_ON_ERROR(pocketjs_rgb565_prepare_motion(renderer, host->target,
                                                     frame, &plan, &motion),
                      TAG, "prepare");
  host->frame = frame;
  host->motion = motion;
  host->move_result = ESP_OK;
  if (motion.present) {
    xEventGroupClearBits(host->move_state, TAB5_MOVED_BIT);
  } else {
    xEventGroupSetBits(host->move_state, TAB5_MOVED_BIT);
  }
  host->strip_count = 0;
  for (uint32_t index = 0; index < plan.region_count; ++index) {
    pocketjs_rgb565_rect_t region = plan.regions[index];
    /* PPA output buffers must span whole 128-byte cache lines: an even
     * number of logical rows keeps a portrait strip's size a multiple of
     * 128 bytes. Repainting one more row is exact. */
    if ((region.height & 1U) != 0U) {
      if (region.y + region.height < frame->logical_height)
        ++region.height;
      else if (region.y > 0U) {
        --region.y;
        ++region.height;
      }
    }
    const uint32_t turned_rows =
        (TAB5_MIN_TURNED_ROWS + frame->raster_density - 1U) /
        frame->raster_density;
    if (host->landscape && region.height < turned_rows) {
      if (region.y + turned_rows > frame->logical_height)
        region.y = frame->logical_height - turned_rows;
      region.height = turned_rows;
    }
    /* Landscape strips share the region's rows evenly, so the last one is
     * not a short remainder; a landscape logical row is a whole number of
     * cache lines, so any count is aligned. */
    const uint32_t count =
        (region.height + host->strip_rows - 1U) / host->strip_rows;
    uint32_t y = region.y;
    for (uint32_t strip = 0; strip < count; ++strip) {
      pocketjs_rgb565_rect_t rect = region;
      rect.y = y;
      rect.height =
          host->landscape
              ? region.height / count + (strip < region.height % count ? 1U : 0U)
              : (region.y + region.height - y < host->strip_rows
                     ? region.y + region.height - y
                     : host->strip_rows);
      host->strips[host->strip_count++] = rect;
      y += rect.height;
    }
  }
  esp_err_t result = ESP_OK;
  if (host->strip_count > 0U) {
    host->runner = xTaskGetCurrentTaskHandle();
    atomic_store(&host->next_strip, 0U);
    xTaskNotifyGive(host->helper);
    result = run_lane(host, &host->lanes[0]);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    wait_presented(&host->lanes[0]);
    wait_presented(&host->lanes[1]);
    if (result == ESP_OK)
      result = host->lanes[1].result;
  } else {
    move_frame(host);
  }
  if (result == ESP_OK)
    result = host->move_result;
  if (result != ESP_OK) {
    pocketjs_rgb565_abort(renderer, host->target);
    return result;
  }
  const uint32_t elapsed =
      (uint32_t)(esp_timer_get_time() - host->frame_started);
  host->frame_us += elapsed;
  if (elapsed > host->frame_max_us)
    host->frame_max_us = elapsed;
  return pocketjs_rgb565_commit(renderer, host->target, frame);
}

static void start_panel(tab5_host_t *host) {
  const bsp_display_config_t display_config = {
      .dsi_bus =
          {
              .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
              .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
          },
  };
  bsp_lcd_handles_t lcd;
  ESP_ERROR_CHECK(bsp_display_new_with_handles(&display_config, &lcd));
  void *framebuffer = NULL;
  ESP_ERROR_CHECK(
      esp_lcd_dpi_panel_get_frame_buffer(lcd.panel, 1, &framebuffer));
  host->framebuffer = framebuffer;
  /* Space beside a picture narrower than the screen stays black. */
  const size_t frame_bytes = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t);
  memset(framebuffer, 0, frame_bytes);
  esp_cache_msync(framebuffer, frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd.panel, true));
  ESP_ERROR_CHECK(bsp_display_backlight_on());
  ESP_ERROR_CHECK(bsp_touch_new(NULL, &host->touch));
}

static void start_lane(tab5_lane_t *lane, uint32_t scale, size_t strip_bytes) {
  pocketjs_rgb565_renderer_config_t renderer_config;
  pocketjs_rgb565_renderer_config_defaults(&renderer_config);
  renderer_config.scale = scale;
  ESP_ERROR_CHECK(
      pocketjs_rgb565_renderer_create(&renderer_config, &lane->renderer));
  pocketjs_esp32p4_ppa_t *ppa = NULL;
  ESP_ERROR_CHECK(pocketjs_esp32p4_ppa_create(&ppa));
  lane->accelerator = pocketjs_esp32p4_ppa_accelerator(ppa);
  const ppa_client_config_t present_config = {
      .oper_type = PPA_OPERATION_SRM,
      .max_pending_trans_num = 2,
  };
  ESP_ERROR_CHECK(ppa_register_client(&present_config, &lane->present));
  const ppa_event_callbacks_t present_callbacks = {
      .on_trans_done = on_srm_done,
  };
  ESP_ERROR_CHECK(
      ppa_client_register_event_callbacks(lane->present, &present_callbacks));
  for (size_t index = 0; index < 2U; ++index) {
    lane->strips[index] = heap_caps_aligned_alloc(
        128, strip_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lane->idle[index] = xSemaphoreCreateBinary();
    assert(lane->strips[index] != NULL && lane->idle[index] != NULL);
    xSemaphoreGive(lane->idle[index]);
  }
}




/* One running guest: its QuickJS realm, UI core, binding and runner. */
typedef struct {
  pocketjs_guest_t *guest;
  pocketjs_ui_core_t *core;
  pocketjs_ui_qjs_t *binding;
  pocketjs_runner_t *runner;
} tab5_guest_t;

static void stop_guest(tab5_guest_t *running) {
  if (running->runner != NULL)
    ESP_ERROR_CHECK(pocketjs_runner_stop(running->runner));
  /* The guest goes before its mounted binding. */
  if (running->guest != NULL)
    pocketjs_guest_destroy(running->guest);
  if (running->binding != NULL)
    pocketjs_ui_qjs_destroy(running->binding);
  if (running->core != NULL)
    pocketjs_ui_core_destroy(running->core);
  *running = (tab5_guest_t){0};
}

/* Boot `app` and start its runner. `system_app` is the launcher's view of
 * it in launcher mode, NULL otherwise. */
static esp_err_t boot_guest(tab5_host_t *host,
                            const pocketjs_package_variant_t *app,
                            const pocketjs_system_app_t *system_app,
                            tab5_guest_t *out) {
  const pocketjs_package_host_contract_t *contract =
      &pocketjs_package_app_contract;
  tab5_guest_t running = {0};
  pocketjs_guest_config_t guest_config;
  pocketjs_guest_config_defaults(&guest_config);
  esp_err_t result = pocketjs_guest_create(&guest_config, &running.guest);
  pocketjs_ui_core_config_t core_config;
  pocketjs_ui_core_config_defaults(&core_config);
  core_config.logical_width = contract->logical_width;
  core_config.logical_height = contract->logical_height;
  core_config.raster_density = contract->raster_density;
  core_config.tick_hz = contract->tick_hz;
  if (result == ESP_OK)
    result = pocketjs_ui_core_create(&core_config, &running.core);
  const pocketjs_ui_qjs_config_t binding_config = {
      .struct_size = sizeof(binding_config),
      .target_id = contract->target_id,
      .host_abi = contract->host_abi,
      .auxiliary_width = contract->auxiliary_width,
      .auxiliary_height = contract->auxiliary_height,
  };
  if (result == ESP_OK)
    result = pocketjs_ui_qjs_create(running.guest, running.core,
                                    &binding_config, &running.binding);
  if (result == ESP_OK)
    result = pocketjs_ui_qjs_feed_pak(running.binding, app->pak.data,
                                      app->pak.size);
  if (result == ESP_OK)
    result = pocketjs_ui_qjs_mount(running.binding);
  if (result == ESP_OK && system_app != NULL)
    result = pocketjs_system_bind(running.guest, system_app);
  if (result == ESP_OK)
    result = pocketjs_guest_eval(running.guest,
                                 (const char *)app->javascript.data,
                                 app->javascript.size - 1U, "app");
  pocketjs_runner_config_t runner_config;
  pocketjs_runner_config_defaults(&runner_config);
  /* Solid's reactive graph recurses through the stack in a turn. */
  runner_config.task_stack_bytes = 64U * 1024U;
  runner_config.task_stack_caps = MALLOC_CAP_SPIRAM;
  runner_config.task_core = 0;
  runner_config.sample_input = sample_input;
  runner_config.after_turn = after_turn;
  runner_config.user_data = host;
  host->switch_signalled = false;
  if (result == ESP_OK)
    result = pocketjs_runner_start(running.binding, &runner_config,
                                   &running.runner);
  if (result != ESP_OK) {
    stop_guest(&running);
    if (system_app != NULL)
      pocketjs_system_unbind();
    return result;
  }
  *out = running;
  return ESP_OK;
}

/* The next guest draws on a picture its renderers have never seen: fresh
 * renderers (their caches key on the old core's texture handles) and a full
 * first frame. */
static void reset_renderers(tab5_host_t *host) {
  for (size_t index = 0; index < 2U; ++index) {
    tab5_lane_t *lane = &host->lanes[index];
    pocketjs_rgb565_renderer_destroy(lane->renderer);
    pocketjs_rgb565_renderer_config_t config;
    pocketjs_rgb565_renderer_config_defaults(&config);
    config.scale = pocketjs_package_app_contract.raster_density;
    ESP_ERROR_CHECK(pocketjs_rgb565_renderer_create(&config, &lane->renderer));
  }
  pocketjs_rgb565_target_invalidate(host->target);
}

/* Replace the running guest with the one the launcher asked for, or with
 * the launcher after `failed`. Returns the app now running. */
static pocketjs_system_app_t *switch_guest(tab5_host_t *host,
                                           tab5_guest_t *running,
                                           pocketjs_system_app_t *app,
                                           bool failed) {
  stop_guest(running);
  pocketjs_system_unbind();
  pocketjs_system_app_free(app);
  reset_renderers(host);
  char slot[17] = {0};
  bool launcher = true;
  if (failed || !pocketjs_system_take_switch(slot, &launcher))
    launcher = true;
  pocketjs_system_app_t *next = NULL;
  esp_err_t result = ESP_FAIL;
  if (!launcher) {
    result = pocketjs_system_load_slot(slot, &next);
    if (result == ESP_OK) {
      result = boot_guest(host, pocketjs_system_app_variant(next), next,
                          running);
      if (result != ESP_OK) {
        pocketjs_system_app_free(next);
        next = NULL;
      }
    }
    if (result != ESP_OK) {
      ESP_LOGE(TAG, "slot %s did not boot: %s", slot, esp_err_to_name(result));
      pocketjs_system_report_failure("the app did not start");
      reset_renderers(host);
    }
  }
  if (result != ESP_OK) {
    ESP_ERROR_CHECK(pocketjs_system_load_launcher(&next));
    ESP_ERROR_CHECK(boot_guest(host, pocketjs_system_app_variant(next), next,
                               running));
  }
  return next;
}

/* The ESP32-C6 that carries Wi-Fi: power it, then bring up the SDIO link
 * (esp_hosted starts here, not before app_main, so Hero and Clear never
 * start it and the render strips are allocated first). */
static esp_err_t start_coprocessor(void) {
  ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_WIFI, true), TAG,
                      "coprocessor power");
  if (esp_hosted_init() != 0 || esp_hosted_connect_to_slave() != 0)
    return ESP_FAIL;
  return ESP_OK;
}

/* Storage and the system services of launcher mode. */
/* The card's SPI bus takes an AXI GDMA pair, and spi_bus_initialize faults
 * in its cleanup instead of failing when none is free: mount only after a
 * probe finds one. */
static bool axi_dma_free(void) {
  const gdma_channel_alloc_config_t config = {0};
  gdma_channel_handle_t tx = NULL;
  gdma_channel_handle_t rx = NULL;
  if (gdma_new_axi_channel(&config, &tx, &rx) != ESP_OK)
    return false;
  gdma_del_channel(tx);
  gdma_del_channel(rx);
  return true;
}

static void start_system(void) {
  /* The SDMMC controller drives the Wi-Fi coprocessor (esp_hosted), so the
   * card is read over SPI. */
  bsp_sdcard_cfg_t card = {0};
  const bool on_card = axi_dma_free() && bsp_sdcard_sdspi_mount(&card) == ESP_OK;
  const char *root = on_card ? BSP_SD_MOUNT_POINT : TAB5_STORE;
  const esp_err_t result =
      on_card ? ESP_OK : pocketjs_system_mount_flash("storage", root);
  ESP_LOGI(TAG, "store: %s at %s (%s)",
           on_card ? "microSD card" : "flash partition", root,
           esp_err_to_name(result));
  ESP_ERROR_CHECK(result);
  const pocketjs_system_config_t config = {
      .struct_size = sizeof(config),
      .contract = &pocketjs_package_app_contract,
      .launcher = pocketjs_package_app.data,
      .launcher_size = pocketjs_package_app.size,
      .root = root,
      .device_name = "M5Stack Tab5",
      .device_model = "M5Stack Tab5",
      .wifi = true,
      .wifi_prepare = start_coprocessor,
  };
  ESP_ERROR_CHECK(pocketjs_system_start(&config));
}


static void run_app(void *unused) {
  static tab5_host_t host;
  const pocketjs_package_host_contract_t *contract =
      &pocketjs_package_app_contract;
  /* The picture is the auxiliary surface when the profile has one: the
   * primary surface stays laid out off screen. */
  const bool auxiliary = contract->auxiliary_width != 0U;
  const uint32_t picture_width =
      (auxiliary ? contract->auxiliary_width : contract->logical_width) *
      contract->raster_density;
  const uint32_t picture_height =
      (auxiliary ? contract->auxiliary_height : contract->logical_height) *
      contract->raster_density;
  host.landscape = picture_width > picture_height;
  const uint32_t screen_width = host.landscape ? BSP_LCD_V_RES : BSP_LCD_H_RES;
  const uint32_t screen_height = host.landscape ? BSP_LCD_H_RES : BSP_LCD_V_RES;
  host.scale = screen_width / picture_width < screen_height / picture_height
                   ? screen_width / picture_width
                   : screen_height / picture_height;
  assert(host.scale >= 1U);
  host.picture_width = picture_width * host.scale;
  host.picture_height = picture_height * host.scale;
  host.origin_x = 0U;
  host.origin_y = 0U;

  /* Strip bytes per logical row of the raster. */
  const size_t row_bytes =
      (size_t)picture_width * contract->raster_density * sizeof(uint16_t);
  size_t strip_budget = TAB5_STRIP_BYTES;
  if (TAB5_LAUNCHER) {
    /* The launcher's radio, store and link take internal RAM outside the
     * largest heap region: all four strips come from that region. */
    const size_t block = heap_caps_get_largest_free_block(
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (block / 4U - 128U < strip_budget)
      strip_budget = block / 4U - 128U;
  }
  host.strip_rows = strip_budget / row_bytes;
  host.strip_bytes = host.strip_rows * row_bytes;
  assert(8U * ((picture_height / contract->raster_density + host.strip_rows -
                1U) /
               host.strip_rows) <= TAB5_MAX_STRIPS);
  async_memcpy_config_t copy_config = ASYNC_MEMCPY_DEFAULT_CONFIG();
  copy_config.backlog = 4;
  copy_config.dma_burst_size = 64;
  ESP_ERROR_CHECK(esp_async_memcpy_install_gdma_axi(&copy_config, &host.copy));
  /* The four strips are the largest internal DMA blocks the firmware holds:
   * take them before the panel and the radio's buffers split the heap. */
  start_lane(&host.lanes[0], contract->raster_density, host.strip_bytes);
  start_lane(&host.lanes[1], contract->raster_density, host.strip_bytes);
  start_panel(&host);
  ESP_ERROR_CHECK(pocketjs_rgb565_target_create(&host.target));
  const ppa_client_config_t mover_config = {
      .oper_type = PPA_OPERATION_SRM,
      .max_pending_trans_num = 1,
  };
  ESP_ERROR_CHECK(ppa_register_client(&mover_config, &host.mover));
  async_memcpy_config_t mover_copy_config = ASYNC_MEMCPY_DEFAULT_CONFIG();
  mover_copy_config.backlog = TAB5_MOVE_BACKLOG;
  mover_copy_config.dma_burst_size = 64;
  ESP_ERROR_CHECK(
      esp_async_memcpy_install_gdma_axi(&mover_copy_config, &host.mover_copy));
  if (host.picture_width < screen_width || host.picture_height < screen_height) {
    host.button = heap_caps_aligned_alloc(
        128, TAB5_BUTTON_SIZE * TAB5_BUTTON_SIZE * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(host.button != NULL);
    host.button_shown = true;
    paint_home_button(&host);
  }
  host.moved = xSemaphoreCreateCounting(TAB5_MOVE_BACKLOG, 0);
  host.written = xSemaphoreCreateCounting(2, 0);
  host.move_state = xEventGroupCreate();
  assert(host.moved != NULL && host.written != NULL &&
         host.move_state != NULL);
  host.app_task = xTaskGetCurrentTaskHandle();
  /* The helper renders the second lane on core 1. */
  const BaseType_t created = xTaskCreatePinnedToCore(
      helper_task, "pocketjs_lane", 16U * 1024U, &host, 5, &host.helper, 1);
  assert(created == pdPASS);
  if (TAB5_LAUNCHER)
    start_system();

  tab5_guest_t running = {0};
  pocketjs_system_app_t *app = NULL;
  if (TAB5_LAUNCHER) {
    ESP_ERROR_CHECK(pocketjs_system_load_launcher(&app));
    ESP_ERROR_CHECK(boot_guest(&host, pocketjs_system_app_variant(app), app,
                               &running));
  } else {
    pocketjs_package_t *package = NULL;
    ESP_ERROR_CHECK(pocketjs_package_open(
        pocketjs_package_app.data, pocketjs_package_app.size, 0, &package));
    static pocketjs_package_variant_t embedded = {.struct_size =
                                                      sizeof(embedded)};
    ESP_ERROR_CHECK(pocketjs_package_select(package, contract, &embedded));
    ESP_ERROR_CHECK(boot_guest(&host, &embedded, NULL, &running));
  }

  pocketjs_runner_stats_t last = {.struct_size = sizeof(last)};
  int64_t logged = esp_timer_get_time();
  for (;;) {
    const bool woken = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) != 0U;
    pocketjs_runner_stats_t stats = {.struct_size = sizeof(stats)};
    ESP_ERROR_CHECK(pocketjs_runner_stats(running.runner, &stats));
    if (TAB5_LAUNCHER && (woken || (stats.frame_errors != last.frame_errors &&
                                    !pocketjs_system_app_is_launcher(app)))) {
      /* An app that throws from a frame goes back to the launcher, which
       * shows the failure once. */
      if (!woken)
        pocketjs_system_report_failure("the app stopped with an error");
      app = switch_guest(&host, &running, app, !woken);
      last = (pocketjs_runner_stats_t){.struct_size = sizeof(last)};
      continue;
    }
    if (esp_timer_get_time() - logged < 5000000)
      continue;
    const uint32_t frames = stats.frames - last.frames;
    ESP_LOGI(TAG,
             "fps=%.1f skipped=%" PRIu32 " errors=%" PRIu32
             " frame_avg_ms=%.2f frame_max_ms=%.2f",
             frames / 5.0, stats.frames_skipped - last.frames_skipped,
             stats.frame_errors - last.frame_errors,
             frames > 0U ? host.frame_us / 1000.0 / frames : 0.0,
             host.frame_max_us / 1000.0);
    host.frame_us = 0;
    host.frame_max_us = 0;
    last = stats;
    logged = esp_timer_get_time();
  }
}

void app_main(void) {
  /* Evaluating Clear needs about 100 KB of stack. A PSRAM stack leaves the
   * internal RAM to the four strips. */
  const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
      run_app, "pocketjs_app", 128U * 1024U, NULL, 1, NULL, 0,
      MALLOC_CAP_SPIRAM);
  assert(created == pdPASS);
}
