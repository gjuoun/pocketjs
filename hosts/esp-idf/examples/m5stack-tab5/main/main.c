#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bsp/display.h"
#include "bsp/touch.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pocketjs/esp32p4_ppa.h"
#include "pocketjs/guest.h"
#include "pocketjs/package.h"
#include "pocketjs/render_rgb565.h"
#include "pocketjs/runner.h"
#include "pocketjs/ui_core.h"
#include "pocketjs/ui_qjs.h"
#include "pocketjs_package_app.h"

static const char *TAG = "pocketjs_tab5";

/* Logical rows per rendered strip. */
#define TAB5_STRIP_ROWS 24U

typedef struct {
  esp_lcd_touch_handle_t touch;
  pocketjs_rgb565_renderer_t *renderer;
  pocketjs_rgb565_target_t *target;
  const pocketjs_rgb565_accelerator_t *accelerator;
  ppa_client_handle_t present;
  uint16_t *framebuffer;
  uint16_t *strip;
  /* Landscape pictures turn 90 degrees counter-clockwise onto the 720x1280
   * portrait panel; portrait pictures keep their orientation. */
  bool landscape;
  int64_t frame_started;
  uint64_t frame_us;
  uint32_t frame_max_us;
} tab5_host_t;

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
  input->touch_count = 0;
  for (uint8_t index = 0; index < count; ++index) {
    const uint32_t x = host->landscape
                           ? contract->physical_width - 1U - points[index].y
                           : points[index].x;
    const uint32_t y = host->landscape ? points[index].x : points[index].y;
    if (x >= contract->physical_width || y >= contract->physical_height)
      continue;
    touches[input->touch_count++] = (pocketjs_ui_touch_t){
        .id = points[index].track_id,
        .x = x / contract->raster_density,
        .y = y / contract->raster_density,
    };
  }
  input->touches = touches;
  return ESP_OK;
}

static esp_err_t present_strip(tab5_host_t *host,
                               const pocketjs_ui_frame_view_t *frame,
                               pocketjs_rgb565_rect_t rect) {
  const uint32_t density = frame->raster_density;
  const uint32_t width = frame->logical_width * density;
  const uint32_t x = rect.x * density;
  const uint32_t y = rect.y * density;
  const uint32_t w = rect.width * density;
  const uint32_t h = rect.height * density;
  ppa_srm_oper_config_t operation = {
      .in =
          {
              .buffer = host->strip,
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
              .block_offset_x = x,
              .block_offset_y = y,
              .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
          },
      .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
      .scale_x = 1.0f,
      .scale_y = 1.0f,
      .mode = PPA_TRANS_MODE_BLOCKING,
  };
  if (host->landscape) {
    /* Picture (x, y) lands on panel (y, width - 1 - x). */
    operation.out.block_offset_x = y;
    operation.out.block_offset_y = width - x - w;
    operation.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
  }
  return ppa_do_scale_rotate_mirror(host->present, &operation);
}

static esp_err_t after_turn(const pocketjs_ui_frame_view_t *frame,
                            void *user_data) {
  tab5_host_t *host = user_data;
  pocketjs_rgb565_damage_plan_t plan = {.struct_size = sizeof(plan)};
  ESP_RETURN_ON_ERROR(
      pocketjs_rgb565_prepare(host->renderer, host->target, frame, &plan), TAG,
      "prepare");
  const size_t row_pixels = (size_t)frame->logical_width *
                            frame->raster_density * frame->raster_density;
  for (uint32_t index = 0; index < plan.region_count; ++index) {
    const pocketjs_rgb565_rect_t region = plan.regions[index];
    const uint32_t bottom = region.y + region.height;
    for (uint32_t y = region.y; y < bottom; y += TAB5_STRIP_ROWS) {
      pocketjs_rgb565_rect_t rect = region;
      rect.y = y;
      rect.height = bottom - y < TAB5_STRIP_ROWS ? bottom - y : TAB5_STRIP_ROWS;
      pocketjs_rgb565_render_stats_t stats = {.struct_size = sizeof(stats)};
      esp_err_t result = pocketjs_rgb565_render_strip(
          host->renderer, frame, host->strip, row_pixels * rect.height, rect,
          host->accelerator, &stats);
      if (result == ESP_OK)
        result = present_strip(host, frame, rect);
      if (result != ESP_OK) {
        pocketjs_rgb565_abort(host->renderer, host->target);
        return result;
      }
    }
  }
  const uint32_t elapsed =
      (uint32_t)(esp_timer_get_time() - host->frame_started);
  host->frame_us += elapsed;
  if (elapsed > host->frame_max_us)
    host->frame_max_us = elapsed;
  return pocketjs_rgb565_commit(host->renderer, host->target, frame);
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
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd.panel, true));
  ESP_ERROR_CHECK(bsp_display_backlight_on());
  ESP_ERROR_CHECK(bsp_touch_new(NULL, &host->touch));

  const ppa_client_config_t present_config = {
      .oper_type = PPA_OPERATION_SRM,
      .max_pending_trans_num = 1,
  };
  ESP_ERROR_CHECK(ppa_register_client(&present_config, &host->present));
}

