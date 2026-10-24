/* M5Stack Tab5: a PocketJS application on the 720x1280 MIPI-DSI panel. The
 * runtime renders the picture in strips with two PPA renderers, one per
 * core; the DPI display copies each strip into the panel's framebuffer,
 * turning a landscape picture onto the portrait panel.
 *
 * With TAB5_LAUNCHER the embedded application is Pocket Home: the firmware
 * keeps a store of installed apps (pocket_system), switches between them,
 * and draws a home button in the margin beside the picture that sends
 * SELECT (or summons the launcher from an app). */
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"
#include "esp_async_memcpy.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pocket/display_esp_dpi.h"
#include "pocket/guest_idf.h"
#include "pocket/package_source.h"
#include "pocket/runner.h"
#include "pocket_profile.h"
#include "pocket_spec.h"
#include "pocketjs/esp32p4_ppa_lanes.h"
#include "pocketjs/esp32p4_ppa_status.h"
#include "pocketjs_package_app.h"
#include "tab5_camera_media.h"

#ifndef TAB5_LAUNCHER
#define TAB5_LAUNCHER 0
#endif
#if TAB5_LAUNCHER
#include "esp_hosted.h"
#include "esp_private/gdma.h"
#include "pocket/system.h"
#endif

static const char *TAG = "pocketjs_tab5";

/* Staging bytes per strip at most; a strip holds the whole raster rows that
 * fit, in multiples of the display's row quantum. */
#define TAB5_STRIP_BYTES (128U * 1024U)
/* Copies queued on the move channel at once. */
#define TAB5_MOVE_BACKLOG 8U
/* Contacts the GT911 reports at once. */
#define TAB5_MAX_CONTACTS 5U
/* QuickJS heap of the guest. */
#define TAB5_GUEST_HEAP (24U * 1024U * 1024U)
/* The home button drawn in the margin right of (or below) a picture smaller
 * than the screen: at most a square this many screen pixels wide, 4 pixels
 * inside the margin on each side, and the SELECT bit it sends. */
#define TAB5_BUTTON_MAX 88U
#define TAB5_BUTTON_INSET 4U
/* Where the launcher keeps installed apps: the microSD card when one is in
 * the slot, the `storage` flash partition otherwise. */
#define TAB5_STORE "/pocket"

/* The primary surface of a profile whose picture is the auxiliary surface:
 * laid out off screen into one PSRAM buffer that nothing scans out. */
typedef struct {
  uint32_t width;
  uint32_t height;
  uint16_t *pixels;
  pocket_rect region;
  pocket_surface surface;
  uint64_t next;
} tab5_blank_t;

/* One running guest: its runtime, realm, renderers and runner. */
typedef struct {
  pocket_runtime *runtime;
  pocket_guest_idf *guest;
  pocket_runner_t *runner;
  pocket_render_esp_ppa *renderers[2];
  pocketjs_esp32p4_ppa_lanes_t *lanes;
  pocket_memory_package package;
} tab5_guest_t;

typedef struct {
  esp_lcd_touch_handle_t touch;
  uint16_t *framebuffer;
  pocket_display_esp_dpi *display;
  tab5_blank_t blank;
  pocketjs_esp32p4_ppa_t *devices[2];
  pocket_ppa_config ppa_config;
  size_t frame_buffer_budget;
  uint32_t quantum;
  /* Landscape pictures turn 90 degrees counter-clockwise onto the portrait
   * panel; portrait pictures keep their orientation. */
  bool turned;
  /* The surface the picture shows (0 primary, 1 auxiliary), and its size
   * and top-left corner on the screen as the picture is oriented, in panel
   * pixels. A picture smaller than the screen sits between two equal
   * margins. */
  uint32_t surface;
  uint32_t picture_width;
  uint32_t picture_height;
  uint32_t origin_x;
  uint32_t origin_y;
  /* Panel pixels per logical pixel. */
  float unit;
  /* The home button in the margin right of (or below) the picture. */
  uint32_t button_size;
  bool button_shown;
  bool button_pressed;
  bool button_released;
  uint32_t contacts[TAB5_MAX_CONTACTS];
  uint32_t contact_count;
  tab5_guest_t running;
  /* Launcher mode: the task that switches guests, woken by the runner. */
  TaskHandle_t app_task;
  bool switch_signalled;
  int64_t tick_started;
  uint64_t tick_us;
  uint32_t tick_max_us;
} tab5_host_t;

