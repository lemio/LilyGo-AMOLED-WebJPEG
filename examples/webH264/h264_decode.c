#include "h264_decode.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_h264_dec_sw.h"
#include "esp_log.h"

static const char *TAG = "h264_decode";

struct h264_decoder {
    esp_h264_dec_handle_t dec;
    uint8_t *rgb565_buf;
    int width;
    int height;
    bool frame_ready;
};

static int find_start_code(const uint8_t *buf, int from, int len, int *start_len) {
    for (int i = from; i + 3 < len; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0) {
            if (buf[i + 2] == 1) {
                *start_len = 3;
                return i;
            }
            if (buf[i + 2] == 0 && buf[i + 3] == 1) {
                *start_len = 4;
                return i;
            }
        }
    }
    return -1;
}

static int nal_type_from_annexb(const uint8_t *nal, int nal_len) {
    int sc_len = 0;
    int start = find_start_code(nal, 0, nal_len, &sc_len);
    if (start < 0 || start + sc_len >= nal_len) return -1;
    return nal[start + sc_len] & 0x1f;
}

// Only forward SPS/PPS/slice NALs to the decoder; drop SEI and other
// metadata NALs to save CPU.
static bool should_decode_nal(const uint8_t *nal, int nal_len) {
    switch (nal_type_from_annexb(nal, nal_len)) {
    case 1: // non-IDR slice
    case 5: // IDR slice
    case 7: // SPS
    case 8: // PPS
        return true;
    default:
        return false;
    }
}

static inline uint16_t yuv_to_rgb565(int y, int u, int v) {
    // H.264 conventionally encodes luma/chroma in limited ("studio"/TV) range -
    // Y in [16,235], Cb/Cr in [16,240] (centered at 128, which the caller has
    // already subtracted off before calling this) - not full [0,255] PC range.
    // The matrix below (1.402/0.344/0.714/1.773, as *359/88/183/454 >> 8) is the
    // correct full-range YCbCr->RGB conversion, but without this rescale it was
    // being fed limited-range samples directly: black (Y=16) decoded to
    // RGB(16,16,16), a washed-out dark gray instead of true black - confirmed via
    // this file's debug top-left-pixel log in webH264.cpp.
    int yf = ((y - 16) * 298) >> 8; // 255/219 ~= 1.164, Q8 fixed-point
    int uf = (u * 291) >> 8;        // 255/224 ~= 1.138
    int vf = (v * 291) >> 8;

    int r = yf + ((vf * 359) >> 8);
    int g = yf - ((uf * 88 + vf * 183) >> 8);
    int b = yf + ((uf * 454) >> 8);

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    uint16_t rgb = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (rgb >> 8) | (rgb << 8); // byte-swap for the QSPI panel
}

static inline int mb_align(int dim) {
    return (dim + 15) & ~15;
}

// The SW decoder outputs I420 padded to 16px macroblock boundaries; this
// crops back down to the real width/height while converting to RGB565.
static void i420_to_rgb565(const uint8_t *i420_buf, uint16_t *rgb565,
                            int width, int height) {
    int stride_w = mb_align(width);
    int stride_h = mb_align(height);

    const uint8_t *y_plane = i420_buf;
    const uint8_t *u_plane = i420_buf + stride_w * stride_h;
    const uint8_t *v_plane = u_plane + (stride_w / 2) * (stride_h / 2);
    int half_stride = stride_w / 2;

    for (int j = 0; j < height; j++) {
        const uint8_t *y_row = y_plane + j * stride_w;
        const uint8_t *u_row = u_plane + (j / 2) * half_stride;
        const uint8_t *v_row = v_plane + (j / 2) * half_stride;

        for (int i = 0; i < width; i++) {
            rgb565[j * width + i] =
                yuv_to_rgb565(y_row[i], u_row[i / 2] - 128, v_row[i / 2] - 128);
        }
    }
}

static esp_h264_dec_handle_t open_decoder_handle(void) {
    esp_h264_dec_handle_t dec = NULL;
    esp_h264_dec_cfg_sw_t cfg = { .pic_type = ESP_H264_RAW_FMT_I420 };
    if (esp_h264_dec_sw_new(&cfg, &dec) != ESP_H264_ERR_OK || !dec) {
        return NULL;
    }
    if (esp_h264_dec_open(dec) != ESP_H264_ERR_OK) {
        esp_h264_dec_del(dec);
        return NULL;
    }
    return dec;
}

