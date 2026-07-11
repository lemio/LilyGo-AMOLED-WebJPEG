# webH264

Streams a browser tab, window, or camera to a LilyGo T-Display AMOLED board over WiFi.
The browser captures video with `getDisplayMedia`/`getUserMedia`, encodes it as H.264
using the [WebCodecs API](https://developer.mozilla.org/en-US/docs/Web/API/WebCodecs_API)
(`VideoEncoder`), and sends the Annex-B stream over a plain `WebSocket` - the ESP32
decodes it with Espressif's [esp_h264](https://github.com/espressif/esp-h264-component)
(tinyh264) software decoder and pushes the result straight to the display.

This is the sibling of [webJPEG](../webJPEG) - see the root [README](../../README.md)
for how the two compare and which one to pick. Both share the same streaming page
([stream.html](../stream.html)) and the same on-device WiFi setup flow, and both
auto-detect the attached panel at boot - no compile-time board selection needed.

**Inspiration:** using a real video codec instead of MJPEG for this kind of streaming
was prompted by [easymcucourse/esp32base's L029_h264](https://github.com/easymcucourse/esp32base/tree/main/L029_h264)
example and its [accompanying video](https://www.youtube.com/watch?v=knCc9FmSXms) -
also an ESP32-S3 + `esp_h264` software decoder. Its demo runs much faster than this
example does (see "Why this is slower than other esp_h264 demos" below) because it
decodes at a much smaller resolution, not because it does anything fundamentally
different.

## Board support

Like webJPEG, this calls the display library's automatic board-detection `amoled.begin()`
(see `beginAutomatic()`/`begin()` in [LilyGo_AMOLED.cpp](../../src/LilyGo_AMOLED.cpp)),
which probes I2C addresses at boot to tell the panels apart. The decode buffer
(device side) and the browser's `VideoEncoder` (via `/boardinfo` - see "Technical
notes" below) both size themselves to whatever panel is actually attached, so one
firmware build works across the whole lineup:

| Board | Panel | Native resolution |
| ----- | ----- | ------------------ |
| T-Display AMOLED Lite 1.47" | SH8501 | 194x368 |
| T-Display AMOLED 1.91" (QSPI) | RM67162 | 240x536 |
| T-Display AMOLED 1.91" (SPI) | RM67162 | 240x536 |
| T-Display AMOLED 2.41" / T4-S3 | RM690B0 | 600x450 |

Decode speed scales with pixel count (see "Flow control" and "Why this is slower than
other esp_h264 demos" below), so the smaller panels will decode noticeably faster than
the ~3-5fps measured on the T4-S3's 600x450 - the T4-S3 is the largest panel here, so
it's also the slowest case.

**Caveat:** this example was developed and compile-tested against the T4-S3 board only
(the same caveat webJPEG's README already carries for its own primary test board). The
other rows are what the shared detection code and the dynamic buffer/encoder sizing
*should* produce, not something verified on that physical hardware here. If you hit a
board-specific problem, please open an issue with your board model and a serial log
from boot.

## Flashing

See the root [README](../../README.md#flashing) for the browser flasher (no PlatformIO
install needed) and the PlatformIO command-line steps. This example's PlatformIO
environment name is `webH264`.

## Using it

1. Flash the firmware and open the Serial Monitor - it prints the assigned IP address,
   and the display itself shows a connection spinner, then the IP address and mDNS
   hostname once WiFi connects (see the root README's "Setup flow" section - this is
   identical to webJPEG's).
2. Visit `http://<that address>` (or `http://esp-webh264.local`, unless you set a
   different mDNS hostname). The board doesn't serve the streaming page itself - it
   302-redirects you to the HTTPS copy of [stream.html](../stream.html) hosted on GitHub
   Pages, pre-filling the `espAddress` field with the board's address and detecting that
   this is a webH264 board. See the root README's "Why the redirect" section for why,
   and the one manual step it requires (allowing "insecure content" once per browser).
3. Optionally adjust the capture FPS / frames-in-flight / ignore-acks controls (see
   "Flow control" below), and click "Start Streaming". All three can also be changed
   live while streaming, without restarting - useful for tuning latency vs. throughput
   while watching the stream itself. Open the browser console (F12) for detailed
   per-frame logs.

## Options in stream.html

Once [stream.html](../stream.html) detects a webH264 board (or you force "Mode: Force
WebH264"), it shows (all three adjustable live, mid-stream):

- **Resolution** - auto-filled from `/boardinfo` once the board is detected, not
  user-editable (see "Board support" above).
- **Capture FPS** - how fast the browser asks the OS to capture frames. Changing this
  live re-applies the constraint to the active capture track (`applyConstraints()`);
  the browser/OS may not always honor it.
- **Frames in flight** - how many un-acked frames the browser allows outstanding at
  once (see "Flow control" below).
- **Ignore device acks** - disables flow control entirely.

`stream.html` also accepts URL query parameters for bookmarking a specific setup:
`espAddress`, `mode=h264`, `h264Fps` (1-60), `maxInFlight` (`1`/`2`), `ignoreAcks`
(`true`/`false`).

## Why this needs its own env

Espressif's `esp_h264` decoder ships as a prebuilt static library
(`libtinyh264.a` per chip target) wired in through the IDF Component Manager
(`idf_component.yml`) - the plain Arduino/SCons build every other example here uses
can't consume that. `[env:webH264]` in [platformio.ini](../../platformio.ini) instead
builds Arduino as a component of a real ESP-IDF/CMake project
(`framework = arduino, espidf`), which is why this example alone needs:

- [webH264.cpp](./webH264.cpp), not `.ino` - the `.ino` preprocessor (auto function
  prototypes, implicit `Arduino.h`) is a plain-Arduino/SCons-only feature, and doesn't
  apply under this builder either way.
- [idf_component.yml](../idf_component.yml) (at the shared `examples/` root, alongside
  [CMakeLists.txt](../CMakeLists.txt), which explicitly lists this example's source
  files since the ESP-IDF/CMake builder ignores PlatformIO's `build_src_filter`), plus a
  project-root [CMakeLists.txt](../../CMakeLists.txt),
  [sdkconfig.defaults](../../sdkconfig.defaults), and
  [partitions.webH264.csv](../../partitions.webH264.csv) - all required by the ESP-IDF
  CMake build, none of which the plain-Arduino `webJPEG` example needs.
- Its own minimal `lib_deps` in `[env:webH264]` rather than inheriting the shared
  `[env]` list - only what this example and this repo's own `src/` actually need,
  since untested libraries under this less common framework combination are a real
  build risk.

## Flow control

The ESP32's software H.264 decoder is far slower than typical capture frame rates -
expect roughly 3-5 fps at the T4-S3's full 600x450 resolution (measured ~200-400ms per
decode+display cycle on real screen content; smaller panels decode proportionally
faster - see "Board support" above), and it does all its work synchronously in the
WebSocket handler with no queue of its own. Left unmanaged, the browser would keep
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
  can only really absorb a few fps at a time (see "Board support" above for how that
  varies by panel), requesting much more than that (with acks not ignored) just means
  most captured frames get immediately dropped for nothing - useful to raise past that
  once ack-ignoring, since capture then determines the send rate directly.

## Why this is slower than other esp_h264 demos

[easymcucourse/esp32base's L029_h264](https://github.com/easymcucourse/esp32base/tree/main/L029_h264)
(the demo that inspired this example - see "Inspiration" above) plays video smoothly at
a much higher frame rate than the ~3-5fps here, despite using the same chip
(ESP32-S3) and the same software decoder (`esp_h264`/tinyh264). The difference isn't a
smarter decoder or hardware acceleration (ESP32-S3 has no H.264 hardware decode block -
only ESP32-P4 does) - it's resolution. That demo decodes at 160x128 (20,480 pixels);
this example decodes at 600x450 (270,000 pixels) to fill the T4-S3's full panel - about
13x more pixels. Software H.264 decode cost scales roughly with pixel/macroblock count,
which lines up with what this example's own per-frame timing shows: ~190-280ms decode
at 600x450 versus an expected ~15-20ms at 160x128, the same order of magnitude as that
demo's much higher frame rate. Their encoder settings (baseline profile, level 3.0, no
B-frames, CAVLC not CABAC) are also essentially what this example already uses -
`avc1.42E01E` in [stream.html](../stream.html) is Baseline/Level 3.0, and Baseline
profile mandates no B-frames/CAVLC anyway.

If you want a higher frame rate than the T4-S3's 600x450 gives you, the 1.47"/1.91"
boards' smaller panels (see "Board support" above) get you that automatically, since
resolution is now auto-detected rather than a constant to hand-edit - decoder tuning
has comparatively little effect next to pixel count.

## Troubleshooting

- **Nothing shows on the display / "Streaming..." status but no image:** open the
  browser console (F12) and check for `DROP`/`WARN` lines - if you see
  `<-- MISMATCH, possible backlog`, the browser's own capture queue is backing up
  (shouldn't happen with the current flow-control logic, but would explain stale/delayed
  frames if it does). If frames are being sent (`ws.send` lines) but the display stays
  blank, check the serial monitor for `Error in decoding` or a crash/reboot.
- **"WebSocket connection ... failed" in the console:** confirm the board's address is
  reachable (`ping <address>`) and that you're on the HTTPS `stream.html` (see the root
  README's "Why the redirect" for the one-time insecure-content exception the WebSocket
  needs).
- **Choppy / low frame rate:** expected at this resolution on this decoder - see "Flow
  control" above. Try "2 frames in flight" for a modest throughput increase.

## Technical notes

- Every received frame logs a per-stage timing breakdown to Serial: `Recv` (network +
  WebSocket fragment reassembly), `Decode` (`h264_decode_parse()` - the actual H.264
  decode and YUV->RGB565 conversion together, see below), `GetFrame` (pulling the
  decoded buffer out, expected ~0ms), `Push` (`amoled.pushColors()`), `Ack`, and
  `Total`. Useful for seeing exactly where time goes on real content - see "Why this is
  slower than other esp_h264 demos" above for what that breakdown typically shows.
- `h264_decode.c`/`.h` wrap `esp_h264`'s streaming decoder API: `h264_decode_open()`
  once at boot, `h264_decode_reset()` on every new WebSocket connection (feeding a fresh
  SPS/PPS/IDR sequence into an already-active decode session can crash the underlying
  tinyh264 decoder), `h264_decode_parse()` per received chunk, `h264_decode_get_frame()`
  to pull out a decoded frame already converted to RGB565.
- `esp_h264_dec_get_resolution()` reports the macroblock-aligned decode size (e.g.
  608x464 for a 600x450 stream), not the true frame size - not useful for sizing the
  display push, so `h264_decode_open()` is instead given the exact width/height to use
  directly (the attached panel's detected size - see `DISPLAY_WIDTH`/`DISPLAY_HEIGHT` in
  [webH264.cpp](./webH264.cpp)) rather than querying it back from the decoder.
- The WebSocket message-reassembly buffer in `webH264.cpp` is allocated once at a fixed
  size (128KB) and reused for every frame, never `malloc`'d/`free`'d per-message -
  repeated variable-size allocations on the PSRAM heap over a long session is a classic
  embedded fragmentation crash, and this sidesteps it entirely.
- `loop()` calls `delay(1)` after `ws.cleanupClients()`: without it, `loopTask` (higher
  priority than the idle task) never yields, and the idle task on that core never runs -
  the task watchdog will fire continuously (harmless by itself, but worth explaining so
  it isn't mistaken for a real hang if you see it in the serial log).
- `/boardinfo` (JSON: `variant`, `name`, `width`, `height`, `boardId`) is what
  `stream.html` uses to tell this firmware apart from webJPEG's. Served with
  `Access-Control-Allow-Origin: *` since the calling page is cross-origin (GitHub Pages,
  not the device) - safe here since every endpoint is read-only board/status info or the
  streaming WebSocket, nothing sensitive.
