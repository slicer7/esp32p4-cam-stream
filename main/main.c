/*
 * ESP32-P4 + OV5647 (Raspberry Pi camera) -> MJPEG over Wi-Fi.
 *
 * Pipeline:
 *   OV5647 --MIPI-CSI (2 lane, RAW8)--> P4 ISP --RGB565/RGB888--> P4 hardware
 *   JPEG encoder --> esp_http_server (multipart/x-mixed-replace) --> your PC.
 *
 * Wi-Fi runs on the on-board ESP32-C6 through ESP-Hosted; the esp_wifi_* calls
 * below are the normal API, esp_wifi_remote forwards them over SDIO.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_cache.h"
/* esp_cache_get_alignment() is only declared in this private IDF header. It is
   used once, for a defensive check on V4L2 buffer alignment in capture_jpeg(). */
#include "esp_private/esp_cache_private.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

#include "driver/jpeg_encode.h"

#include "linux/videodev2.h"
#include "esp_video_init.h"

#if CONFIG_P4CAM_DETECT_ENABLE
#include "detector.h"
#endif

static const char *TAG = "p4cam";

#define VIDEO_DEVICE      "/dev/video0"   /* MIPI-CSI capture device */
#define ISP_DEVICE        "/dev/video20"  /* ISP controls (brightness etc.) */
#define VIDEO_BUF_COUNT   2

#if CONFIG_P4CAM_FMT_RGB888
  #define CAP_PIXFMT      V4L2_PIX_FMT_RGB24
  #define JPEG_SRC_FMT    JPEG_ENCODE_IN_FORMAT_RGB888
  #define CAP_BPP         3
#else
  #define CAP_PIXFMT      V4L2_PIX_FMT_RGB565
  #define JPEG_SRC_FMT    JPEG_ENCODE_IN_FORMAT_RGB565
  #define CAP_BPP         2
#endif

/* ------------------------------------------------------------------ Wi-Fi */

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_events;
static int s_retry_num;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_retry_num < CONFIG_P4CAM_WIFI_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retry %d", s_retry_num);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "================================================");
        ESP_LOGI(TAG, " Stream ready:  http://" IPSTR "/", IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "================================================");
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, CONFIG_P4CAM_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_P4CAM_WIFI_PASSWORD, sizeof(wc.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Keep latency down while streaming. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Wi-Fi failed to connect to \"%s\"", CONFIG_P4CAM_WIFI_SSID);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ Camera */

typedef struct {
    int       fd;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pixfmt;
    uint8_t  *buf[VIDEO_BUF_COUNT];
    size_t    buf_size[VIDEO_BUF_COUNT];
} cam_t;

static cam_t s_cam;

static void fourcc_str(uint32_t f, char out[5])
{
    out[0] = f & 0xff;
    out[1] = (f >> 8) & 0xff;
    out[2] = (f >> 16) & 0xff;
    out[3] = (f >> 24) & 0xff;
    out[4] = 0;
}

static esp_err_t camera_hw_init(void)
{
    static const esp_video_init_csi_config_t csi_cfg[] = {
        {
            .sccb_config = {
                .init_sccb = true,
                .i2c_config = {
                    .port    = 0,
                    .scl_pin = CONFIG_P4CAM_CSI_SCCB_SCL,
                    .sda_pin = CONFIG_P4CAM_CSI_SCCB_SDA,
                },
                .freq = 100000,
            },
            .reset_pin = CONFIG_P4CAM_CSI_RESET_GPIO,
            .pwdn_pin  = CONFIG_P4CAM_CSI_PWDN_GPIO,
        },
    };
    static const esp_video_init_config_t vid_cfg = {
        .csi = csi_cfg,
    };

    esp_err_t ret = esp_video_init(&vid_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s -- camera not answering on SCCB? "
                      "check the FPC seating, orientation and the I2C pins",
                 esp_err_to_name(ret));
    }
    return ret;
}

static void camera_log_capabilities(int fd)
{
    struct v4l2_capability cap = { 0 };
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        ESP_LOGI(TAG, "driver=%s card=%s", cap.driver, cap.card);
    }

    for (int i = 0; ; i++) {
        struct v4l2_fmtdesc desc = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        if (ioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0) {
            break;
        }
        char cc[5];
        fourcc_str(desc.pixelformat, cc);
        ESP_LOGI(TAG, "  format[%d]: %s (%s)", i, cc, desc.description);

        for (int j = 0; ; j++) {
            struct v4l2_frmsizeenum fs = { .index = j, .pixel_format = desc.pixelformat };
            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) != 0) {
                break;
            }
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                ESP_LOGI(TAG, "      %" PRIu32 "x%" PRIu32,
                         fs.discrete.width, fs.discrete.height);
            }
        }
    }
}