void h264_decode_reset(h264_decoder_t *d) {
    if (!d) return;
    esp_h264_dec_handle_t new_dec = open_decoder_handle();
    if (!new_dec) {
        ESP_LOGE(TAG, "failed to reset decoder");
        return;
    }
    if (d->dec) {
        esp_h264_dec_close(d->dec);
        esp_h264_dec_del(d->dec);
    }
    d->dec = new_dec;
    d->frame_ready = false;
}

h264_decoder_t *h264_decode_open(void) {
    struct h264_decoder *d = (struct h264_decoder *)calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->dec = open_decoder_handle();
    if (!d->dec) {
        free(d);
        return NULL;
    }
    // esp_h264_dec_get_resolution() reports the macroblock-aligned decode
    // size (e.g. 608x464 for a 600x450 stream), not the true frame size, so
    // it's not useful for sizing the display push. Browser and firmware are
    // both hardcoded to this same resolution by design, so just use it
    // directly instead of trying to query it back from the decoder.
    d->width = H264_DECODE_MAX_WIDTH;
    d->height = H264_DECODE_MAX_HEIGHT;

    d->rgb565_buf = (uint8_t *)heap_caps_malloc(
        (size_t)H264_DECODE_MAX_WIDTH * H264_DECODE_MAX_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM);
    if (!d->rgb565_buf) {
        esp_h264_dec_close(d->dec);
        esp_h264_dec_del(d->dec);
        free(d);
        return NULL;
    }
    ESP_LOGI(TAG, "decoder open, %dx%d", d->width, d->height);
    return d;
}

void h264_decode_close(h264_decoder_t *d) {
    if (!d) return;
    if (d->dec) {
        esp_h264_dec_close(d->dec);
        esp_h264_dec_del(d->dec);
    }
    if (d->rgb565_buf) heap_caps_free(d->rgb565_buf);
    free(d);
}

int h264_decode_parse(h264_decoder_t *d, const uint8_t *data, size_t len) {
    if (!d || !data || len == 0) return -1;

    int sc_len = 0;
    int nal_start = find_start_code(data, 0, (int)len, &sc_len);
    if (nal_start < 0) return -1;

    int ret = -1;
    while (nal_start >= 0) {
        int next_sc_len = 0;
        int next = find_start_code(data, nal_start + sc_len, (int)len, &next_sc_len);
        int nal_end = (next < 0) ? (int)len : next;
        int nal_len = nal_end - nal_start;
        if (nal_len > 0 && should_decode_nal(data + nal_start, nal_len)) {
            esp_h264_dec_in_frame_t in_frame = {0};
            in_frame.raw_data.buffer = (uint8_t *)(data + nal_start);
            in_frame.raw_data.len = (uint32_t)nal_len;
            esp_h264_dec_out_frame_t out_frame = {0};

            while (in_frame.raw_data.len > 0) {
                esp_h264_err_t err = esp_h264_dec_process(d->dec, &in_frame, &out_frame);
                if (err != ESP_H264_ERR_OK || in_frame.consume == 0) break;

                in_frame.raw_data.buffer += in_frame.consume;
                in_frame.raw_data.len -= in_frame.consume;

                if (out_frame.out_size == 0 || !out_frame.outbuf) continue;

                i420_to_rgb565(out_frame.outbuf, (uint16_t *)d->rgb565_buf, d->width, d->height);
                d->frame_ready = true;
                ret = 0;
                ESP_LOGI(TAG, "frame ready");
            }
        }

        if (next < 0) break;
        nal_start = next;
        sc_len = next_sc_len;
    }
    return ret;
}

int h264_decode_get_frame(h264_decoder_t *d, uint8_t **out_buf, int *out_width, int *out_height) {
    if (!d || !d->frame_ready) return -1;
    *out_buf = d->rgb565_buf;
    *out_width = d->width;
    *out_height = d->height;
    d->frame_ready = false;
    return 0;
}