static uint32_t screen_width(const tab5_host_t *host) {
  return host->turned ? BSP_LCD_V_RES : BSP_LCD_H_RES;
}

static uint32_t screen_height(const tab5_host_t *host) {
  return host->turned ? BSP_LCD_H_RES : BSP_LCD_V_RES;
}

/* ---- the home button ---------------------------------------------------- */

/* The button's top-left corner on the oriented screen: centered in the
 * margin right of the picture, or below it when the picture is as wide as
 * the screen. */
static void home_button_origin(const tab5_host_t *host, uint32_t *x, uint32_t *y) {
  const uint32_t size = host->button_size;
  const uint32_t right = host->origin_x + host->picture_width;
  if (right < screen_width(host)) {
    *x = right + (screen_width(host) - right - size) / 2U;
    *y = (screen_height(host) - size) / 2U;
    return;
  }
  const uint32_t bottom = host->origin_y + host->picture_height;
  *x = (screen_width(host) - size) / 2U;
  *y = bottom + (screen_height(host) - bottom - size) / 2U;
}

/* The whole margin right of (or below) the picture answers, so a thumb need
 * not aim; the margin left of (or above) it does not. */
static bool on_home_button(const tab5_host_t *host, int32_t x, int32_t y) {
  if (host->origin_x + host->picture_width < screen_width(host))
    return x >= (int32_t)(host->origin_x + host->picture_width);
  return y >= (int32_t)(host->origin_y + host->picture_height);
}

static uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Coverage of one pixel by the button's disc and by the rounded square
 * drawn on it, from 4x4 samples. The proportions are those of the 88-pixel
 * button; the stroke stays at least 1.5 pixels wide. */
