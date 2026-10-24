/* M5Stack Tab5: one embedded application on the 720x1280 MIPI-DSI panel.
 * The runtime renders the picture in strips with the PPA renderer; the DPI
 * display copies each strip into the panel's framebuffer, turning a
 * landscape picture onto the portrait panel. */
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"
#include "esp_async_memcpy.h"
#include "esp_cache.h"
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
#include "pocketjs/esp32p4_ppa_lanes.h"
#include "pocketjs/esp32p4_ppa_status.h"
#include "pocketjs_package_app.h"

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

typedef struct {
  esp_lcd_touch_handle_t touch;
  uint16_t *framebuffer;
  /* Landscape pictures turn 90 degrees counter-clockwise onto the portrait
   * panel; portrait pictures keep their orientation. */
  bool turned;
  /* The surface the picture shows (0 primary, 1 auxiliary), and its size
   * and top-left corner on the screen as the picture is oriented, in panel
   * pixels. */
  uint32_t surface;
  uint32_t picture_width;
  uint32_t picture_height;
  uint32_t origin_x;
  uint32_t origin_y;
  /* Panel pixels per logical pixel. */
  float unit;
  uint32_t contacts[TAB5_MAX_CONTACTS];
  uint32_t contact_count;
  pocket_guest_idf *guest;
  int64_t tick_started;
  uint64_t tick_us;
  uint32_t tick_max_us;
} tab5_host_t;

static uint32_t input_classes(void *context) {
  (void)context;
  return POCKET_INPUT_CLASS_CONTACT;
}

