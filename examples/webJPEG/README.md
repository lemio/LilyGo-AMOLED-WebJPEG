# webJPEG

Streams a browser tab, window, or camera to a LilyGo T-Display AMOLED board over WiFi.
The browser captures video with `getDisplayMedia`/`getUserMedia`, draws each frame to a
`<canvas>`, encodes it as a JPEG, and sends it over a plain `WebSocket`. The ESP32
decodes each JPEG and pushes it straight to the display.

This is the sibling of [webH264](../webH264) - see the root [README](../../README.md)
for how the two compare and which one to pick. Both share the same streaming page
([stream.html](../stream.html)) and the same on-device WiFi setup flow.

## Flashing

See the root [README](../../README.md#flashing) for the browser flasher (no PlatformIO
install needed) and the PlatformIO command-line steps. This example's PlatformIO
environment name is `webJPEG`.

## Board support

This example calls the display library's automatic board-detection `amoled.begin()`
(see `beginAutomatic()`/`begin()` in
[LilyGo_AMOLED.cpp](../../src/LilyGo_AMOLED.cpp)), which probes I2C addresses at boot to
tell the panels apart - no compile-time board selection needed. In principle the same
one firmware build works on:

| Board | Panel | Native resolution |
| ----- | ----- | ------------------ |
| T-Display AMOLED Lite 1.47" | SH8501 | 194x368 |
| T-Display AMOLED 1.91" (QSPI) | RM67162 | 240x536 |
| T-Display AMOLED 1.91" (SPI) | RM67162 | 240x536 |
| T-Display AMOLED 2.41" / T4-S3 | RM690B0 | 600x450 |

**Caveat:** this fork's changes (the WebSocket/JPEG rendering path, the `/boardinfo`
endpoint, this README) were developed against the 1.47" board; the 1.91" panel has
since been confirmed working on real hardware too (see "Measured performance" below).
The 2.41"/T4-S3 row is what the detection code *should* produce, read from the library
source, not something verified on that physical hardware here. If you hit a
board-specific problem (wrong colors, offset image, wrong dimensions), please open an
issue with your board model and a serial log from boot.

## Measured performance

Real numbers from the per-frame Serial log (see "Technical notes" below), not
estimates - useful as a baseline for judging your own setup.

| Board | Resolution | Avg decode | Avg render | Rendered-frame rate |
| ----- | ---------- | ---------- | ---------- | -------------------- |
| 1.91" | 536x240 | 1.6ms | 183ms | ~5.4fps |

(23 consecutive rendered frames, screen share content.)

JPEG decode itself is cheap - the real cost is `Render`, which (for a frame matching
the display size exactly, `Direct: YES` in the log) pushes each decoded 16x16 MCU block
to the display individually as it's decoded: 34x15 = 510 `pushColors()` calls for
536x240, averaging ~360 microseconds each. That's a lot of small display-bus
transactions rather than one bulk transfer - see "Technical notes" below for how this
differs from webH264's approach, which decodes the whole frame before a single
`pushColors()` call.

**The device has no flow control**, unlike webH264 (see that example's "Flow control"
section). It just renders what it can and silently drops the rest: in the same
session, 57 fully-received frames were dropped ("Mutex busy - frame dropped") against
24 that got rendered - about 70% of everything the browser successfully encoded and
sent was thrown away without ever reaching the screen, because the device was still
busy rendering the previous frame. That's wasted browser-side encode CPU and network
bandwidth for nothing. If you're seeing a lot of these in your own serial log, try
lowering **Frame Rate** in stream.html closer to what "Rendered-frame rate" in the
table above suggests your board can actually keep up with.

## Using it

