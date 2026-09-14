# STATUS.md

The shared, living state of this project. **Both machines read this at the start
of a session and update it at the end.** CLAUDE.md is the stable reference (how
things work and why); this file is the moving part (where things stand right now).

Keep it short. Rewrite "Current state" and "Next up" in place — don't let them
grow into a history. Append one line per session to the log at the bottom.

## Current state

- **Stream:** working on hardware. 1280x960 full-FOV binned mode, RGB565, hardware
  JPEG, served over Wi-Fi. Confirmed from a boot log on 2026-09-03.
- **Chip revision config:** confirmed correct (P4 rev v1.3 boots, min rev v1.0).
- **ISP brightness:** 24, applied at boot. Picture was still described as "a
  little on the dark side" before this landed; not re-checked since.
- **Object detection:** builds and is committed (YOLO11n 320, 80 COCO classes).
  First flash panicked on a stale packed model; fixed by deleting
  `build/espdl_models/`. **Not yet confirmed that the fixed build boots with
  detection running** — no boot log since the fix.

## Next up

1. Confirm detection boots: look for `detect: ready: 1280x960` and inference
   timings in the monitor, and boxes in the browser.
2. Check boxes land on the objects (1280x960 -> 320x320 mapping is assumed, not
   verified).
3. Re-judge brightness at ISP brightness 24. If still dark, the real fix is the AE
   target in the sensor IPA JSON (see CLAUDE.md, "Image is dark").

## Open questions

- What does inference actually cost per frame, and how much does it drop the
  stream's frame rate?

## Machines

| Machine | Notes |
|---|---|
| Desktop | Windows 11. ESP-IDF v5.5.5 at `C:\esp\v5.5.5`, tools `C:\Espressif\tools`. Board on COM3. Fully set up and flashing. |
| Laptop | Windows 11, Windows PowerShell 5.1 only (no pwsh). Clone at `C:\Users\lukes\Claude\esp32p4-cam-stream`. ESP-IDF v5.5.5 at `C:\esp\v5.5.5\esp-idf` (EIM also has v6.0.1 selected, and `C:\esp\v6.1` exists — ignored). Tools `C:\Espressif\tools`, python venv `C:\Espressif\tools\python\v5.5.5\venv`, same tool version folders as the desktop plus `qemu-riscv32`/`qemu-xtensa` `esp_develop_9.2.2_20260417`. VS Code extension 2.2.0; `.vscode/settings.json` written from the example, plus `IDF_COMPONENT_CACHE_PATH=C:\Espressif\cmc` (long-path trap, see CLAUDE.md). **Builds** (resolved `esp_hosted` 3.0.7, not 3.0.6). Not flashed on this machine. Wi-Fi credentials still placeholders — set them in menuconfig. |

## Session log

Newest at the bottom. One line each: date, machine, what changed.

- 2026-09-01 desktop — Project created: MJPEG stream firmware, pushed to GitHub.
- 2026-09-01 desktop — Toolchain fixed (IDF 5.5.5, VS Code extension config); first clean build.
- 2026-09-02 desktop — Stream confirmed working on hardware. Full FOV (1280x960 binned), chip rev v1.3 config.
- 2026-09-03 desktop — Sensor mode actually compiled in; object detection and ISP brightness added; stale packed-model panic diagnosed.
- 2026-09-14 desktop — Added STATUS.md and the cross-machine sync protocol.
- 2026-09-14 laptop — First setup: cloned, IDF 5.5.5 env + `.vscode/settings.json`, `set-target esp32p4` + clean build (builds; not flashed). Hit and documented the Windows 260-char path trap in the component cache.