static esp_err_t camera_open(cam_t *c)
{
    c->fd = open(VIDEO_DEVICE, O_RDWR);
    if (c->fd < 0) {
        ESP_LOGE(TAG, "open(%s) failed, errno %d", VIDEO_DEVICE, errno);
        return ESP_FAIL;
    }

    camera_log_capabilities(c->fd);

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(c->fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed, errno %d", errno);
        return ESP_FAIL;
    }

    fmt.fmt.pix.width       = CONFIG_P4CAM_FRAME_WIDTH;
    fmt.fmt.pix.height      = CONFIG_P4CAM_FRAME_HEIGHT;
    fmt.fmt.pix.pixelformat = CAP_PIXFMT;
    if (ioctl(c->fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT %dx%d REJECTED (errno %d)",
                 CONFIG_P4CAM_FRAME_WIDTH, CONFIG_P4CAM_FRAME_HEIGHT, errno);
        ESP_LOGE(TAG, "  The sensor mode is almost certainly not compiled in. Picking a");
        ESP_LOGE(TAG, "  resolution under \"P4 Camera Stream Configuration\" is NOT enough --");
        ESP_LOGE(TAG, "  the mode must also be enabled under Component config -> Espressif");
        ESP_LOGE(TAG, "  Camera Sensors -> OV5647. See the format list logged just above.");
        ESP_LOGE(TAG, "  Falling back to the sensor default, which may be a CROPPED mode");
        ESP_LOGE(TAG, "  with a narrower field of view than you asked for.");
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(c->fd, VIDIOC_G_FMT, &fmt) != 0) {
            return ESP_FAIL;
        }
        fmt.fmt.pix.pixelformat = CAP_PIXFMT;
        if (ioctl(c->fd, VIDIOC_S_FMT, &fmt) != 0) {
            ESP_LOGE(TAG, "cannot set any capture format, errno %d", errno);
            return ESP_FAIL;
        }
    }

    c->width  = fmt.fmt.pix.width;
    c->height = fmt.fmt.pix.height;
    c->pixfmt = fmt.fmt.pix.pixelformat;

    char cc[5];
    fourcc_str(c->pixfmt, cc);
    ESP_LOGI(TAG, "capturing %" PRIu32 "x%" PRIu32 " %s", c->width, c->height, cc);

    struct v4l2_requestbuffers req = {
        .count  = VIDEO_BUF_COUNT,
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(c->fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed, errno %d", errno);
        return ESP_FAIL;
    }

    for (int i = 0; i < VIDEO_BUF_COUNT; i++) {
        struct v4l2_buffer b = {
            .index  = i,
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(c->fd, VIDIOC_QUERYBUF, &b) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF %d failed, errno %d", i, errno);
            return ESP_FAIL;
        }
        c->buf_size[i] = b.length;
        c->buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                         c->fd, b.m.offset);
        if (!c->buf[i]) {
            ESP_LOGE(TAG, "mmap of buffer %d failed", i);
            return ESP_FAIL;
        }
        if (ioctl(c->fd, VIDIOC_QBUF, &b) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF %d failed, errno %d", i, errno);
            return ESP_FAIL;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(c->fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed, errno %d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------ ISP tuning */

static void isp_set_ctrl(int fd, uint32_t id, int32_t val, const char *what)
{
    struct v4l2_ext_control ctrl = { .id = id, .value = val };
    struct v4l2_ext_controls ctrls = {
        .ctrl_class = V4L2_CTRL_CLASS_USER,
        .count      = 1,
        .controls   = &ctrl,
    };
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
        ESP_LOGW(TAG, "ISP %s = %d rejected (errno %d)", what, (int)val, errno);
    } else {
        ESP_LOGI(TAG, "ISP %s = %d", what, (int)val);
    }
}

/* Post-ISP image tuning. Brightness here is a lift applied after the ISP, not a
   longer exposure, so it costs no frame rate but does lift noise with it. */
static void isp_apply_controls(void)
{
    int fd = open(ISP_DEVICE, O_RDWR);
    if (fd < 0) {
        ESP_LOGW(TAG, "no ISP device at %s (errno %d); skipping image tuning",
                 ISP_DEVICE, errno);
        return;
    }
    isp_set_ctrl(fd, V4L2_CID_BRIGHTNESS, CONFIG_P4CAM_ISP_BRIGHTNESS, "brightness");
    isp_set_ctrl(fd, V4L2_CID_CONTRAST,   CONFIG_P4CAM_ISP_CONTRAST,   "contrast");
    isp_set_ctrl(fd, V4L2_CID_SATURATION, CONFIG_P4CAM_ISP_SATURATION, "saturation");
    close(fd);
}

/* ------------------------------------------------------- box overlay */

#if CONFIG_P4CAM_DETECT_ENABLE && CONFIG_P4CAM_DETECT_DRAW_BOXES

#if CONFIG_P4CAM_FMT_RGB888
  #define BOX_R 0x00
  #define BOX_G 0xFF
  #define BOX_B 0x40
#else
  /* RGB565 little-endian bright green. */
  #define BOX_RGB565 0x07E4
#endif

static inline void put_px(uint8_t *fb, uint32_t w, uint32_t h, int x, int y)
{
    if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) {
        return;
    }
#if CONFIG_P4CAM_FMT_RGB888
    uint8_t *p = fb + ((size_t)y * w + x) * 3;
    p[0] = BOX_R; p[1] = BOX_G; p[2] = BOX_B;
#else
    *(uint16_t *)(fb + ((size_t)y * w + x) * 2) = BOX_RGB565;
#endif
}

static void draw_rect(uint8_t *fb, uint32_t w, uint32_t h,
                      int x0, int y0, int x1, int y1)
{
    const int t = 2;   /* line thickness */
    for (int k = 0; k < t; k++) {
        for (int x = x0; x <= x1; x++) {
            put_px(fb, w, h, x, y0 + k);
            put_px(fb, w, h, x, y1 - k);
        }
        for (int y = y0; y <= y1; y++) {
            put_px(fb, w, h, x0 + k, y);
            put_px(fb, w, h, x1 - k, y);
        }
    }
}

static void draw_detections(uint8_t *fb, uint32_t w, uint32_t h)
{
    detection_t d[DETECTOR_MAX_RESULTS];
    size_t n = detector_get(d, DETECTOR_MAX_RESULTS);
    for (size_t i = 0; i < n; i++) {
        draw_rect(fb, w, h, d[i].x0, d[i].y0, d[i].x1, d[i].y1);
    }
}
#endif /* draw boxes */

/* --------------------------------------------------------- JPEG encoding */

static jpeg_encoder_handle_t s_jpeg;
static uint8_t *s_jpeg_out;
static size_t   s_jpeg_out_size;
static uint8_t *s_align_scratch;        /* only used if V4L2 buffers are unaligned */
static size_t   s_align_scratch_size;
static size_t   s_cache_align;

static esp_err_t jpeg_init(const cam_t *c)
{
    jpeg_encode_engine_cfg_t eng = { .timeout_ms = 5000 };
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&eng, &s_jpeg), TAG, "jpeg engine");

    jpeg_encode_memory_alloc_cfg_t mem = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    /* One byte per pixel is a very generous ceiling for a q80 photographic frame. */
    size_t want = c->width * c->height;
    s_jpeg_out = jpeg_alloc_encoder_mem(want, &mem, &s_jpeg_out_size);
    if (!s_jpeg_out) {
        ESP_LOGE(TAG, "no memory for the JPEG output buffer (%u bytes)", (unsigned)want);
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &s_cache_align));
    ESP_LOGI(TAG, "JPEG output buffer %u bytes, cache alignment %u",
             (unsigned)s_jpeg_out_size, (unsigned)s_cache_align);
    return ESP_OK;
}

