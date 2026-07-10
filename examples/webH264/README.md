# webH264

Streams a browser tab, window, or camera to a LilyGo T4-S3 AMOLED board over WiFi.
The browser captures video with `getDisplayMedia`/`getUserMedia`, encodes it as H.264
using the [WebCodecs API](https://developer.mozilla.org/en-US/docs/Web/API/WebCodecs_API)
(`VideoEncoder`), and sends the Annex-B stream over a plain `WebSocket` - the ESP32
decodes it with Espressif's [esp_h264](https://github.com/espressif/esp-h264-component)
(tinyh264) software decoder and pushes the result straight to the display.

This is the same idea as the sibling [webJPEG](../webJPEG) example, but a different
codec tradeoff: H.264 compresses far better than MJPEG for the same visual quality
(lower bandwidth, and the decoder itself is what's slow here, not the network), at the
cost of a much heavier on-device decoder and a more involved build (see "Why this needs
its own env" below). Compare the two if you're unsure which fits your use case.

**T4-S3 only.** Unlike webJPEG's auto-detect-any-panel support, both ends of this
stream hardcode the resolution (600x450, the T4-S3/RM690B0 panel's native size) - the
browser's `VideoEncoder` is configured for it and the on-device decode buffers are sized
for it. It won't run correctly on the 1.47"/1.91" boards without changing those
constants (`WIDTH`/`HEIGHT` in [h264_stream.html](./h264_stream.html),
`DISPLAY_WIDTH`/`DISPLAY_HEIGHT` in [webH264.cpp](./webH264.cpp), and
`H264_DECODE_MAX_WIDTH`/`H264_DECODE_MAX_HEIGHT` in [h264_decode.h](./h264_decode.h)).

## Flashing with PlatformIO

```bash
pio run -e webH264 --target upload
```

(There's no browser-flasher/GitHub Pages build for this example yet - unlike webJPEG,
publishing it that way would need changes to the
[lemio/ESP32-S3-Flasher](https://github.com/lemio/ESP32-S3-Flasher) action to support
more than one firmware per repo.)

Before compiling, edit the placeholder values in [webH264.cpp](./webH264.cpp):

```cpp
const char ssid[100] = "|*S*|";     // replace with your WiFi SSID
const char password[100] = "|*P*|"; // replace with your WiFi password
```

## Why this needs its own env

Espressif's `esp_h264` decoder ships as a prebuilt static library
(`libtinyh264.a` per chip target) wired in through the IDF Component Manager
(`idf_component.yml`) - the plain Arduino/SCons build every other example here uses
can't consume that. `[env:webH264]` in [platformio.ini](../../platformio.ini) instead
builds Arduino as a component of a real ESP-IDF/CMake project
(`framework = arduino, espidf`), which is why this example alone needs:

- [webH264.cpp](./webH264.cpp), not `.ino` - the `.ino` preprocessor (auto function
  prototypes, implicit `Arduino.h`) is a plain-Arduino/SCons-only feature.
- [CMakeLists.txt](./CMakeLists.txt) and [idf_component.yml](./idf_component.yml) here,
  plus a project-root [CMakeLists.txt](../../CMakeLists.txt),
  [sdkconfig.defaults](../../sdkconfig.defaults), and
  [partitions.webH264.csv](../../partitions.webH264.csv) - all required by the ESP-IDF
  CMake build, none of which the plain-Arduino examples need.
- Its own minimal `lib_deps` in `[env:webH264]` rather than inheriting the shared
  `[env]` list - deliberately excludes dependencies (lvgl, JPEGDecoder, TinyGPSPlus,
  etc.) other examples need but this one doesn't, since untested libraries under this
  less common framework combination are a real build risk, not just dead weight.

## Using it

1. Flash the firmware (see above) and open the Serial Monitor - it prints the assigned
   IP address, and the display itself shows "Connecting to WiFi", then that address
   (as a `ws://` URL) once connected.
2. Open [h264_stream.html](./h264_stream.html) from a **secure context** - e.g. run
   `python3 -m http.server` in this folder and open `http://localhost:8000/h264_stream.html`.
   Unlike webJPEG, this page isn't hosted anywhere over HTTPS yet, so a plain
   `file://` open or a LAN `http://` address won't work: `getDisplayMedia()` requires
   HTTPS or `localhost`/`127.0.0.1` specifically. See webJPEG's README ("Why the
   redirect") for the full explanation of this browser restriction.
3. Enter the board's address from step 1 into the "ESP32 address" field, optionally
   adjust the capture FPS / frames-in-flight / ignore-acks controls (see "Flow control"
   below), and click "Start streaming". Open the browser console (F12) for detailed
   per-frame logs.

## Flow control

The ESP32's software H.264 decoder is far slower than typical capture frame rates -
expect roughly 3-5 fps at full 600x450 resolution (measured ~200-400ms per
decode+display cycle on real screen content), and it does all its work synchronously in
the WebSocket handler with no queue of its own. Left unmanaged, the browser would keep
sending faster than the device can keep up and lag would grow without bound. Instead,
the ESP32 sends a tiny WebSocket text message back after it finishes processing each
frame, and the browser uses that as a pacing signal:

- **Frames in flight (1 or 2):** how many un-acked frames the browser allows outstanding
  at once. 1 (default) is lowest latency - the browser waits for each frame to be fully
  processed before sending the next, so any frame captured in between is simply dropped.
  2 lets the browser send a frame while the previous one is still being decoded/pushed
  on-device, trading a bit more latency for a bit more throughput.
- **Ignore device acks:** disables this pacing entirely and sends every captured frame
  unconditionally, like there was no flow control at all - capture looks smoother
  (nothing gets dropped client-side) but latency grows for as long as you keep streaming,
  since frames queue up waiting to be decoded.
- **Capture FPS:** how fast the browser asks the OS to capture frames. Since the device
  can only really absorb ~3-5fps, requesting much more than that (with acks not
  ignored) just means most captured frames get immediately dropped for nothing -
  useful to raise past that once ack-ignoring, since capture then determines the send
  rate directly.

## Troubleshooting

- **Nothing shows on the display / "Streaming..." status but no image:** open the
  browser console (F12) and check for `DROP`/`WARN` lines - if you see
  `<-- MISMATCH, possible backlog`, the browser's own capture queue is backing up
  (shouldn't happen with the current flow-control logic, but would explain stale/delayed
  frames if it does). If frames are being sent (`ws.send` lines) but the display stays
  blank, check the serial monitor for `Error in decoding` or a crash/reboot.
- **"WebSocket connection ... failed" in the console:** confirm the board's address is
  reachable (`ping <address>`) and that `h264_stream.html` is being served from a
  secure context (see "Using it" above) - Chrome silently fails the WebCodecs setup on
  an insecure origin without a clear error otherwise.
- **Choppy / low frame rate:** expected at this resolution on this decoder - see "Flow
  control" above. Try "2 frames in flight" for a modest throughput increase.

## Technical notes

- `h264_decode.c`/`.h` wrap `esp_h264`'s streaming decoder API: `h264_decode_open()`
  once at boot, `h264_decode_reset()` on every new WebSocket connection (feeding a fresh
  SPS/PPS/IDR sequence into an already-active decode session can crash the underlying
  tinyh264 decoder), `h264_decode_parse()` per received chunk, `h264_decode_get_frame()`
  to pull out a decoded frame already converted to RGB565.
- `esp_h264_dec_get_resolution()` reports the macroblock-aligned decode size (608x464
  for this 600x450 stream), not the true frame size - not useful for sizing the display
  push, so the decoder just uses the known 600x450 directly instead of querying it back.
- The WebSocket message-reassembly buffer in `webH264.cpp` is allocated once at a fixed
  size (128KB) and reused for every frame, never `malloc`'d/`free`'d per-message -
  repeated variable-size allocations on the PSRAM heap over a long session is a classic
  embedded fragmentation crash, and this sidesteps it entirely.
- `loop()` calls `delay(1)` after `ws.cleanupClients()`: without it, `loopTask` (higher
  priority than the idle task) never yields, and the idle task on that core never runs -
  the task watchdog will fire continuously (harmless by itself, but worth explaining so
  it isn't mistaken for a real hang if you see it in the serial log).
