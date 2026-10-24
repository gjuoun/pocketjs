#pragma once

/*
 * The system services of a PocketJS launcher console on ESP-IDF: installed
 * apps and switching between them, the Pocket Link to a paired companion,
 * net.http over the link, a guest's data.fs tree and the Wi-Fi radio. The
 * state machines are engine/host-services; this component keeps the store,
 * the radio and the QuickJS bindings.
 *
 * The store lives under one mounted directory (`root`):
 *
 *   <root>/link/                        device id, paired companions
 *   <root>/apps/<slot>/package.pocket   the slot's installed package
 *   <root>/apps/<slot>/app.txt          the launcher's cached facts
 *   <root>/apps/<slot>/data/            the guest's data.fs tree
 *
 * where <slot> is the first 16 hex digits of SHA-256 of the app id. The
 * launcher is the package the firmware embeds; it gets a slot for its data
 * like any app.
 *
 * Threads: everything except pocketjs_system_start runs on the task that
 * runs the guest's turns.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pocketjs/guest.h"
#include "pocketjs/package.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  size_t struct_size;
  /** The firmware's host contract: installs must select a variant for it. */
  const pocketjs_package_host_contract_t *contract;
  /** The launcher package the firmware embeds. */
  const void *launcher;
  size_t launcher_size;
  /** A mounted directory holding the store. */
  const char *root;
  /** The console's name and model in the link handshake. */
  const char *device_name;
  const char *device_model;
  /** Bring the Wi-Fi radio up (esp_wifi through esp_wifi_remote on chips
   * without a radio of their own). */
  bool wifi;
} pocketjs_system_config_t;

/** Open the store and the launcher package, start the link and the radio. */
esp_err_t pocketjs_system_start(const pocketjs_system_config_t *config);

/** Mount the FAT partition `label` (wear levelled) at `root`. */
esp_err_t pocketjs_system_mount_flash(const char *label, const char *root);

/** One package ready to boot. Its byte spans stay valid until it is freed. */
typedef struct pocketjs_system_app pocketjs_system_app_t;

/** The launcher, or the next app the launcher asked for. */
esp_err_t pocketjs_system_load_launcher(pocketjs_system_app_t **out_app);
/** The app a switch request names; false when none is pending. */
bool pocketjs_system_take_switch(char slot[17], bool *launcher);
esp_err_t pocketjs_system_load_slot(const char *slot, pocketjs_system_app_t **out_app);
void pocketjs_system_app_free(pocketjs_system_app_t *app);
const pocketjs_package_variant_t *pocketjs_system_app_variant(const pocketjs_system_app_t *app);
bool pocketjs_system_app_is_launcher(const pocketjs_system_app_t *app);

/** Before the guest evaluates `app`: record it as the running app, bind its
 * data.fs tree when its plan declares one, and install `globalThis.link`,
 * `net`, `wifi`, `fs` and the launcher ops on `ui` (mount the ui binding
 * first). */
esp_err_t pocketjs_system_bind(pocketjs_guest_t *guest, const pocketjs_system_app_t *app);
/** After the guest is destroyed: drop its link requests and its tree. */
void pocketjs_system_unbind(void);

/** Once per frame, before the guest's turn. */
void pocketjs_system_pump(void);
/** A switch is requested and not yet taken. */
bool pocketjs_system_switch_pending(void);
/** SELECT in an app other than the launcher: switch to the launcher. */
void pocketjs_system_summon(void);
bool pocketjs_system_launcher_running(void);
/** The running app failed: the launcher shows `message` once. */
void pocketjs_system_report_failure(const char *message);

#ifdef __cplusplus
}
#endif
