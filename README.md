# ESP32-P4 + OV5647 → MJPEG Wi-Fi stream

Streams the Raspberry Pi camera (OV5647, MIPI-CSI) from a Waveshare ESP32-P4-WIFI6
board to your PC over HTTP.

```
OV5647 ──MIPI-CSI 2-lane RAW8──▶ P4 ISP ──RGB565──▶ P4 hardware JPEG encoder
       ──▶ esp_http_server (multipart/x-mixed-replace) ──▶ browser / ffplay / OpenCV
```

Everything heavy is done in hardware: the ISP does the debayering, the JPEG
peripheral does the compression. The CPU only moves pointers and pushes bytes
into lwIP.

## 1. Requirements

- ESP-IDF **v5.4 or newer** (v5.5 recommended — better ESP-Hosted integration).
  In VS Code: *ESP-IDF: Configure ESP-IDF Extension* → install v5.5.
- The ESP32-C6 on the board must be running ESP-Hosted **slave** firmware
  (Waveshare ships it preflashed). See §6 if Wi-Fi init fails.

## 2. Wiring

Plug the camera FFC into the **CSI** connector (not DSI — they look identical).
Contacts face the board's connector contacts; the blue stiffener faces away.
Both connectors on this board are the 15-pin / 1.0 mm Pi-style type, so a
mis-plug is easy — double-check the silkscreen.

The OV5647 module needs a 24 MHz clock. Pi Camera v1.3 boards and most clones
carry their own oscillator; if yours does not, the sensor will never answer on
I2C and `esp_video_init` will fail.

## 3. Configure

```bash
idf.py set-target esp32p4
```

```bash
idf.py menuconfig
```

Set under **P4 Camera Stream Configuration**:

| Option | Notes |
|---|---|
| Wi-Fi SSID / password | 2.4 GHz or 5 GHz, the C6 does both |
| Camera SCCB SDA / SCL | default 7 / 8 — **verify against the Waveshare schematic** |
| Capture width / height | default 1280×960 — the only mode with the lens's full field of view (see below) |
| JPEG quality | 80 is a good starting point |
| ISP output format | RGB565 (fast) or RGB888 (use if colours look wrong) |

Also check **Component config → Espressif Camera Sensors Configurations** that
`OV5647` is enabled (`sdkconfig.defaults` already sets it).

## 4. Build, flash, monitor

```bash
idf.py build flash monitor
```

The board prints its address:

```
I (5231) p4cam:  Stream ready:  http://192.168.1.57/
```

## 5. View on the PC

- **Browser** — open `http://<ip>/`. Chrome and Firefox render MJPEG natively.
- **ffplay** —
  ```bash
  ffplay -fflags nobuffer -flags low_delay -f mjpeg http://192.168.1.57/stream
  ```
- **OpenCV** —
  ```python
  import cv2
  cap = cv2.VideoCapture("http://192.168.1.57/stream")
  while True:
      ok, frame = cap.read()
      if not ok:
          break
      cv2.imshow("p4", frame)
      if cv2.waitKey(1) == 27:
          break
  ```
- **Single frame** — `http://<ip>/jpg`

Only one `/stream` client at a time; a second gets 503. `/jpg` waits its turn.

## 6. Troubleshooting

**`esp_video_init failed` / sensor not detected**
The camera is not answering on SCCB. In order of likelihood: FFC in the DSI
socket instead of CSI, FFC upside down or not fully latched, wrong I2C pins in
menuconfig, or no 24 MHz clock reaching the sensor.

**`VIDIOC_S_FMT ... rejected`**
The requested resolution is not a mode compiled into the OV5647 driver. The
boot log enumerates every supported format and size — pick one of those. The
code falls back to the sensor default automatically, so you still get a stream.

**Red and blue swapped / odd tint**
Switch the ISP output format to RGB888 in menuconfig. It costs 50% more PSRAM
bandwidth per frame but sidesteps any RGB565 byte-order mismatch between the
ISP and the JPEG encoder.

**`esp_wifi_init` fails or no AP found**
The C6 co-processor firmware is missing or mismatched with your IDF version.
Follow Waveshare's wiki for the ESP32-P4-WIFI6 to reflash the ESP-Hosted slave
firmware onto the C6, and check the SDIO pin assignment under
*Component config → ESP-Hosted* matches the board schematic.

**Narrow field of view**
The lens is 120°, but the sensor mode decides how much of it you see. Only
1280×960 (2×2 binned) reads essentially the whole array. 800×640 crops to 81% of
the width, and 1920×1080 — despite having the most pixels — is a centre crop at
73% wide and 56% tall, the narrowest of the three. Use 1280×960, the default.

**Low frame rate**
Drop JPEG quality first. Dropping to 800×640 also works but costs field of
view, so prefer quality. Wi-Fi
throughput through the SDIO link, not the encoder, is usually the limit.
Watch the `x.x fps, last frame N B` lines in the monitor to see which.

## 7. Files

| Path | |
|---|---|
| `main/main.c` | camera init, JPEG encode, Wi-Fi, HTTP server |
| `main/Kconfig.projbuild` | all the knobs listed above |
| `main/idf_component.yml` | pulls `esp_video`, `esp_wifi_remote`, `esp_hosted` |
| `sdkconfig.defaults` | PSRAM, flash, camera and lwIP tuning |
| `partitions.csv` | 4 MB app on 16 MB flash |
