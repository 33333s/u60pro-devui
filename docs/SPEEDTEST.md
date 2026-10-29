# Optional speedtest integration

This document describes the optional `better-speedtest` integration in `zwrt-devui`.

## Current model

- DevUI does not bundle the `better-speedtest` binary.
- DevUI ships a second-level speedtest template at `ui/subpages/speedtest.html`.
- If `/data/plugins/better-speedtest/better-speedtest` exists and is executable, the "更多功能" page shows a `网络测速` tile.
- Tapping the tile opens the speedtest second-level page; it does not add another top-level swipe page.
- Removing the speedtest backend hides the entry button automatically.
- Locked preview mode reuses the first page but hides speedtest controls and native speedtest widgets.
- The old optional `07-speedtest.html` standalone swipe page has been removed. Do not ship or install it for the current UX.

## Runtime paths

- Binary: `/data/plugins/better-speedtest/better-speedtest`
- Log: `/tmp/better-speedtest.log`
- DevUI config: `/data/plugins/zwrt-devui/devui.conf`
- Loop flag: `/tmp/better-speedtest.loop`
- Loop pid: `/tmp/better-speedtest.loop.pid`

DevUI only stores small UI preferences in `devui.conf`:

- `st_src`
- `st_dir`
- `st_dur`

DevUI does not rewrite `better-speedtest`'s own `config.json`.

## Lock screen behavior

The screen-lock preview (`g_lock_state == 1`) intentionally keeps using the first
page as a read-only signal overview, but it must not expose speedtest controls.

- The "更多功能" speedtest tile is not reachable from locked preview mode.
- Native gauge/chart drawing is skipped while locked.
- `speedtest_poll()` still runs, so backend detection and an already-running
  speedtest state remain fresh after unlock.

## Actions

The speedtest second-level page uses these `act:` commands:

- `act:stpage`: open `ui/subpages/speedtest.html` when the backend is available.
- `act:ststart`: run `better-speedtest test --json` with the selected source, direction, and duration.
- `act:ststop`: kill the running `better-speedtest` process and reset the speedtest state.
- `act:stsrc:<mode>`: select source, currently `auto`, `cnspeed`, `ookla`, or `cdn`.
- `act:stdir:<mode>`: select direction, currently `both`, `dl`, or `ul`.
- `act:stdur:<sec>`: select duration, currently `10`, `15`, `20`, or `0`; `0` means loop until the user taps stop.

`act:sttoggle` and `act:stclose` may remain accepted for compatibility with older templates, but the current UI should use the second-level page.

## Rendering notes

Speedtest visuals are hybrid-rendered:

- HTML/CSS lays out the second-level page, option buttons, loop warning, empty gauge placeholder, chart placeholders, and log card.
- Native drawing in `htmlmain.c` paints the circular gauge, pointer, live speed number, and two line charts.
- Native drawing is clipped below the 26 px status bar so dragging the page cannot paint over the status bar.
- Horizontal swipe previews render pages at the current scroll position and draw native chart layers into the preview buffers, so charts do not disappear during page swipes.
- Chart under-fill follows the curve color in both dark and light themes.

## Install and uninstall convention

Install `better-speedtest` independently:

```sh
mkdir -p /data/plugins/better-speedtest
cp better-speedtest /data/plugins/better-speedtest/better-speedtest
chmod +x /data/plugins/better-speedtest/better-speedtest
```

No extra UI file copy is required for the current second-level speedtest page. The DevUI binary and default UI templates already contain the optional entry point; it only appears when the backend binary is present.

To uninstall the optional speedtest backend:

```sh
rm -f /data/plugins/better-speedtest/better-speedtest
rm -f /tmp/better-speedtest.log
```

If an old deployment still has standalone page files, remove them so the deleted page does not reappear:

```sh
rm -f /data/plugins/zwrt-devui/ui/07-speedtest.html
rm -f /data/plugins/zwrt-devui/ui/speedtest.css
```

## Public repository boundary

The DevUI repository may document and implement the optional integration, but it should not include private device logs, local SSH targets, local screenshots, framebuffer dumps, or the `better-speedtest` binary itself.

The `zwrt-datad` repository remains unrelated to speedtest execution. The speedtest backend is an independent optional plugin under `/data/plugins/better-speedtest/`.
