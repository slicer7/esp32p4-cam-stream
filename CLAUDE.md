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

**Builds clean, but has never run on the board.** As of 2026-09-01 the project
compiles without warnings against ESP-IDF v5.5.5 (app binary ~951 KB, 77% of the
4 MB partition free) and all managed components resolve: `esp_video` 2.4.1,
`esp_cam_sensor` 2.4.0, `esp_hosted` 3.0.6, `esp_wifi_remote` 1.6.4. No frame
has ever been captured. Three things are unconfirmed and are the first suspects
for any runtime failure:

1. **SCCB pins.** Default GPIO7 (SDA) / GPIO8 (SCL) — that is Espressif's
   reference board. Waveshare's schematic may differ. Configurable in menuconfig.
2. **CSI vs DSI connector.** The board has two identical-looking 15-pin sockets.
3. **Camera clock.** OV5647 needs 24 MHz. Pi Camera v1.3 and most clones have
   their own oscillator; if this one does not, the sensor never answers on I2C.

## Toolchain — read this before building

**ESP-IDF v5.5.5.** The project targets `>=5.5` and is verified against 5.5.5.

The desktop machine has two IDF versions installed by EIM: `C:\esp\v5.5.5` and
`C:\esp\v6.0.2`, tools shared in `C:\Espressif\tools`. **EIM has 6.0.2 selected**,
so anything that follows EIM's own selection picks the wrong one. VS Code is
pinned to 5.5.5 explicitly (see below). Do not assume the active version — check.

### Three environment traps, all of which cost real time already

1. **`ESP_IDF_VERSION` must be exported.** `esp_wifi_remote`'s Kconfig does
   `orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"`. That variable is set by IDF's
   *activation* step, not by the CMake build. Build from a shell where it is
   unset and the orsource silently resolves to a nonexistent file, leaving
   `CONFIG_WIFI_RMT_*` undefined — the build then fails deep inside esp_hosted's
   `eh_host_wifi.c` with errors that look like a dependency conflict but are not.
   Do not "fix" `idf_component.yml` in response; fix the environment.

2. **EIM's activation script needs PowerShell 7.** `C:\Espressif\tools\
   Microsoft.v5.5.5.PowerShell_profile.ps1` does not parse under Windows
   PowerShell 5.1 (parse error at the `Register-IdfCompletions` closing brace),
   and only 5.1 is installed. Either install pwsh 7 or set the environment
   manually — the script's `$env_var_pairs` block lists exactly what is needed.

3. **Do not set `IDF_COMPONENT_LOCAL_STORAGE_URL`.** EIM's activation script
   points it at a local offline mirror, which will not contain `esp_video`.
   It is deliberately omitted from the VS Code config for that reason.

### VS Code setup

Extension 2.2.0 does **not** use `idf.espIdfPath` / `idf.toolsPath` /
`idf.customExtraPaths` — those keys no longer exist. It reads `idf.currentSetup`
(an IDF *path*) plus `idf.customExtraVars`, falling back to EIM's manifest at
`C:\Espressif\tools\eim_idf.json`. With `idf.currentSetup` empty it hands
`undefined` to a Node path call and every command dies with "The 'path' argument
must be of type string." With `idf.customExtraVars` empty it silently defaults
`IDF_PATH` to `%USERPROFILE%\esp\esp-idf`, which does not exist here.

`.vscode/settings.json` is gitignored (absolute paths). Recreate it per machine
with `idf.currentSetup`, and `idf.customExtraVars` carrying at least `IDF_PATH`,
`IDF_TOOLS_PATH`, `IDF_PYTHON_ENV_PATH` and `ESP_IDF_VERSION`. The extension's
"Use Existing Setup" wizard is supposed to write these but failed partway here,
leaving only `idf.gitPathWin`.

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

Two IDF API details `main.c` depends on, both confirmed against 5.5.5:

- `esp_cache_get_alignment()` is declared **only** in the private header
  `esp_private/esp_cache_private.h`, not in `esp_cache.h`. If a version bump
  moves or removes it, replace the call with a fixed 64-byte constant rather
  than dropping the alignment check.
- `httpd_err_code_t` has **no 503 entry**. The "another client is streaming"
  path therefore sets the status line directly with `httpd_resp_set_status()`
  instead of `httpd_resp_send_err()`.

`CONFIG_PARTITION_TABLE_OFFSET` is moved to `0xA000` in `sdkconfig.defaults`.
The P4 bootloader with PSRAM and esp_hosted support is 0x60c0 bytes, which does
not fit the 0x6000 the default 0x8000 offset allows. Do not move it back.
