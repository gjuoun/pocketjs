#include "tab5_camera_media.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bsp/m5stack_tab5.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "pocketjs/camera.h"
#include "pocketjs/media.h"

static const char *TAG = "tab5_camera_media";

/* The sensor's picture is upright with the Tab5 held as the landscape
 * firmware draws: the panel's left edge on top. */
#define TAB5_CAMERA_ROTATION 0U
/* The ES8388's volume at the guest's full volume. */
#define TAB5_VOLUME_MAX 80U
/* The second core: the first runs the guest. */
#define TAB5_WORKER_CORE 1

static bool camera_ready;
static bool media_ready;
static esp_codec_dev_handle_t speaker;

/* A resolved plan's canonical JSON lists each capability under features as
 * `"<capability>":true` when the target provides it. */
static bool admits(const pocketjs_package_variant_t *app,
                   const char *capability) {
  char needle[48];
  const int length = snprintf(needle, sizeof needle, "\"%s\":true", capability);
  const uint8_t *plan = app->plan_json.data;
  const size_t size = app->plan_json.size;
  if (length <= 0 || (size_t)length >= sizeof needle || size < (size_t)length)
    return false;
  for (size_t at = 0; at + (size_t)length <= size; ++at)
    if (memcmp(plan + at, needle, (size_t)length) == 0)
      return true;
  return false;
}

static esp_err_t start_sensor(void *context) {
  (void)context;
  return bsp_camera_start(NULL);
}

static esp_err_t audio_open(void *context, uint32_t rate) {
  (void)context;
  speaker = bsp_audio_codec_speaker_init();
  if (speaker == NULL)
    return ESP_FAIL;
  esp_codec_dev_sample_info_t format = {
      .bits_per_sample = 16,
      .channel = 2,
      .sample_rate = rate,
  };
  if (esp_codec_dev_open(speaker, &format) != ESP_CODEC_DEV_OK)
    return ESP_FAIL;
  esp_codec_dev_set_out_vol(speaker, TAB5_VOLUME_MAX);
  return ESP_OK;
}

static esp_err_t audio_write(void *context, const int16_t *pcm,
                             size_t frames) {
  (void)context;
  return esp_codec_dev_write(speaker, (void *)pcm, (int)(frames * 4U)) ==
                 ESP_CODEC_DEV_OK
             ? ESP_OK
             : ESP_FAIL;
}

static void audio_volume(void *context, uint32_t percent) {
  (void)context;
  esp_codec_dev_set_out_vol(speaker, (int)(percent * TAB5_VOLUME_MAX / 100U));
}

void tab5_camera_media_install(pocketjs_guest_t *guest,
                               pocketjs_ui_core_t *core,
                               const pocketjs_package_variant_t *app) {
  const bool qr = admits(app, "camera.qr");
  const bool photo = admits(app, "camera.photo");
  if ((qr || photo) && !camera_ready) {
    pocketjs_camera_config_t config;
    pocketjs_camera_config_defaults(&config);
    config.device = ESP_VIDEO_MIPI_CSI_DEVICE_NAME;
    config.start_sensor = start_sensor;
    config.rotation = TAB5_CAMERA_ROTATION;
    config.storage_root = "/pocket";
    config.worker_core = TAB5_WORKER_CORE;
    camera_ready = pocketjs_camera_setup(&config) == ESP_OK;
  }
  if (camera_ready)
    ESP_ERROR_CHECK(pocketjs_camera_install(guest, core, qr, photo));
  if (admits(app, "media.playback")) {
    if (!media_ready) {
      pocketjs_media_config_t config;
      pocketjs_media_config_defaults(&config);
      config.audio_open = audio_open;
      config.audio_write = audio_write;
      config.audio_volume = audio_volume;
      config.worker_core = TAB5_WORKER_CORE;
      media_ready = pocketjs_media_setup(&config) == ESP_OK;
      if (!media_ready)
        ESP_LOGE(TAG, "media did not start");
    }
    if (media_ready)
      ESP_ERROR_CHECK(pocketjs_media_install(guest, core));
  }
}

void tab5_camera_media_present(void) {
  if (camera_ready)
    pocketjs_camera_present();
  if (media_ready)
    pocketjs_media_present();
}

void tab5_camera_media_teardown(void) {
  if (camera_ready)
    pocketjs_camera_teardown();
  if (media_ready)
    pocketjs_media_teardown();
}
