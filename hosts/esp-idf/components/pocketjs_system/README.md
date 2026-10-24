# pocketjs_system

The system services of a launcher console: installed apps and switching
between them, the Pocket Link to a paired companion, `net.http` through the
companion, a guest's `data.fs` tree and the Wi-Fi radio.

- Public header: `pocketjs/system.h`
- Shared state machines: `engine/host-services` (`link.c`, `launcher.c`,
  `fs.c`), compiled from the repository
- Targets: ESP32-P4 and ESP32-S3. On the ESP32-P4, `esp_wifi` comes from
  `espressif/esp_wifi_remote` and `espressif/esp_hosted`, which the product
  lists with its coprocessor's pins

## Store

`pocketjs_system_start` takes one mounted directory:

| Path | Contents |
| --- | --- |
| `link/` | the device id and one file per paired companion |
| `apps/<slot>/package.pocket` | the slot's installed package; its hash is the file's footer |
| `apps/<slot>/app.txt` | the launcher's cached facts (output, id, title, version) |
| `apps/<slot>/data/` | the guest's `data.fs` tree, quota from `applet.store` |

`<slot>` is the first 16 hex digits of SHA-256 of the app id. An install
streams into `apps/<slot>/link-upload.pocket`, is admitted (footer, FNV-1a64,
a variant for the firmware's host contract) and replaces `package.pocket`.
`pocketjs_system_mount_flash` mounts a wear-levelled FAT partition when no
card is available.

## Guest lifecycle

The firmware loads the launcher (`pocketjs_system_load_launcher`), mounts the
ui binding, calls `pocketjs_system_bind` and evaluates the guest. Each frame,
before the turn, it calls `pocketjs_system_pump`; SELECT outside the launcher
calls `pocketjs_system_summon`. When `pocketjs_system_switch_pending`
becomes true, the firmware stops the runner, destroys the guest, calls
`pocketjs_system_unbind`, takes the target with `pocketjs_system_take_switch`
and boots it.

## Guest namespaces

| Global | Contract |
| --- | --- |
| `ui.appTable`, `ui.appLaunch`, `ui.appShot` | spec ops 39 to 41 |
| `link` | `contracts/spec/link.ts` |
| `net` | `contracts/spec/net.ts` over the link |
| `wifi` | `contracts/spec/wifi.ts`, with `scan`, `join` and `forget` |
| `fs` | `contracts/spec/fs.ts`, when the plan's features hold `data.fs` |

The radio keeps three saved networks in NVS (namespace `pocketwifi`), joins
the most recently used at start, and saves a joined network once it has an
address.
