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
endpoint, this README) were developed and compile-tested against the 1.47" board only.
The 1.91"/2.41" rows above are what the detection code *should* produce, read from the
library source, not something verified on that physical hardware here. If you hit a
board-specific problem (wrong colors, offset image, wrong dimensions), please open an
issue with your board model and a serial log from boot.

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
- **Choppy / laggy stream:** lower the frame rate or JPEG quality slider.

## Technical notes

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
