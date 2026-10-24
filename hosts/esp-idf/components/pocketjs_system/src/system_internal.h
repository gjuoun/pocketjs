#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "pocketjs/guest.h"

/* wifi.c: `globalThis.wifi` (contracts/spec/wifi.ts) over esp_wifi. The radio
 * comes up on its own task; every call below is safe before it has. */
void wifi_start(void);
const char *wifi_status_json(void);
bool wifi_set_enabled(bool on);
bool wifi_connect(int slot);
bool wifi_scan(void);
bool wifi_join(const char *ssid, size_t ssid_length, const char *passphrase, size_t passphrase_length);
bool wifi_forget(int slot);
/* An address is assigned: sockets reach the LAN. */
bool wifi_online(void);
void wifi_address(char *out, size_t capacity);

/* qjs.c: install the system namespaces into the guest's realm. */
esp_err_t system_qjs_install(pocketjs_guest_t *guest);