/* Grabs one frame and encodes it. On success *out points into s_jpeg_out. */
static esp_err_t capture_jpeg(cam_t *c, uint8_t **out, uint32_t *out_len)
{
    struct v4l2_buffer b = {
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(c->fd, VIDIOC_DQBUF, &b) != 0) {
        ESP_LOGE(TAG, "VIDIOC_DQBUF failed, errno %d", errno);
        return ESP_FAIL;
    }

    uint8_t *src     = c->buf[b.index];
    uint32_t src_len = b.bytesused ? b.bytesused : (c->width * c->height * CAP_BPP);

    /* The hardware encoder needs a cache-line-aligned source. The V4L2 buffers
       normally already are; copy through scratch if a driver version is not. */
    if (s_cache_align && (((uintptr_t)src % s_cache_align) != 0)) {
        if (s_align_scratch_size < src_len) {
            free(s_align_scratch);
            jpeg_encode_memory_alloc_cfg_t mem = {
                .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER
            };
            s_align_scratch = jpeg_alloc_encoder_mem(src_len, &mem, &s_align_scratch_size);
            if (!s_align_scratch) {
                s_align_scratch_size = 0;
                ioctl(c->fd, VIDIOC_QBUF, &b);
                return ESP_ERR_NO_MEM;
            }
        }
        memcpy(s_align_scratch, src, src_len);
        src = s_align_scratch;
    }

#if CONFIG_P4CAM_DETECT_ENABLE
    /* Hand this frame to the model if it is idle. Copies and returns at once,
       so the stream never waits on inference. */
    detector_submit(src, src_len);
#if CONFIG_P4CAM_DETECT_DRAW_BOXES
    /* Boxes come from the most recent completed inference, which is a frame or
       two old — they lag slightly on fast movement. Drawn before encoding so
       ffplay and OpenCV clients see them too. */
    draw_detections(src, c->width, c->height);
#endif
#endif

    jpeg_encode_cfg_t ecfg = {
        .src_type      = JPEG_SRC_FMT,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = CONFIG_P4CAM_JPEG_QUALITY,
        .width         = c->width,
        .height        = c->height,
    };

    uint32_t jlen = 0;
    esp_err_t ret = jpeg_encoder_process(s_jpeg, &ecfg, src, src_len,
                                         s_jpeg_out, s_jpeg_out_size, &jlen);

    /* Hand the capture buffer back to the driver as soon as we are done with it. */
    ioctl(c->fd, VIDIOC_QBUF, &b);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_encoder_process: %s", esp_err_to_name(ret));
        return ret;
    }
    *out     = s_jpeg_out;
    *out_len = jlen;
    return ESP_OK;
}

