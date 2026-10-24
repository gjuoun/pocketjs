# Host services in C

Portable C that native hosts compile directly into their binaries: the
services a console offers its guests besides the UI. Each source implements
one contract and calls the platform only through `host_services.h`, which
every host implements once.

| Source | Contract | What it does |
| --- | --- | --- |
| `link.c`, `link.h` | `contracts/spec/link.ts`, `contracts/spec/net.ts` | The Pocket Link client: beacons, the handshake, pairing, catalog and install requests, and `net.http` through the companion. One state machine pumped once per frame; every socket is non-blocking. |
| `link_proto.c`, `link_proto.h` | `contracts/spec/link.ts` | The socket-free half: constants, beacon and frame layouts, the pairing URI, HMAC proofs. |
| `launcher.c`, `launcher.h` | spec ops 39 to 41, `docs/LAUNCHER.md` | The app table of a console with installed apps, the switch request, SELECT summons, `resume` and `lastError`. The host's main loop performs the switch. |
| `fs.c`, `fs.h` | `contracts/spec/fs.ts` | One guest's `data.fs` tree: the nine ops, quota, and replacement of a file on FAT, which refuses a rename onto an existing name. |
| `jscan.c`, `jscan.h` | | A bounded JSON reader and a growable JSON writer for the host's own records. |
| `sha256.c`, `sha256.h` | | SHA-256, HMAC-SHA256, hex, and the slot of an app id (the first 16 hex digits of SHA-256 of the id). |

## Hosts

| Host | Build | `host_services.h` implementation |
| --- | --- | --- |
| Nintendo 3DS | `hosts/3ds/Makefile` (`SHARED`) | `hosts/3ds/src/services_host.c`: libctru clocks and `ps:ps` randomness, the CFG owner name, `soc:U`, the SD card runtime slots (`runtime.c`), the agent channel (`agent.c`) |
| ESP-IDF | `hosts/esp-idf/components/pocketjs_system` | `src/services_host.c` in that component: `esp_timer`, `esp_fill_random`, lwIP, the `/pocket` store |

The build machine compiles `fs.c`, `jscan.c`, `link_proto.c` and `sha256.c`
in `tests/fs-3ds.test.ts`, `tests/3ds-link.test.ts` and
`tests/3ds-runtime-state.test.ts`.