static void button_coverage(uint32_t size, float px, float py, float *disc,
                            float *mark) {
  const float unit = size / (float)TAB5_BUTTON_MAX;
  const float center = size / 2.0f;
  const float radius = 40.0f * unit;
  const float half = 13.0f * unit;
  const float corner = 5.5f * unit;
  const float stroke = fmaxf(2.5f * unit, 1.5f);
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

/* Paint the home button in its pressed or resting state straight into the
 * framebuffer: screen (x, y) of a turned picture is panel (y, 1279 - x). */
static void paint_home_button(tab5_host_t *host) {
  const uint32_t size = host->button_size;
  const float face = host->button_pressed ? 70.0f : 30.0f;
  uint32_t left;
  uint32_t top;
  home_button_origin(host, &left, &top);
  for (uint32_t y = 0; y < size; ++y) {
    for (uint32_t x = 0; x < size; ++x) {
      float disc;
      float mark;
      button_coverage(size, (float)x, (float)y, &disc, &mark);
      const float level = disc * face + mark * (142.0f - face);
      const uint16_t pixel = rgb565((uint32_t)level, (uint32_t)level,
                                    (uint32_t)(level * 1.03f));
      const uint32_t sx = left + x;
      const uint32_t sy = top + y;
      const uint32_t px = host->turned ? sy : sx;
      const uint32_t py = host->turned ? BSP_LCD_V_RES - 1U - sx : sy;
      host->framebuffer[py * BSP_LCD_H_RES + px] = pixel;
    }
  }
  /* The rows the button touches, whole cache lines. */
  const uint32_t first = host->turned ? BSP_LCD_V_RES - left - size : top;
  const size_t offset = (size_t)first * BSP_LCD_H_RES * sizeof(uint16_t);
  const size_t bytes = (size_t)size * BSP_LCD_H_RES * sizeof(uint16_t);
  esp_cache_msync((uint8_t *)host->framebuffer + (offset & ~(size_t)127U),
                  (bytes + 255U) & ~(size_t)127U,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/* ---- input --------------------------------------------------------------- */

static uint32_t input_classes(void *context) {
  (void)context;
  return POCKET_INPUT_CLASS_CONTACT |
         (TAB5_LAUNCHER ? POCKET_INPUT_CLASS_BUTTONS : 0U);
}

/* Contacts on the picture in logical pixels; a contact in the margin presses
 * the home button, which sends SELECT on release. A failed read cancels the
 * contacts the previous sample reported. */
static int32_t input_poll(void *context, pocket_input_frame_builder *builder) {
  tab5_host_t *host = context;
  if (!host || !builder || builder->struct_size < sizeof(*builder) ||
      !builder->append)
    return POCKET_STATUS_INVALID;
  esp_lcd_touch_point_data_t points[TAB5_MAX_CONTACTS];
  uint8_t count = 0;
  const bool read =
      esp_lcd_touch_read_data(host->touch) == ESP_OK &&
      esp_lcd_touch_get_data(host->touch, points, &count, TAB5_MAX_CONTACTS) ==
          ESP_OK;
  pocket_contact contacts[TAB5_MAX_CONTACTS];
  pocket_cancelled_contact cancelled[TAB5_MAX_CONTACTS];
  size_t contact_count = 0;
  size_t cancelled_count = 0;
  bool on_button = false;
  if (!read) {
    for (uint32_t i = 0; i < host->contact_count; i++)
      cancelled[cancelled_count++] = (pocket_cancelled_contact){
          .struct_size = sizeof(pocket_cancelled_contact),
          .surface = host->surface,
          .id = host->contacts[i]};
    count = 0;
  }
  for (uint8_t i = 0; i < count; i++) {
    const int32_t sx = host->turned ? (int32_t)BSP_LCD_V_RES - 1 - points[i].y
                                    : points[i].x;
    const int32_t sy = host->turned ? points[i].x : points[i].y;
    const int32_t x = sx - (int32_t)host->origin_x;
    const int32_t y = sy - (int32_t)host->origin_y;
    if (x < 0 || y < 0 || x >= (int32_t)host->picture_width ||
        y >= (int32_t)host->picture_height) {
      on_button |= host->button_shown && on_home_button(host, sx, sy);
      continue;
    }
    contacts[contact_count++] = (pocket_contact){
        .struct_size = sizeof(pocket_contact),
        .surface = host->surface,
        .id = points[i].track_id,
        .x = (float)x / host->unit,
        .y = (float)y / host->unit};
  }
  /* The button acts on release, like a key: one tick of SELECT. Outside
   * the launcher it summons the launcher instead. */
  uint32_t buttons = host->button_released ? POCKET_BUTTON_SELECT : 0U;
#if TAB5_LAUNCHER
  if (buttons != 0U && !pocketjs_system_launcher_running()) {
    pocketjs_system_summon();
    buttons = 0U;
  }
#endif
  host->button_released = host->button_pressed && !on_button;
  if (on_button != host->button_pressed) {
    host->button_pressed = on_button;
    paint_home_button(host);
  }
  const pocket_input_frame frame = {
      .struct_size = sizeof(frame),
      .buttons = buttons,
      .contacts = contact_count ? contacts : NULL,
      .contact_count = contact_count,
      .cancelled = cancelled_count ? cancelled : NULL,
      .cancelled_count = cancelled_count};
  const int32_t result = builder->append(builder->context, &frame);
  if (result == POCKET_STATUS_OK) {
    host->contact_count = contact_count;
    for (size_t i = 0; i < contact_count; i++)
      host->contacts[i] = contacts[i].id;
  }
  return result;
}

static const pocket_input_source_v1 INPUT = {.struct_size = sizeof(INPUT),
                                             .classes = input_classes,
                                             .poll = input_poll};

/* ---- the off-screen primary surface -------------------------------------- */

static int32_t blank_descriptor(void *context, pocket_display_descriptor *out) {
  tab5_blank_t *blank = context;
  if (!blank || !out || out->struct_size < sizeof(*out))
    return POCKET_STATUS_INVALID;
  *out = (pocket_display_descriptor){
      .struct_size = sizeof(*out),
      .role = POCKET_DISPLAY_ROLE_PRIMARY,
      .width = blank->width,
      .height = blank->height,
      .pixel_format = POCKET_PIXEL_FORMAT_RGB565,
      .stride_bytes = (size_t)blank->width * 2U,
      .target_kind = POCKET_TARGET_CPU_VISIBLE,
      .flags = POCKET_DISPLAY_FRAME_STORAGE | POCKET_DISPLAY_PARTIAL_REFRESH,
      .refresh_width = 1,
      .refresh_height = 1,
      .window_x_alignment = 1,
      .window_y_alignment = 1};
  return POCKET_STATUS_OK;
}

static int32_t blank_acquire(void *context, const pocket_rect *region,
                             pocket_target *out) {
  tab5_blank_t *blank = context;
  if (!blank || !region || region->struct_size < sizeof(*region) || !out ||
      out->struct_size < sizeof(*out) || region->x ||
      region->width != blank->width || !region->height ||
      region->y > blank->height - region->height)
    return POCKET_STATUS_INVALID;
  blank->region = *region;
  blank->surface = (pocket_surface){
      .struct_size = sizeof(blank->surface),
      .pixels = (uint8_t *)(blank->pixels + (size_t)region->y * blank->width),
      .byte_length = (size_t)blank->width * region->height * 2U,
      .region = &blank->region,
      .stride_bytes = (size_t)blank->width * 2U,
      .pixel_format = POCKET_PIXEL_FORMAT_RGB565};
  *out = (pocket_target){.struct_size = sizeof(*out),
                         .kind = POCKET_TARGET_CPU_VISIBLE,
                         .region = &blank->region,
                         .pixel_format = POCKET_PIXEL_FORMAT_RGB565,
                         .surface = &blank->surface};
  return POCKET_STATUS_OK;
}

static int32_t blank_flush(void *context, const pocket_rect *regions,
                           size_t count, uint64_t *token) {
  tab5_blank_t *blank = context;
  (void)regions;
  if (!blank || !count || !token)
    return POCKET_STATUS_INVALID;
  *token = ++blank->next;
  return POCKET_STATUS_OK;
}

static int32_t blank_wait(void *context, uint64_t token) {
  (void)context;
  (void)token;
  return POCKET_STATUS_OK;
}

static int32_t blank_identity(void *context, pocket_buffer_identity *out) {
  if (!context || !out || out->struct_size < sizeof(*out))
    return POCKET_STATUS_INVALID;
  *out = (pocket_buffer_identity){
      .struct_size = sizeof(*out), .buffer = 2, .generation = 1};
  return POCKET_STATUS_OK;
}

static const pocket_display_output_v1 BLANK = {.struct_size = sizeof(BLANK),
                                               .descriptor = blank_descriptor,
                                               .acquire = blank_acquire,
                                               .flush = blank_flush,
                                               .wait = blank_wait,
                                               .frame_identity = blank_identity};

/* ---- the launcher's operations ------------------------------------------- */

#if TAB5_LAUNCHER
static char launch_output[128];

static int32_t applications(void *context, uint16_t op,
                            const pocket_op_arg *args, size_t length,
                            pocket_result *out) {
  (void)context;
  out->tag = POCKET_RESULT_NONE;
  out->integer = 0;
  out->real = 0;
  out->bytes = NULL;
  out->byte_length = 0;
  switch (op) {
  case POCKET_OP_appTable: {
    const char *table = pocketjs_system_app_table();
    out->tag = POCKET_RESULT_BYTES;
    out->bytes = (const uint8_t *)table;
    out->byte_length = strlen(table);
    return POCKET_STATUS_OK;
  }
  case POCKET_OP_appLaunch:
    if (!args || length < sizeof *args || args->struct_size < sizeof *args ||
        args->tag != POCKET_ARG_BYTES ||
        args->byte_length >= sizeof launch_output)
      return POCKET_STATUS_INVALID;
    memcpy(launch_output, args->bytes, args->byte_length);
    launch_output[args->byte_length] = 0;
    out->tag = POCKET_RESULT_I64;
    out->integer = pocketjs_system_launch(launch_output) ? 1 : 0;
    return POCKET_STATUS_OK;
  case POCKET_OP_appShot:
    out->tag = POCKET_RESULT_I64;
    out->integer = -1;
    return POCKET_STATUS_OK;
  default:
    return POCKET_STATUS_UNSUPPORTED;
  }
}

static const pocket_op_provider_v1 APPLICATIONS = {sizeof APPLICATIONS,
                                                   applications};
static const uint16_t APPLICATION_OPS[] = {POCKET_OP_appTable,
                                           POCKET_OP_appLaunch,
                                           POCKET_OP_appShot};
#endif

/* ---- the panel ------------------------------------------------------------- */

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
  /* The margins beside a picture smaller than the screen stay black. */
  const size_t frame_bytes = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t);
  memset(framebuffer, 0, frame_bytes);
  esp_cache_msync(framebuffer, frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd.panel, true));
  ESP_ERROR_CHECK(bsp_display_backlight_on());
  ESP_ERROR_CHECK(bsp_touch_new(NULL, &host->touch));
}