/* ------------------------------------------------------------- HTTP server */

#define BOUNDARY "p4camframe"

static SemaphoreHandle_t s_cam_lock;

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>ESP32-P4 camera</title>"
    "<style>"
    "body{margin:0;background:#111;color:#eee;font:14px system-ui;text-align:center}"
    "#wrap{position:relative;display:inline-block;line-height:0}"
    "#v{max-width:100%;height:auto;display:block}"
    ".tag{position:absolute;transform:translateY(-100%);background:#07e4;"
    "color:#000;font:600 12px/1.4 system-ui;padding:0 4px;white-space:nowrap;"
    "border-radius:2px 2px 0 0}"
    "#foot{padding:8px;line-height:1.5}"
    "</style></head><body>"
    "<div id=\"wrap\"><img id=\"v\" src=\"/stream\" alt=\"live stream\"></div>"
    "<div id=\"foot\">/stream = MJPEG &middot; /jpg = single frame &middot; "
    "/detections = JSON<br><span id=\"st\">&nbsp;</span></div>"
    "<script>"
    "const v=document.getElementById('v'),w=document.getElementById('wrap'),"
    "st=document.getElementById('st');"
    "async function poll(){"
    " try{"
    "  const r=await fetch('/detections',{cache:'no-store'});"
    "  const j=await r.json();"
    "  for(const e of w.querySelectorAll('.tag'))e.remove();"
    "  if(!j.width||!v.clientWidth){return;}"
    "  const s=v.clientWidth/j.width;"
    "  for(const d of j.objects){"
    "   const t=document.createElement('div');"
    "   t.className='tag';t.style.left=(d.x0*s)+'px';t.style.top=(d.y0*s)+'px';"
    "   t.textContent=d.label+' '+Math.round(d.score*100)+'%';"
    "   w.appendChild(t);"
    "  }"
    "  st.textContent=j.objects.length+' object(s), inference '+j.ms.toFixed(0)+' ms';"
    " }catch(e){}"
    "}"
    "setInterval(poll,400);poll();"
    "</script></body></html>";

