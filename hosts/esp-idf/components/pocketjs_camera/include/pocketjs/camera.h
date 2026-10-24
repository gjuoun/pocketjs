#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pocketjs/guest.h"
#include "pocketjs/ui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Board facts and hooks for `globalThis.camera` (contracts/spec/camera.ts). */
typedef struct {
  size_t struct_size;
  /** The V4L2 capture device esp_video registers for the sensor, e.g.
   * ESP_VIDEO_MIPI_CSI_DEVICE_NAME. */
  const char *device;
  /** Brings the sensor up once, before the first open (the board's camera
   * start: power, SCCB, esp_video_init). */
  esp_err_t (*start_sensor)(void *context);
  void *context;
  /** Degrees the sensor's picture turns clockwise to stand upright on the
   * board as it is held: 0, 90, 180 or 270. */
  uint32_t rotation;
  /** Directory holding DCIM/ and the thumbnail cache, e.g. "/pocket". */
  const char *storage_root;
  /** Core for the capture, QR decoder and photo worker tasks. */
  int worker_core;
} pocketjs_camera_config_t;

void pocketjs_camera_config_defaults(pocketjs_camera_config_t *config);

/** Record the board's facts. Nothing starts until a guest opens the camera. */
esp_err_t pocketjs_camera_setup(const pocketjs_camera_config_t *config);

/** Install `globalThis.camera` for the guest about to be evaluated: `qr`
 * publishes the viewfinder ops (camera.qr), `photo` adds the photo store
 * (camera.photo). Neither installs nothing. Textures live in `core`. */
esp_err_t pocketjs_camera_install(pocketjs_guest_t *guest,
                                  pocketjs_ui_core_t *core, bool qr,
                                  bool photo);

/** The owner task between turns: copy the newest preview frame into the
 * camera texture, upload decoded photos and publish their events. */
void pocketjs_camera_present(void);

/** Guest teardown: stop capture and drop queued photo work. The guest's core
 * is about to go, and its textures with it. */
void pocketjs_camera_teardown(void);

#ifdef __cplusplus
}
#endif