/* ---- the runner ------------------------------------------------------------ */

static void interrupt_guest(void *context) {
  tab5_host_t *host = context;
  if (host->running.guest)
    pocket_guest_idf_interrupt(host->running.guest);
}

static void before_tick(void *context) {
  tab5_host_t *host = context;
  host->tick_started = esp_timer_get_time();
  tab5_camera_media_present();
#if TAB5_LAUNCHER
  pocketjs_system_pump();
  if (pocketjs_system_switch_pending() && !host->switch_signalled) {
    host->switch_signalled = true;
    xTaskNotifyGive(host->app_task);
  }
#endif
}

static void after_tick(int32_t status, void *context) {
  (void)status;
  tab5_host_t *host = context;
  const uint32_t elapsed =
      (uint32_t)(esp_timer_get_time() - host->tick_started);
  host->tick_us += elapsed;
  if (elapsed > host->tick_max_us)
    host->tick_max_us = elapsed;
}

/* ---- guests ----------------------------------------------------------------- */

static void stop_guest(tab5_host_t *host) {
  tab5_guest_t *running = &host->running;
  if (running->runner)
    ESP_ERROR_CHECK(pocket_runner_stop(running->runner));
  /* Camera and media textures belong to the runtime, which goes before its
   * guest, renderers and displays. */
  if (running->runtime) {
    tab5_camera_media_teardown();
    pocket_runtime_destroy(running->runtime);
  }
  if (running->guest)
    pocket_guest_idf_destroy(running->guest);
  if (running->lanes)
    pocketjs_esp32p4_ppa_lanes_destroy(running->lanes);
  for (size_t i = 0; i < 2U; i++)
    if (running->renderers[i])
      pocket_render_esp_ppa_destroy(running->renderers[i]);
  memset(running, 0, sizeof *running);
  /* The next guest draws a picture its displays have never seen. */
  pocket_display_esp_dpi_invalidate(host->display);
}

