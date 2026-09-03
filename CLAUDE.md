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

**Working on hardware.** As of 2026-09-02 the live stream runs end to end on the
real board: capture, ISP, hardware JPEG, Wi-Fi, browser. Built against ESP-IDF
v5.5.5 with `esp_video` 2.4.1, `esp_cam_sensor` 2.4.0, `esp_hosted` 3.0.6,
`esp_wifi_remote` 1.6.4.

That settles what used to be the three open questions: the SCCB pins at GPIO7/8
are right for this board, the CSI connector is the correct one of the two 15-pin
sockets, and the camera module self-clocks (no host 24 MHz needed).

**The chip is ESP32-P4 revision v1.3.** IDF defaults to requiring rev v3.1 and
the firmware then refuses to boot. `sdkconfig.defaults` sets
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and `CONFIG_ESP32P4_REV_MIN_100=y`
(minimum v1.0). Do not remove these. A side effect: the 250 MHz PSRAM option
disappears from menuconfig, because it requires rev >= 3.0. 200 MHz is unaffected
and is what this project uses.

**The camera is a 120 degree wide-angle OV5647.** See the FOV notes below — the
sensor mode determines how much of that 120 degrees you actually get.

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

`idf.customExtraVars` must also carry a **`PATH`** key listing every tool
directory. The extension validates the toolchain by looking for each required
tool on *that* PATH, not the system one; without it you get "ESP-IDF Setup from
environment variables is not valid: Missing required tools: xtensa-esp-elf-gdb,
riscv32-esp-elf-gdb, ..." even though every tool is installed and working.

**The extension rewrites this file and has been observed silently dropping
`IDF_PATH`, `IDF_TOOLS_PATH` and `IDF_PYTHON_ENV_PATH` from `customExtraVars`.**
If ESP-IDF commands start failing after they previously worked, re-read
`.vscode/settings.json` before assuming anything else changed.

`.vscode/settings.json` is gitignored (absolute paths). `docs/vscode-settings.example.json`
is a working copy from the desktop machine — copy it and adjust the versions in
the paths to match what EIM installed on the other machine. The extension's
"Use Existing Setup" wizard is supposed to generate all of this but failed
partway here, leaving only `idf.gitPathWin`.

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
- **1280x960 is chosen for field of view, not for resolution.** Do not "optimise"
  it down to 800x640 for speed, and do not raise it to 1920x1080 assuming bigger
  is better — see below.

## Field of view — why the capture mode is 1280x960

The lens is 120 degrees, but the OV5647 modes read different parts of the
2624x1954 array, and all but one crop it. Windows are from the sensor's
0x3800-0x3807 registers in `managed_components/espressif__esp_cam_sensor/
sensors/ov5647/private_include/`:

| Mode | Array window | Coverage |
|---|---|---|
| **1280x960** | x 24..2600, y 12..1944 | **98%, 2x2 binned — full FOV** |
| 800x640 | x 500..2623, y 0..1953 | 81% of the width |
| 1920x1080 | x 348..2275, y 434..1521 | 73% wide, 56% tall — narrowest |

So 1920x1080 has the most pixels and the *worst* field of view; it is the classic
1080p centre crop. Width comes from the binned mode, not from resolution. The
cost of 1280x960 is 2.4 MB per RGB565 frame buffer (two of them) plus a 1.2 MB
JPEG output buffer, and a lower frame rate — 2.4x the pixels of 800x640 to encode
and push over Wi-Fi.

If someone asks for a wider view than this, the mode list is exhausted; the
answer is a different lens, not a different setting.

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