/* Contacts on the picture in logical pixels. A failed read cancels the
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
  if (!read) {
    for (uint32_t i = 0; i < host->contact_count; i++)
      cancelled[cancelled_count++] = (pocket_cancelled_contact){
          .struct_size = sizeof(pocket_cancelled_contact),
          .surface = host->surface,
          .id = host->contacts[i]};
    count = 0;
  }
  for (uint8_t i = 0; i < count; i++) {
    const int32_t screen_x = host->turned
                                 ? (int32_t)BSP_LCD_V_RES - 1 - points[i].y
                                 : points[i].x;
    const int32_t screen_y = host->turned ? points[i].x : points[i].y;
    const int32_t x = screen_x - (int32_t)host->origin_x;
    const int32_t y = screen_y - (int32_t)host->origin_y;
    if (x < 0 || y < 0 || x >= (int32_t)host->picture_width ||
        y >= (int32_t)host->picture_height)
      continue;
    contacts[contact_count++] = (pocket_contact){
        .struct_size = sizeof(pocket_contact),
        .surface = host->surface,
        .id = points[i].track_id,
        .x = (float)x / host->unit,
        .y = (float)y / host->unit};
  }
  const pocket_input_frame frame = {
      .struct_size = sizeof(frame),
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

static void interrupt_guest(void *context) {
  tab5_host_t *host = context;
  pocket_guest_idf_interrupt(host->guest);
}

static void before_tick(void *context) {
  tab5_host_t *host = context;
  host->tick_started = esp_timer_get_time();
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

static void require(int32_t status, const char *what) {
  if (status != POCKET_STATUS_OK) {
    ESP_LOGE(TAG, "%s failed: %" PRId32, what, status);
    abort();
  }
}

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
  const uint32_t screen_width = host.turned ? BSP_LCD_V_RES : BSP_LCD_H_RES;
  const uint32_t screen_height = host.turned ? BSP_LCD_H_RES : BSP_LCD_V_RES;
  const uint32_t scale = screen_width / width < screen_height / height
                             ? screen_width / width
                             : screen_height / height;
  assert(scale >= 1U);
  host.picture_width = width * scale;
  host.picture_height = height * scale;
  host.origin_x = (screen_width - host.picture_width) / 2U;
  host.origin_y = (screen_height - host.picture_height) / 2U;
  host.unit = (float)(scale * picture->raster_density);
  start_panel(&host);

  /* One DMA channel copies whole-row strips; the PPA turns the others. */
  async_memcpy_handle_t copy = NULL;
  async_memcpy_config_t copy_config = ASYNC_MEMCPY_DEFAULT_CONFIG();
  copy_config.backlog = 4;
  copy_config.dma_burst_size = 64;
  ESP_ERROR_CHECK(esp_async_memcpy_install_gdma_axi(&copy_config, &copy));
  /* A second channel moves retained pixels for content that only moved. */
  async_memcpy_handle_t move_copy = NULL;
  async_memcpy_config_t move_config = ASYNC_MEMCPY_DEFAULT_CONFIG();
  move_config.backlog = TAB5_MOVE_BACKLOG;
  move_config.dma_burst_size = 64;
  ESP_ERROR_CHECK(esp_async_memcpy_install_gdma_axi(&move_config, &move_copy));
  /* A turned strip presented by the PPA is at least 8 raster rows tall: the
   * PPA stops completing 90-degree presents of blocks 4 or 6 raster rows
   * tall while the panel scans out (measured on ESP32-P4 rev 1.3; 2 and 8 to
   * 28 rows complete). Unturned strips keep whole 128-byte cache lines. */
  const uint32_t quantum = host.turned ? 8U : 4U;
  const uint32_t strip_rows =
      TAB5_STRIP_BYTES / (width * 2U) / quantum * quantum;
  const size_t budget = (size_t)width * 2U * strip_rows * 2U;
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
      .row_alignment = quantum,
      .frame_buffer_budget = budget,
      .copy = copy,
      .move_copy = move_copy,
  };
  pocket_display_esp_dpi *display =
      pocket_display_esp_dpi_create(&display_config);
  assert(display != NULL);

  pocket_runtime_config config = {
      .struct_size = sizeof(config),
      .host_abi = pocket_profile.host_abi,
      .tick_hz = pocket_profile.tick_hz,
      .heap_limit = TAB5_GUEST_HEAP,
      .frame_buffer_budget = budget,
      .profile = facts,
  };
  memcpy(config.profile_hash, pocket_profile.profile_hash, 32);
  pocket_runtime *runtime = pocket_runtime_create(&config);
  assert(runtime != NULL);
  const pocket_guest_idf_config guest_config = {
      .heap_limit = TAB5_GUEST_HEAP,
      .stack_limit = 256U * 1024U,
      .prefer_psram = true,
  };
  pocket_guest_idf *guest = pocket_guest_idf_create(&guest_config);
  assert(guest != NULL);
  host.guest = guest;
  require(pocket_runtime_set_guest(runtime, pocket_guest_idf_vtable(), guest),
          "guest");
  static pocket_memory_package package;
  package = (pocket_memory_package){pocketjs_package_app.data,
                                    pocketjs_package_app.size};
  require(pocket_runtime_set_package_source(
              runtime, pocket_memory_package_vtable(), &package),
          "package");
  require(pocket_runtime_add_input_source(runtime, &INPUT, &host), "input");

  /* Two renderers, one per core, split every strip. */
  const pocket_ppa_config ppa_config = {
      .struct_size = sizeof(ppa_config),
      .scale = picture->raster_density,
      .min_fill_pixels = 1024,
      .min_blend_pixels = 256,
      .min_srm_pixels = 256,
      .mask_budget = TAB5_STRIP_BYTES / 2U,
  };
  pocket_render_esp_ppa *renderers[2];
  for (size_t i = 0; i < 2U; i++) {
    pocketjs_esp32p4_ppa_t *device = NULL;
    ESP_ERROR_CHECK(pocketjs_esp32p4_ppa_create(&device));
    renderers[i] = pocket_render_esp_ppa_create(
        &ppa_config, pocketjs_esp32p4_ppa_status_ops(device));
    assert(renderers[i] != NULL);
  }
  pocketjs_esp32p4_ppa_lanes_t *lanes = NULL;
  ESP_ERROR_CHECK(pocketjs_esp32p4_ppa_lanes_create(renderers[0], renderers[1],
                                                    1, quantum, &lanes));
  const pocket_renderer_id renderer_id = pocket_runtime_add_renderer(
      runtime, pocketjs_esp32p4_ppa_lanes_vtable(), lanes);
  assert(renderer_id != POCKET_RENDERER_INVALID);
  require(pocket_runtime_add_display(runtime, pocket_display_esp_dpi_vtable(),
                                     display, renderer_id),
          "display");
  const int64_t boot_started = esp_timer_get_time();
  const int32_t booted = pocket_runtime_boot(runtime);
  if (booted != POCKET_STATUS_OK)
    ESP_LOGE(TAG, "boot status=%" PRId32 ": %s", booted,
             pocket_guest_idf_error(guest));
  require(booted, "boot");
  ESP_LOGI(TAG, "booted %s in %.1f ms (%" PRIu32 "x%" PRIu32 " %s, scale %" PRIu32 ")",
           pocket_profile.id, (esp_timer_get_time() - boot_started) / 1000.0,
           width, height, host.turned ? "turned" : "upright", scale);

  pocket_runner_config_t runner_config;
  pocket_runner_config_defaults(&runner_config);
  /* Solid's reactive graph recurses through the stack in a tick. */
  runner_config.task_stack_bytes = 64U * 1024U;
  runner_config.task_stack_caps = MALLOC_CAP_SPIRAM;
  runner_config.task_core = 0;
  runner_config.interrupt = interrupt_guest;
  runner_config.before_tick = before_tick;
  runner_config.after_tick = after_tick;
  runner_config.user_data = &host;
  pocket_runner_t *runner = NULL;
  ESP_ERROR_CHECK(pocket_runner_start(runtime, pocket_profile.tick_hz,
                                      &runner_config, &runner));

  pocket_runner_stats_t last = {.struct_size = sizeof(last)};
  int64_t logged = esp_timer_get_time();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    pocket_runner_stats_t stats = {.struct_size = sizeof(stats)};
    ESP_ERROR_CHECK(pocket_runner_stats(runner, &stats));
    const int64_t now = esp_timer_get_time();
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