/* Boot the application in `bytes` and start its runner. `bind` records it
 * as the running app of the launcher mode and installs the system
 * namespaces into its realm. */
static int32_t boot_guest(tab5_host_t *host, const uint8_t *bytes, size_t size,
                          int32_t (*bind)(JSContext *realm,
                                          pocket_runtime *runtime,
                                          void *context),
                          void *bind_context) {
  tab5_guest_t *running = &host->running;
  const pocket_profile_facts *facts = pocket_profile.facts;
  pocket_runtime_config config = {
      .struct_size = sizeof(config),
      .host_abi = pocket_profile.host_abi,
      .tick_hz = pocket_profile.tick_hz,
      .heap_limit = TAB5_GUEST_HEAP,
      .frame_buffer_budget = host->frame_buffer_budget,
      .profile = facts,
  };
  memcpy(config.profile_hash, pocket_profile.profile_hash, 32);
  running->runtime = pocket_runtime_create(&config);
  if (!running->runtime)
    return POCKET_STATUS_FAILED;
  const pocket_guest_idf_config guest_config = {
      .heap_limit = TAB5_GUEST_HEAP,
      .stack_limit = 256U * 1024U,
      .prefer_psram = true,
  };
  running->guest = pocket_guest_idf_create(&guest_config);
  if (!running->guest)
    return POCKET_STATUS_FAILED;
  int32_t status = pocket_runtime_set_guest(
      running->runtime, pocket_guest_idf_vtable(), running->guest);
  running->package = (pocket_memory_package){bytes, size};
  if (status == POCKET_STATUS_OK)
    status = pocket_runtime_set_package_source(
        running->runtime, pocket_memory_package_vtable(), &running->package);
  if (status == POCKET_STATUS_OK)
    status = pocket_runtime_add_input_source(running->runtime, &INPUT, host);
  /* Two renderers, one per core, split every strip. Their caches key on
   * this guest's texture handles, so each guest gets its own. */
  for (size_t i = 0; i < 2U && status == POCKET_STATUS_OK; i++) {
    running->renderers[i] = pocket_render_esp_ppa_create(
        &host->ppa_config, pocketjs_esp32p4_ppa_status_ops(host->devices[i]));
    if (!running->renderers[i])
      status = POCKET_STATUS_FAILED;
  }
  if (status == POCKET_STATUS_OK &&
      pocketjs_esp32p4_ppa_lanes_create(running->renderers[0],
                                        running->renderers[1], 1,
                                        host->quantum, &running->lanes) != ESP_OK)
    status = POCKET_STATUS_FAILED;
  pocket_renderer_id renderer = POCKET_RENDERER_INVALID;
  if (status == POCKET_STATUS_OK) {
    renderer = pocket_runtime_add_renderer(
        running->runtime, pocketjs_esp32p4_ppa_lanes_vtable(), running->lanes);
    if (renderer == POCKET_RENDERER_INVALID)
      status = POCKET_STATUS_FAILED;
  }
  if (status == POCKET_STATUS_OK)
    status = pocket_runtime_add_display(running->runtime,
                                        pocket_display_esp_dpi_vtable(),
                                        host->display, renderer);
  if (status == POCKET_STATUS_OK && facts->auxiliary)
    status = pocket_runtime_add_display(running->runtime, &BLANK, &host->blank,
                                        POCKET_RENDERER_SOFTWARE);
#if TAB5_LAUNCHER
  if (status == POCKET_STATUS_OK)
    status = pocket_runtime_add_op_provider(
        running->runtime, &APPLICATIONS, host, APPLICATION_OPS,
        sizeof APPLICATION_OPS / sizeof *APPLICATION_OPS);
#endif
  if (status == POCKET_STATUS_OK && bind)
    status = bind(pocket_guest_idf_context(running->guest), running->runtime,
                  bind_context);
  if (status == POCKET_STATUS_OK) {
    const int64_t started = esp_timer_get_time();
    status = pocket_runtime_boot(running->runtime);
    if (status != POCKET_STATUS_OK)
      ESP_LOGE(TAG, "boot status=%" PRId32 ": %s", status,
               pocket_guest_idf_error(running->guest));
    else
      ESP_LOGI(TAG, "booted in %.1f ms",
               (esp_timer_get_time() - started) / 1000.0);
  }
  if (status != POCKET_STATUS_OK)
    return status;
  pocket_runner_config_t runner_config;
  pocket_runner_config_defaults(&runner_config);
  /* Solid's reactive graph recurses through the stack in a tick. */
  runner_config.task_stack_bytes = 64U * 1024U;
  runner_config.task_stack_caps = MALLOC_CAP_SPIRAM;
  runner_config.task_core = 0;
  runner_config.interrupt = interrupt_guest;
  runner_config.before_tick = before_tick;
  runner_config.after_tick = after_tick;
  runner_config.user_data = host;
  host->switch_signalled = false;
  return pocket_runner_start(running->runtime, pocket_profile.tick_hz,
                             &runner_config, &running->runner) == ESP_OK
             ? POCKET_STATUS_OK
             : POCKET_STATUS_FAILED;
}

