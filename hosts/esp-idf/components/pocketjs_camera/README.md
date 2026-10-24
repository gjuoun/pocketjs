# pocketjs_camera

`globalThis.camera` (contracts/spec/camera.ts) for the ESP-IDF QuickJS guest:
the camera.qr viewfinder with QR decoding and the camera.photo store.

- Public header: `pocketjs/camera.h`
- Target: ESP32-P4 (PPA, JPEG codec)
- Dependencies: `esp_video`, `pocketjs_guest`, `pocketjs_ui_core`

The board supplies the sensor's V4L2 device, a hook that starts the sensor,
the sensor's rotation and the storage directory. A capture task streams
RGB565 from the ISP and the PPA turns each frame into a 320x240 PSM_5650
preview: **a centered 4:3 crop scaled by a sixteenth-step factor, rotated
upright and mirrored for the inner flag**. The owner task copies the newest
preview into one 512x256 core texture between turns
(`pocketjs_camera_present`), so the guest draws an ordinary image handle. A
decode task runs quirc (`engine/host-services/quirc`) on the preview's luma
at most every 250 ms.

Stills are the same crop at 640x480, encoded by the JPEG codec to
`<storage>/DCIM/100POCKT/PKT_nnnn.JPG`. Thumbnails (78 px in a 128 texture)
and full views (fitted to 320x240) are decoded by the codec and box-filtered
by `engine/host-services/photos_image.c`; thumbnails are cached under
`<storage>/photos/thumbs`. Long file names need FAT LFN or LittleFS.

Call `pocketjs_camera_install` before the guest is evaluated and
`pocketjs_camera_teardown` before its core is destroyed.
