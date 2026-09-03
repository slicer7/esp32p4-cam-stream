/*
 * Object detection (YOLO11n / COCO, 80 classes) running on the ESP32-P4 via
 * ESP-DL. C wrapper around the C++ coco_detect component so main.c stays C.
 *
 * Threading model: one detector task. Frames are handed over with
 * detector_submit(), which copies and returns immediately; if inference is
 * still running the frame is dropped rather than queued, so the video path is
 * never blocked by the model. Results are read with detector_get().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DETECTOR_MAX_RESULTS 16

typedef struct {
    int         x0, y0, x1, y1;  /* box, in capture-frame pixel coordinates */
    float       score;           /* 0..1 */
    int         category;        /* COCO class index */
    const char *label;           /* class name, never NULL */
} detection_t;

/* Starts the model and its task. width/height describe the frames that will be
   submitted. Safe to call once. */
esp_err_t detector_start(uint16_t width, uint16_t height);

/* Hands one RGB565 (or RGB888, per build config) frame to the detector.
   Non-blocking. Returns true if the frame was taken, false if the detector was
   busy or not enough time has passed since the last run. */
bool detector_submit(const uint8_t *frame, size_t len);

/* Copies out the most recent results. Returns how many were written. */
size_t detector_get(detection_t *out, size_t max);

/* Milliseconds the last inference took, 0 if none has completed. */
float detector_last_ms(void);

/* Class name for a COCO category index; "?" if out of range. */
const char *detector_label(int category);

#ifdef __cplusplus
}
#endif
