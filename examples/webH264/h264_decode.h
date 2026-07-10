#ifndef DISPLAY_STREAMING_H264_DECODE_H
#define DISPLAY_STREAMING_H264_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h264_decoder h264_decoder_t;

// Opens a streaming H.264 (constrained baseline) decoder backed by Espressif's
// esp_h264 software (tinyh264) decoder, for frames of exactly width x height (must
// match what the browser's VideoEncoder is configured for - see stream.html, which
// reads this size back from /boardinfo). Returns NULL on failure.
h264_decoder_t *h264_decode_open(int width, int height);

void h264_decode_close(h264_decoder_t *dec);

// Recreates the underlying decoder so a new streaming session starts from a
// clean state. Feeding a fresh SPS/PPS/IDR sequence into an already-active
// decode session (e.g. after a browser reconnect) can crash the underlying
// tinyh264 decoder; call this whenever a new client connects. Safe to call
// even if a previous session never sent an EOS/disconnect.
void h264_decode_reset(h264_decoder_t *dec);

// Feeds one Annex-B chunk as produced by a WebCodecs VideoEncoder configured
// with avc.format = "annexb" (one WebSocket message = one encoded chunk,
// possibly containing several NAL units, e.g. SPS+PPS+IDR for a keyframe).
// Returns 0 if a new frame became available (see h264_decode_get_frame),
// -1 otherwise.
int h264_decode_parse(h264_decoder_t *dec, const uint8_t *data, size_t len);

// If a decoded frame is ready, converted to RGB565 and byte-swapped for
// pushColors(), fills *out_buf/*out_width/*out_height and returns 0. The
// buffer is owned by the decoder and stays valid only until the next
// h264_decode_parse() call. Returns -1 if no frame is ready.
int h264_decode_get_frame(h264_decoder_t *dec, uint8_t **out_buf, int *out_width, int *out_height);

#ifdef __cplusplus
}
#endif

#endif