#if TAB5_LAUNCHER
static int32_t bind_system(JSContext *realm, pocket_runtime *runtime,
                           void *context) {
  const pocketjs_system_app_t *app = context;
  if (pocketjs_system_bind(realm, app) != ESP_OK)
    return POCKET_STATUS_FAILED;
  tab5_camera_media_install(realm, runtime,
                            pocketjs_system_app_facts(app)->features);
  return POCKET_STATUS_OK;
}

/* Boot `app`; on failure the guest is torn down. */
static int32_t boot_app(tab5_host_t *host, pocketjs_system_app_t *app) {
  size_t size = 0;
  const uint8_t *bytes = pocketjs_system_app_bytes(app, &size);
  const int32_t status = boot_guest(host, bytes, size, bind_system, app);
  if (status != POCKET_STATUS_OK) {
    stop_guest(host);
    pocketjs_system_unbind();
  }
  return status;
}

/* Replace the running guest with the one the launcher asked for, or with
 * the launcher after `failed`. Returns the app now running. */
static pocketjs_system_app_t *switch_guest(tab5_host_t *host,
                                           pocketjs_system_app_t *app,
                                           bool failed) {
  stop_guest(host);
  pocketjs_system_unbind();
  pocketjs_system_app_free(app);
  char slot[17] = {0};
  bool launcher = true;
  if (failed || !pocketjs_system_take_switch(slot, &launcher))
    launcher = true;
  pocketjs_system_app_t *next = NULL;
  int32_t status = POCKET_STATUS_FAILED;
  if (!launcher) {
    if (pocketjs_system_load_slot(slot, &next) == ESP_OK) {
      status = boot_app(host, next);
      if (status != POCKET_STATUS_OK) {
        pocketjs_system_app_free(next);
        next = NULL;
      }
    }
    if (status != POCKET_STATUS_OK) {
      ESP_LOGE(TAG, "slot %s did not boot", slot);
      pocketjs_system_report_failure("the app did not start");
    }
  }
  if (status != POCKET_STATUS_OK) {
    ESP_ERROR_CHECK(pocketjs_system_load_launcher(&next));
    if (boot_app(host, next) != POCKET_STATUS_OK)
      abort();
  }
  return next;
}

