/*
 * ESP-DL YOLO11n object detection on captured frames. See detector.h for the
 * threading contract.
 */

#include <string.h>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "coco_detect.hpp"

#include "detector.h"
#include "sdkconfig.h"

static const char *TAG = "detect";

/* The frame layout must match what main.c captures. */
#if CONFIG_P4CAM_FMT_RGB888
  #define DETECT_PIX_TYPE  dl::image::DL_IMAGE_PIX_TYPE_RGB888
  #define DETECT_BPP       3
#else
  /* esp-dl 3.1.x has a single RGB565 type; on ESP32-P4 coco_detect builds its
     preprocessor without DL_IMAGE_CAP_RGB565_BIG_ENDIAN, i.e. little-endian,
     which is what the ISP writes. */
  #define DETECT_PIX_TYPE  dl::image::DL_IMAGE_PIX_TYPE_RGB565
  #define DETECT_BPP       2
#endif

/* esp-dl 3.1.x has no set_score_thr(); coco_detect fixes the model's internal
   threshold at 0.25, so anything stricter is applied here. */
#define DETECT_SCORE_THR ((float)CONFIG_P4CAM_DETECT_SCORE_THR / 100.0f)

static const char *const COCO_LABELS[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
    "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
    "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush",
};
static const int COCO_LABEL_COUNT = sizeof(COCO_LABELS) / sizeof(COCO_LABELS[0]);

static COCODetect       *s_model;
static uint8_t          *s_frame;        /* private copy handed to the model */
static size_t            s_frame_cap;
static uint16_t          s_width, s_height;
static SemaphoreHandle_t s_wake;
static SemaphoreHandle_t s_lock;         /* guards s_results / s_count */
static volatile bool     s_busy;         /* true from submit until run finishes */
static volatile bool     s_ready;
static int64_t           s_last_submit_us;
static detection_t       s_results[DETECTOR_MAX_RESULTS];
static size_t            s_count;
static float             s_last_ms;

extern "C" const char *detector_label(int category)
{
    if (category < 0 || category >= COCO_LABEL_COUNT) {
        return "?";
    }
    return COCO_LABELS[category];
}

static void detector_task(void *arg)
{
    while (true) {
        xSemaphoreTake(s_wake, portMAX_DELAY);

        dl::image::img_t img = {};
        img.data     = s_frame;
        img.width    = s_width;
        img.height   = s_height;
        img.pix_type = DETECT_PIX_TYPE;

        int64_t t0 = esp_timer_get_time();
        auto &results = s_model->run(img);
        float ms = (float)(esp_timer_get_time() - t0) / 1000.0f;

        /* Keep the highest-scoring boxes; the list is not sorted by score. */
        std::vector<dl::detect::result_t> sorted(results.begin(), results.end());
        std::sort(sorted.begin(), sorted.end(), dl::detect::greater_box);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_count = 0;
        for (auto &r : sorted) {
            if (s_count >= DETECTOR_MAX_RESULTS) {
                break;
            }
            if (r.box.size() < 4 || r.score < DETECT_SCORE_THR) {
                continue;
            }
            r.limit_box(s_width, s_height);
            detection_t *d = &s_results[s_count++];
            d->x0       = r.box[0];
            d->y0       = r.box[1];
            d->x1       = r.box[2];
            d->y1       = r.box[3];
            d->score    = r.score;
            d->category = r.category;
            d->label    = detector_label(r.category);
        }
        s_last_ms = ms;
        xSemaphoreGive(s_lock);

        ESP_LOGD(TAG, "%u objects in %.0f ms", (unsigned)s_count, ms);
        s_busy = false;
    }
}

extern "C" esp_err_t detector_start(uint16_t width, uint16_t height)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_width  = width;
    s_height = height;

    s_frame_cap = (size_t)width * height * DETECT_BPP;
    s_frame = (uint8_t *)heap_caps_aligned_alloc(64, s_frame_cap,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame) {
        ESP_LOGE(TAG, "no PSRAM for a %u byte detector frame buffer",
                 (unsigned)s_frame_cap);
        return ESP_ERR_NO_MEM;
    }

    s_wake = xSemaphoreCreateBinary();
    s_lock = xSemaphoreCreateMutex();
    if (!s_wake || !s_lock) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "loading YOLO11n (this takes a moment)...");
    s_model = new (std::nothrow) COCODetect();
    if (!s_model) {
        ESP_LOGE(TAG, "failed to construct the model");
        return ESP_ERR_NO_MEM;
    }
    /* Inference is heavy and slow; keep it off the stream's core and below the
       HTTP task in priority so video always wins. */
    BaseType_t ok = xTaskCreatePinnedToCore(detector_task, "detector", 8192, NULL,
                                            3, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start the detector task");
        return ESP_FAIL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready: %ux%u, score threshold %.2f, min interval %d ms",
             width, height, (float)CONFIG_P4CAM_DETECT_SCORE_THR / 100.0f,
             CONFIG_P4CAM_DETECT_INTERVAL_MS);
    return ESP_OK;
}

extern "C" bool detector_submit(const uint8_t *frame, size_t len)
{
    if (!s_ready || s_busy || !frame) {
        return false;
    }
    if (len > s_frame_cap) {
        return false;
    }

    int64_t now = esp_timer_get_time();
    if (now - s_last_submit_us < (int64_t)CONFIG_P4CAM_DETECT_INTERVAL_MS * 1000) {
        return false;
    }
    s_last_submit_us = now;

    /* Safe without a lock: the task only reads s_frame while s_busy is true,
       and this is the only writer, only while it is false. */
    s_busy = true;
    memcpy(s_frame, frame, len);
    xSemaphoreGive(s_wake);
    return true;
}

extern "C" size_t detector_get(detection_t *out, size_t max)
{
    if (!s_ready || !out || max == 0) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_count < max ? s_count : max;
    memcpy(out, s_results, n * sizeof(detection_t));
    xSemaphoreGive(s_lock);
    return n;
}

extern "C" float detector_last_ms(void)
{
    return s_last_ms;
}
