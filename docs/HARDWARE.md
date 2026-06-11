# Hardware notes (U60Pro screen)

These are interface facts about the device, recorded so the code is
understandable. They were determined by probing standard Linux interfaces on
the running device — **no vendor binary or asset is copied into this repo.**

## Display
- Node: `/dev/dri/card0` (standard DRM/KMS).
- Panel: ~320x480, **RGB565 (16bpp)**.
- Update model: command-mode panel — pixels are pushed with
  `DRM_IOCTL_MODE_DIRTYFB` after writing the dumb buffer (low/`vrefresh=1`,
  not a continuously-scanned display).
- Mounting: rotated 180° relative to framebuffer scan order, so we draw flipped
  (`DEVUI_ROTATE_180`).
- connector / crtc ids: **auto-enumerated at runtime** via
  `DRM_IOCTL_MODE_GETRESOURCES` / `GETCONNECTOR` / `GETENCODER`. Observed values
  on this unit were connector 31 / crtc 34; they live only as last-resort
  fallbacks in `include/devui_config.h` and are not required for operation.

## Touch
- Node: a `/dev/input/event*` evdev device (observed: `event3`) — **auto-probed**
  by scanning for `ABS_MT_POSITION_X/Y` (multitouch) or `ABS_X/Y`.
- Raw axis ranges are read with `EVIOCGABS` and scaled to screen pixels.
- The 180° display rotation is mirrored onto touch coords
  (`DEVUI_TOUCH_ROTATE_180`); swap/invert tunables exist for calibration.

## Device data (for real screens)
The device runs OpenWRT. Live state (signal, WiFi, connected clients, battery,
SMS) is exposed through **ubus** services (`zwrt_*` namespaces) and **uci**
config — both standard OpenWRT mechanisms. Query them via the `ubus`/`uci` CLI
or libubus/libubox. This project deliberately does **not** link the vendor
`libzte_*.so` libraries.

## Clean-room policy
The original `zte_topsw_devui`, `libzte_*.so`, fonts, PNGs, and `usr_ui/`
resources are used **only** for local interface analysis and are excluded by
`.gitignore`. This project is an independent reimplementation built on public
Linux/OpenWRT interfaces; it is not affiliated with or endorsed by ZTE.