static esp_err_t detections_handler(httpd_req_t *req)
{
    char json[1024];
    int n = snprintf(json, sizeof(json), "{\"width\":%" PRIu32 ",\"height\":%" PRIu32,
                     s_cam.width, s_cam.height);

#if CONFIG_P4CAM_DETECT_ENABLE
    detection_t d[DETECTOR_MAX_RESULTS];
    size_t count = detector_get(d, DETECTOR_MAX_RESULTS);
    n += snprintf(json + n, sizeof(json) - n, ",\"ms\":%.1f,\"objects\":[",
                  detector_last_ms());
    for (size_t i = 0; i < count && n < (int)sizeof(json) - 128; i++) {
        n += snprintf(json + n, sizeof(json) - n,
                      "%s{\"label\":\"%s\",\"score\":%.3f,"
                      "\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d}",
                      i ? "," : "", d[i].label, d[i].score,
                      d[i].x0, d[i].y0, d[i].x1, d[i].y1);
    }
    n += snprintf(json + n, sizeof(json) - n, "]}");
#else
    n += snprintf(json + n, sizeof(json) - n, ",\"ms\":0,\"objects\":[]}");
#endif

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t jpg_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(s_cam_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera busy");
        return ESP_FAIL;
    }

    uint8_t *jpg;
    uint32_t len;
    esp_err_t ret = capture_jpeg(&s_cam, &jpg, &len);
    if (ret == ESP_OK) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        ret = httpd_resp_send(req, (const char *)jpg, len);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
    }

    xSemaphoreGive(s_cam_lock);
    return ret;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(s_cam_lock, 0) != pdTRUE) {
        /* httpd_err_code_t has no 503 entry, so set the status line directly. */
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "another client is already streaming");
        return ESP_FAIL;
    }

    esp_err_t ret = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_cam_lock);
        return ret;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");

    char hdr[96];
    int64_t t0 = esp_timer_get_time();
    uint32_t frames = 0;

    while (true) {
        uint8_t *jpg;
        uint32_t len;
        if (capture_jpeg(&s_cam, &jpg, &len) != ESP_OK) {
            break;
        }

        int n = snprintf(hdr, sizeof(hdr),
                         "\r\n--" BOUNDARY "\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "Content-Length: %" PRIu32 "\r\n\r\n", len);

        if (httpd_resp_send_chunk(req, hdr, n) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)jpg, len) != ESP_OK) {
            break;      /* client went away */
        }

        if (++frames % 30 == 0) {
            int64_t dt = esp_timer_get_time() - t0;
            ESP_LOGI(TAG, "%.1f fps, last frame %" PRIu32 " B",
                     30.0f * 1000000.0f / (float)dt, len);
            t0 = esp_timer_get_time();
        }
    }

    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "stream client gone after %" PRIu32 " frames", frames);
    xSemaphoreGive(s_cam_lock);
    return ESP_OK;
}

static esp_err_t http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.ctrl_port        = 32768;
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;
    /* The MJPEG handler occupies its socket for as long as the client watches. */
    cfg.max_open_sockets = 4;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd_start");

    httpd_uri_t uris[] = {
        { .uri = "/",           .method = HTTP_GET, .handler = index_handler      },
        { .uri = "/stream",     .method = HTTP_GET, .handler = stream_handler     },
        { .uri = "/jpg",        .method = HTTP_GET, .handler = jpg_handler        },
        { .uri = "/detections", .method = HTTP_GET, .handler = detections_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "uri");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------- main */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_cam_lock = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(camera_hw_init());
    ESP_ERROR_CHECK(camera_open(&s_cam));
    isp_apply_controls();
    ESP_ERROR_CHECK(jpeg_init(&s_cam));

#if CONFIG_P4CAM_DETECT_ENABLE
    /* Not fatal: a camera that streams without detection beats no camera. */
    if (detector_start(s_cam.width, s_cam.height) != ESP_OK) {
        ESP_LOGE(TAG, "object detection unavailable, streaming without it");
    }
#endif

    /* Prove the imaging path works before there is any network to blame. */
    uint8_t *jpg;
    uint32_t len;
    if (capture_jpeg(&s_cam, &jpg, &len) == ESP_OK) {
        ESP_LOGI(TAG, "first frame encoded OK, %" PRIu32 " bytes", len);
    }

    ESP_ERROR_CHECK(wifi_init_sta());
    ESP_ERROR_CHECK(http_start());

    ESP_LOGI(TAG, "ready");
}