/* The ESP32-C6 that carries Wi-Fi: power it, then bring up the SDIO link
 * (esp_hosted starts here, not before app_main, so the render strips are
 * allocated first). */
static esp_err_t start_coprocessor(void) {
  ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_WIFI, true), TAG,
                      "coprocessor power");
  if (esp_hosted_init() != 0 || esp_hosted_connect_to_slave() != 0)
    return ESP_FAIL;
  return ESP_OK;
}

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

/* Storage and the system services of launcher mode. */
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
      .target = pocket_profile.id,
      .host_abi = pocket_profile.host_abi,
      .profile_hash = pocket_profile.profile_hash,
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
#endif

/* ---- the program ------------------------------------------------------------ */

static void run_app(void *unused) {
  (void)unused;
  static tab5_host_t host;
  const pocket_profile_facts *facts = pocket_profile.facts;
  /* The picture is the auxiliary surface when the profile has one. */
  const pocket_profile_display *picture =
      facts->auxiliary ? facts->auxiliary : facts->display;
  host.surface = facts->auxiliary ? POCKET_DISPLAY_ROLE_AUXILIARY
                                  : POCKET_DISPLAY_ROLE_PRIMARY;
  const uint32_t width = picture->physical[0];
  const uint32_t height = picture->physical[1];
  host.turned = width > height;
  const uint32_t scale = screen_width(&host) / width < screen_height(&host) / height
                             ? screen_width(&host) / width
                             : screen_height(&host) / height;
  assert(scale >= 1U);
  host.picture_width = width * scale;
  host.picture_height = height * scale;
  host.origin_x = (screen_width(&host) - host.picture_width) / 2U;
  host.origin_y = (screen_height(&host) - host.picture_height) / 2U;
  host.unit = (float)(scale * picture->raster_density);
  start_panel(&host);

  /* One DMA channel copies whole-row strips; the PPA turns the others. A
   * second channel moves retained pixels for content that only moved. */
  async_memcpy_handle_t copy = NULL;
  async_memcpy_config_t copy_config = ASYNC_MEMCPY_DEFAULT_CONFIG();
  copy_config.backlog = 4;
  copy_config.dma_burst_size = 64;
  ESP_ERROR_CHECK(esp_async_memcpy_install_gdma_axi(&copy_config, &copy));
  async_memcpy_handle_t move_copy = NULL;
  async_memcpy_config_t move_config = ASYNC_MEMCPY_DEFAULT_CONFIG();
  move_config.backlog = TAB5_MOVE_BACKLOG;
  move_config.dma_burst_size = 64;
  ESP_ERROR_CHECK(esp_async_memcpy_install_gdma_axi(&move_config, &move_copy));
  /* A turned strip presented by the PPA is at least 8 raster rows tall: the
   * PPA stops completing 90-degree presents of blocks 4 or 6 raster rows
   * tall while the panel scans out (measured on ESP32-P4 rev 1.3; 2 and 8 to
   * 28 rows complete). Unturned strips keep whole 128-byte cache lines. */
  host.quantum = host.turned ? 8U : 4U;
  size_t strip_budget = TAB5_STRIP_BYTES;
  if (TAB5_LAUNCHER) {
    /* The launcher's radio, store and link take internal RAM outside the
     * largest heap region: both strips come from that region. */
    const size_t block = heap_caps_get_largest_free_block(
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (block / 2U - 128U < strip_budget)
      strip_budget = block / 2U - 128U;
  }
  const uint32_t strip_rows =
      strip_budget / (width * 2U) / host.quantum * host.quantum;
  assert(strip_rows >= host.quantum);
  const size_t strips = (size_t)width * 2U * strip_rows * 2U;
  const pocket_display_esp_dpi_config display_config = {
      .struct_size = sizeof(display_config),
      .framebuffer = host.framebuffer,
      .panel_width = BSP_LCD_H_RES,
      .panel_height = BSP_LCD_V_RES,
      .role = host.surface,
      .width = width,
      .height = height,
      .turned = host.turned,
      .scale = scale,
      .origin_x = host.origin_x,
      .origin_y = host.origin_y,
      .strip_rows = strip_rows,
      .row_alignment = host.quantum,
      .frame_buffer_budget = strips,
      .copy = copy,
      .move_copy = move_copy,
  };
  host.display = pocket_display_esp_dpi_create(&display_config);
  assert(host.display != NULL);
  host.frame_buffer_budget = strips;
  if (facts->auxiliary) {
    host.blank.width = facts->display->physical[0];
    host.blank.height = facts->display->physical[1];
    const size_t bytes = (size_t)host.blank.width * host.blank.height * 2U;
    host.blank.pixels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    assert(host.blank.pixels != NULL);
    host.frame_buffer_budget += bytes;
  }
  for (size_t i = 0; i < 2U; i++)
    ESP_ERROR_CHECK(pocketjs_esp32p4_ppa_create(&host.devices[i]));
  host.ppa_config = (pocket_ppa_config){
      .struct_size = sizeof(pocket_ppa_config),
      .scale = picture->raster_density,
      .min_fill_pixels = 1024,
      .min_blend_pixels = 256,
      .min_srm_pixels = 256,
      .mask_budget = strip_budget / 2U,
  };
  const uint32_t margin =
      host.picture_width < screen_width(&host)
          ? screen_width(&host) - host.origin_x - host.picture_width
          : screen_height(&host) - host.origin_y - host.picture_height;
  if (margin > 2U * TAB5_BUTTON_INSET) {
    host.button_size = margin - 2U * TAB5_BUTTON_INSET < TAB5_BUTTON_MAX
                           ? margin - 2U * TAB5_BUTTON_INSET
                           : TAB5_BUTTON_MAX;
    host.button_shown = true;
    paint_home_button(&host);
  }
  ESP_LOGI(TAG, "%s: %" PRIu32 "x%" PRIu32 " %s at %" PRIu32 "x, strips of %" PRIu32 " rows",
           pocket_profile.id, width, height, host.turned ? "turned" : "upright",
           scale, strip_rows);
  host.app_task = xTaskGetCurrentTaskHandle();

#if TAB5_LAUNCHER
  start_system();
  pocketjs_system_app_t *app = NULL;
  ESP_ERROR_CHECK(pocketjs_system_load_launcher(&app));
  if (boot_app(&host, app) != POCKET_STATUS_OK)
    abort();
#else
  if (boot_guest(&host, pocketjs_package_app.data, pocketjs_package_app.size,
                 NULL, NULL) != POCKET_STATUS_OK)
    abort();
#endif

  pocket_runner_stats_t last = {.struct_size = sizeof(last)};
  int64_t logged = esp_timer_get_time();
  for (;;) {
    const bool woken = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) != 0U;
    pocket_runner_stats_t stats = {.struct_size = sizeof(stats)};
    ESP_ERROR_CHECK(pocket_runner_stats(host.running.runner, &stats));
#if TAB5_LAUNCHER
    if (woken || (stats.frame_errors != last.frame_errors &&
                  !pocketjs_system_app_is_launcher(app))) {
      /* An app that throws from a frame goes back to the launcher, which
       * shows the failure once. */
      if (!woken)
        pocketjs_system_report_failure("the app stopped with an error");
      app = switch_guest(&host, app, !woken);
      last = (pocket_runner_stats_t){.struct_size = sizeof(last)};
      continue;
    }
#else
    (void)woken;
#endif
    const int64_t now = esp_timer_get_time();
    if (now - logged < 5000000)
      continue;
    const uint32_t frames = stats.frames - last.frames;
    ESP_LOGI(TAG,
             "fps=%.1f skipped=%" PRIu32 " errors=%" PRIu32
             " frame_avg_ms=%.2f frame_max_ms=%.2f",
             frames * 1e6 / (double)(now - logged),
             stats.frames_skipped - last.frames_skipped,
             stats.frame_errors - last.frame_errors,
             frames > 0U ? host.tick_us / 1000.0 / frames : 0.0,
             host.tick_max_us / 1000.0);
    host.tick_us = 0;
    host.tick_max_us = 0;
    last = stats;
    logged = now;
  }
}

void app_main(void) {
  /* Evaluating Clear needs about 100 KB of stack. A PSRAM stack leaves the
   * internal RAM to the staging strips. */
  const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
      run_app, "pocketjs_app", 128U * 1024U, NULL, 1, NULL, 0,
      MALLOC_CAP_SPIRAM);
  assert(created == pdPASS);
}
