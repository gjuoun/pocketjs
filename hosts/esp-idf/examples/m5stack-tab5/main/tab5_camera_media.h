#pragma once

#include "pocketjs/guest.h"
#include "pocketjs/package.h"
#include "pocketjs/ui_core.h"

/* `globalThis.camera` and `globalThis.media` on the Tab5's SC202CS camera
 * and ES8388 speaker, for the guests whose plans admit them. */

/* Before the guest is evaluated. */
void tab5_camera_media_install(pocketjs_guest_t *guest,
                               pocketjs_ui_core_t *core,
                               const pocketjs_package_variant_t *app);
/* The owner task, before each turn. */
void tab5_camera_media_present(void);
/* Before the guest's core is destroyed. */
void tab5_camera_media_teardown(void);