1. Flash the firmware and open the Serial Monitor - it prints the assigned IP address,
   and the display itself shows a connection spinner, then the IP address and mDNS
   hostname once WiFi connects (see the root README's "Setup flow" section - this is
   identical to webH264's).
2. Visit `http://<that address>` (or `http://esp-webjpeg.local`, unless you set a
   different mDNS hostname). The board doesn't serve the streaming page itself - it
   302-redirects you to the HTTPS copy of [stream.html](../stream.html) hosted on GitHub
   Pages, pre-filling the `espAddress` field with the board's address and detecting that
   this is a webJPEG board. See the root README's "Why the redirect" section for why,
   and the one manual step it requires (allowing "insecure content" once per browser).
3. The Display Size dropdown auto-detects your board's resolution - override it
   manually if detection fails. Pick a source (webcam or screen share), then Start
   Streaming and grant the permission prompt. Captured video is stretched to fill the
   display exactly, so match its aspect ratio to avoid distortion.

## Options in stream.html

Once [stream.html](../stream.html) detects a webJPEG board (or you force "Mode: Force
WebJPEG"), it shows:

- **Display Size** - auto-filled from `/boardinfo`; override manually if it's wrong or
  the board is unreachable at page load.
- **Source** - webcam or screen share.
- **Frame Rate** - how often the browser captures and sends a frame.
- **JPEG Quality** - lower for less bandwidth/lag, higher for image quality.
- **Monochrome Mode** - converts frames to grayscale before encoding; smaller frames,
  faster to send, at the cost of colour. Sent over a separate `/ws-mono` WebSocket
  endpoint from colour mode's `/ws`.

`stream.html` also accepts URL query parameters for bookmarking a specific setup:
`espAddress`, `displaySize` (`WxH` or a predefined size), `customWidth`/`customHeight`
(when `displaySize=custom`), `sourceType` (`camera`/`screen`), `frameRate` (1-30),
`quality` (0.1-1.0), `monoMode` (`true`/`false`).

## Troubleshooting

- **"Start Streaming" does nothing / permission prompt never appears:** you're probably
  on the board's own `http://` page rather than the HTTPS redirect target - check the
  address bar says `https://lemio.github.io/...`. If it already does, see the root
  README's "Why the redirect" for the one-time insecure-content exception the WebSocket
  needs.
- **Image looks wrong (bands, wrong colors, partial screen):** the display size selected
  in the browser must match your physical board. Auto-detection should get this right;
  if it didn't (e.g. `/boardinfo` unreachable), set it manually from the table above.
- **Choppy / laggy stream:** lower the frame rate or JPEG quality slider - see
  "Measured performance" above for roughly what frame rate your board can sustain.
  A high drop count ("Mutex busy - frame dropped" in the serial log) means the browser
  is sending faster than the device can render; lowering Frame Rate reduces wasted
  encode/network work even though the *rendered* rate is capped by the device either way.

## Technical notes

- Every rendered frame logs a per-stage timing breakdown to Serial: `Decode` (JPEG
  decode itself, typically ~1-3ms - cheap), `Setup` (centering/offset math), `Render`
  (pushing decoded pixels to the display - the dominant cost, see "Measured
  performance" above), `Push` (bulk display push, only used when the frame doesn't
  match the display size exactly and had to go through a sprite buffer - 0ms otherwise,
  see below), and `Total`.
- When the incoming JPEG exactly matches the display's resolution (`Direct: YES` in the
  log), each decoded MCU block is pushed to the display individually as it's decoded
  (many small `pushColors()` calls) rather than assembled into a full frame buffer
  first - unlike webH264, which always decodes the whole frame before a single bulk
  `pushColors()` call. This wasn't something either example was tuned against the
  other for; it's just a real difference worth knowing if you're comparing the two.
- Board IDs returned by `amoled.getBoardID()` (see
  [LilyGo_AMOLED.h](../../src/LilyGo_AMOLED.h)):
  `LILYGO_AMOLED_147=1`, `LILYGO_AMOLED_191=2`, `LILYGO_AMOLED_241=3`,
  `LILYGO_AMOLED_191_SPI=4`.
- Color format is RGB565. `webJPEG.cpp` only byte-swaps when pushing pixels straight to
  the display (`amoled.pushColors`) - the `TFT_eSprite` buffer path swaps internally via
  `setSwapBytes(true)`, so swapping there too would double-flip the colors.
- When the incoming JPEG doesn't exactly match the display's resolution, frames are
  rendered into a sprite buffer and centered instead of pushed directly - slower, but
  avoids corrupting partial-tile edges.
- `/boardinfo` (JSON: `variant`, `name`, `width`, `height`, `boardId`) is what
  `stream.html` uses to tell this firmware apart from webH264's and to auto-fill the
  display size. Served with `Access-Control-Allow-Origin: *` since the calling page is
  cross-origin (GitHub Pages, not the device) - safe here since every endpoint is
  read-only board/status info or the streaming WebSocket, nothing sensitive.