void app_main(void) {
  static tab5_host_t host;
  const pocketjs_package_host_contract_t *contract =
      &pocketjs_package_app_contract;
  start_panel(&host);
  host.landscape = contract->physical_width > contract->physical_height;
  /* Both host profiles fill the panel at raster density 2. */
  assert((host.landscape ? contract->physical_height
                         : contract->physical_width) == BSP_LCD_H_RES);
  assert((host.landscape ? contract->physical_width
                         : contract->physical_height) == BSP_LCD_V_RES);

  pocketjs_package_t *package = NULL;
  ESP_ERROR_CHECK(pocketjs_package_open(
      pocketjs_package_app.data, pocketjs_package_app.size, 0, &package));
  pocketjs_package_variant_t app = {.struct_size = sizeof(app)};
  ESP_ERROR_CHECK(pocketjs_package_select(package, contract, &app));

  pocketjs_guest_config_t guest_config;
  pocketjs_guest_config_defaults(&guest_config);
  pocketjs_guest_t *guest = NULL;
  ESP_ERROR_CHECK(pocketjs_guest_create(&guest_config, &guest));

  pocketjs_ui_core_config_t core_config;
  pocketjs_ui_core_config_defaults(&core_config);
  core_config.logical_width = contract->logical_width;
  core_config.logical_height = contract->logical_height;
  core_config.raster_density = contract->raster_density;
  core_config.tick_hz = contract->tick_hz;
  pocketjs_ui_core_t *core = NULL;
  ESP_ERROR_CHECK(pocketjs_ui_core_create(&core_config, &core));

  const pocketjs_ui_qjs_config_t binding_config = {
      .struct_size = sizeof(binding_config),
      .target_id = contract->target_id,
      .host_abi = contract->host_abi,
  };
  pocketjs_ui_qjs_t *binding = NULL;
  ESP_ERROR_CHECK(
      pocketjs_ui_qjs_create(guest, core, &binding_config, &binding));
  ESP_ERROR_CHECK(
      pocketjs_ui_qjs_feed_pak(binding, app.pak.data, app.pak.size));
  ESP_ERROR_CHECK(pocketjs_ui_qjs_mount(binding));
  ESP_ERROR_CHECK(pocketjs_guest_eval(guest, (const char *)app.javascript.data,
                                      app.javascript.size - 1U, "app"));

  pocketjs_rgb565_renderer_config_t renderer_config;
  pocketjs_rgb565_renderer_config_defaults(&renderer_config);
  renderer_config.scale = contract->raster_density;
  ESP_ERROR_CHECK(
      pocketjs_rgb565_renderer_create(&renderer_config, &host.renderer));
  ESP_ERROR_CHECK(pocketjs_rgb565_target_create(&host.target));
  pocketjs_esp32p4_ppa_t *ppa = NULL;
  ESP_ERROR_CHECK(pocketjs_esp32p4_ppa_create(&ppa));
  host.accelerator = pocketjs_esp32p4_ppa_accelerator(ppa);
  host.strip = heap_caps_aligned_alloc(
      128,
      (size_t)contract->physical_width * TAB5_STRIP_ROWS *
          contract->raster_density * sizeof(uint16_t),
      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  assert(host.strip != NULL);

  pocketjs_runner_config_t runner_config;
  pocketjs_runner_config_defaults(&runner_config);
  runner_config.task_stack_bytes = 64U * 1024U;
  runner_config.sample_input = sample_input;
  runner_config.after_turn = after_turn;
  runner_config.user_data = &host;
  pocketjs_runner_t *runner = NULL;
  ESP_ERROR_CHECK(pocketjs_runner_start(binding, &runner_config, &runner));

  pocketjs_runner_stats_t last = {.struct_size = sizeof(last)};
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    pocketjs_runner_stats_t stats = {.struct_size = sizeof(stats)};
    ESP_ERROR_CHECK(pocketjs_runner_stats(runner, &stats));
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
  }
}
