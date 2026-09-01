# CLAUDE.md

Context for Claude Code working on this repo. Read this before touching anything.

## What this is

Firmware for an **ESP32-P4** that captures from an **OV5647** camera over MIPI-CSI
and serves it to a PC as an MJPEG stream over Wi-Fi.

```
OV5647 ──MIPI-CSI 2-lane RAW8──▶ P4 ISP ──RGB565──▶ P4 hardware JPEG encoder
       ──▶ esp_http_server (multipart/x-mixed-replace) ──▶ browser / ffplay / OpenCV
```

Design rule: **keep the CPU out of the pixel path.** The ISP does debayering and
the JPEG peripheral does compression. `main.c` only moves buffer pointers and
feeds lwIP. Any change that adds a per-pixel loop in C is the wrong direction —
push it into the ISP or the encoder instead.

## Hardware

| | |
|---|---|
| Board | Waveshare ESP32-P4-WIFI6 (ESP32-P4 + on-board ESP32-C6 for Wi-Fi) |
| Camera | OV5647, Raspberry Pi Zero style module, 15-pin 1.0 mm FFC |
| Link to camera | MIPI-CSI, 2 lane, RAW8; SCCB on I2C port 0 |
| Wi-Fi | ESP32-C6 over SDIO via ESP-Hosted; `esp_wifi_remote` forwards the normal `esp_wifi_*` API |

**Not verified on real hardware yet.** Nothing in this repo has been run on the
board. Three things are unconfirmed and are the first suspects for any failure:

1. **SCCB pins.** Default GPIO7 (SDA) / GPIO8 (SCL) — that is Espressif's
   reference board. Waveshare's schematic may differ. Configurable in menuconfig.
2. **CSI vs DSI connector.** The board has two identical-looking 15-pin sockets.
3. **Camera clock.** OV5647 needs 24 MHz. Pi Camera v1.3 and most clones have
   their own oscillator; if this one does not, the sensor never answers on I2C.

## Toolchain — read this before building

**ESP-IDF v5.5 (stable).** The project targets `>=5.4`.

The desktop machine had **v6.1-beta1** installed at `C:\esp\v6.1-beta1\esp-idf`
(tools in `C:\Espressif\tools`). That is a pre-release, and `esp_video` /
`esp_hosted` have no release validated against it — dependency resolution is
expected to fail. Install 5.5 alongside it via the EIM GUI and select that.

Multiple IDF versions coexist fine; the VS Code extension switches between them.

### VS Code setup (this bit already went wrong once)

The Espressif extension needs configuring before any command works. If
`settings.json` has no `idf.espIdfPath` / `idf.toolsPath` / python path, every
ESP-IDF command errors out.

Fix: Ctrl+Shift+P → **ESP-IDF: Configure ESP-IDF Extension** → **Use Existing
Setup**. It reads `C:\Espressif\tools\eim_idf.json` and writes the paths itself.
Do not hand-edit these — EIM's python layout (`tools\python\<ver>\venv\`) is not
the classic `python_env\` one and is easy to get wrong.

`.vscode/` is gitignored because those paths are machine-specific. Run the
"Use Existing Setup" step once on each machine.

Use the extension's buttons or **ESP-IDF: Open ESP-IDF Terminal**. A plain
PowerShell window has no `idf.py` until the export script has run.

## Build

```bash
idf.py set-target esp32p4
idf.py menuconfig     # Wi-Fi SSID/password under "P4 Camera Stream Configuration"
idf.py build flash monitor
```

Wi-Fi credentials live in menuconfig, so they land in `sdkconfig` — which is
**gitignored**. They will not leak into the repo, and they do not travel between
machines. Set them again after cloning.

## Layout

| Path | |
|---|---|
| `main/main.c` | everything: camera init, JPEG encode, Wi-Fi, HTTP server |
| `main/Kconfig.projbuild` | pins, resolution, JPEG quality, pixel format, Wi-Fi creds |
| `main/idf_component.yml` | `esp_video`, `esp_wifi_remote`, `esp_hosted` |
| `sdkconfig.defaults` | PSRAM (hex/200 MHz), 16 MB flash, OV5647 on, lwIP tuning |
| `partitions.csv` | 4 MB app on 16 MB flash |

`main.c` is sectioned by banner comments: Wi-Fi → Camera → JPEG encoding →
HTTP server → `app_main`. Keep that order and that style if you extend it.

## Endpoints

- `/` — HTML page wrapping the stream
- `/stream` — MJPEG, `multipart/x-mixed-replace`. One client at a time; a second
  gets 503. This is deliberate: there is one camera and one encoder.
- `/jpg` — single frame, takes the same mutex with a 2 s timeout

## Deliberate choices worth not "fixing"

- **Format enumeration at boot.** `camera_log_capabilities()` logs every pixel
  format and frame size the driver accepts. It looks like debug noise; it is the
  fastest way to find a working resolution on unverified hardware. Keep it.
- **`VIDIOC_S_FMT` falls back** to the sensor default instead of aborting, so a
  wrong resolution in menuconfig degrades rather than bricks the boot.
- **One frame is encoded before Wi-Fi starts,** so an imaging failure cannot be
  mistaken for a network failure.
- **Cache-alignment check in `capture_jpeg()`.** The hardware encoder needs a
  cache-line-aligned source. V4L2 buffers normally already are; the memcpy path
  is a fallback for driver versions where they are not. Do not delete it as dead
  code without testing on the actual `esp_video` version in use.
- **RGB565 vs RGB888** is a menuconfig choice, not a constant. RGB888 exists as
  the escape hatch if the ISP and encoder disagree on RGB565 byte order
  (symptom: red and blue swapped).

## Known-fragile

`esp_video`'s API has churned across releases. `esp_video_init_csi_config_t`
field names and the `/dev/video0` device naming are the likely breakage points
on a version bump. `idf_component.yml` pins `"*"` deliberately — if the build
breaks there, check the installed component's headers under
`managed_components/` rather than guessing.
