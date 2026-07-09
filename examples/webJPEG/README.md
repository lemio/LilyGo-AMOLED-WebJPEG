# webJPEG

Streams a browser tab, window, or camera to a LilyGo T-Display AMOLED board over WiFi.
The browser captures video with `getDisplayMedia`/`getUserMedia`, draws each frame to a
`<canvas>`, encodes it as a JPEG, and sends it over a plain `WebSocket` - the ESP32
decodes each JPEG and pushes it straight to the display. (This is not the WebRTC
protocol - no `RTCPeerConnection`/SDP/ICE - despite the repo's history using that name.)

## Flashing prebuilt firmware

Don't want to install PlatformIO just to try this? Every push to `main` is built
automatically and published as ready-to-flash firmware at
**[lemio.github.io/LilyGo-AMOLED-WebJPEG](https://lemio.github.io/LilyGo-AMOLED-WebJPEG/)**
- open it in Chrome or Edge (Web Serial support required), plug the board in over USB,
and flash it directly from the browser using
[ESP32-S3-Flasher](https://github.com/lemio/ESP32-S3-Flasher). The page also lets you
fill in your WiFi SSID/password/mDNS hostname - they get patched into the firmware at
flash time, no recompiling needed.

*One-time setup for maintainers: GitHub Pages must be enabled once in this repo's
Settings > Pages, with source set to the `docs/` folder on `main` - the workflow at
[build-and-publish-flasher.yml](../../.github/workflows/build-and-publish-flasher.yml)
keeps `docs/` up to date after that.*

## Flashing with PlatformIO

```bash
pio run -e T-Display-AMOLED --target upload
```

Before compiling, either use the browser flasher above to set your WiFi credentials, or
edit the placeholder values directly in [webJPEG.ino](./webJPEG.ino):

```cpp
const char ssid[100] = "|*S*|";           // replace with your WiFi SSID
const char password[100] = "|*P*|";       // replace with your WiFi password
const char mdnsName[100] = "esp-webjpeg"; // reachable at http://esp-webjpeg.local
```

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

1. Flash the firmware (see above) and open the Serial Monitor - it prints the assigned
   IP address, and the display itself shows it plus the mDNS hostname once WiFi
   connects.
2. Visit `http://<that address>` (or `http://esp-webjpeg.local`, unless you set a
   different mDNS hostname) in Chrome, Edge, or Firefox. This is served directly by the
   board, so there's no separate file to open - though
   [webrtc_stream.html](./webrtc_stream.html) is the same page, kept in the repo so it
   can also be opened as a local file (e.g. to point it at a board whose mDNS name
   doesn't resolve on your network).
3. Click "🔍 Detect Board" to auto-fill the correct display size, or set it manually -
   the dropdown lists the resolutions above.
4. Pick a source (webcam or screen share), then "▶️ Start Streaming" and grant the
   permission prompt.

### URL query parameters

`webrtc_stream.html`/the device's own `/` page accept the same query parameters, useful
for bookmarking a specific setup:

- `espAddress` - ESP32 URL (defaults to the page's own address when served by the board)
- `displaySize` - `WxH` or one of the predefined sizes
- `customWidth`, `customHeight` - used when `displaySize=custom`
- `sourceType` - `camera` or `screen`
- `frameRate` - FPS (1-30)
- `quality` - JPEG quality (0.1-1.0)
- `monoMode` - `true`/`false`

Example: `webrtc_stream.html?espAddress=http://192.168.1.88&displaySize=600x450&frameRate=10`

## Troubleshooting

- **Board not detected / `/boardinfo` fails:** confirm the ESP32 connected to WiFi (check
  the Serial Monitor), then try the board's raw IP address instead of the mDNS hostname.
- **Image looks wrong (bands, wrong colors, partial screen):** the display size selected
  in the browser must match the physical board. Click "🔍 Detect Board" rather than
  picking a size by hand.
- **Choppy / laggy stream:** lower the frame rate or JPEG quality slider.

## Technical notes

- Board IDs returned by `amoled.getBoardID()` (see
  [LilyGo_AMOLED.h](../../src/LilyGo_AMOLED.h)):
  `LILYGO_AMOLED_147=1`, `LILYGO_AMOLED_191=2`, `LILYGO_AMOLED_241=3`,
  `LILYGO_AMOLED_191_SPI=4`.
- Color format is RGB565. `webJPEG.ino` only byte-swaps when pushing pixels straight to
  the display (`amoled.pushColors`) - the `TFT_eSprite` buffer path swaps internally via
  `setSwapBytes(true)`, so swapping there too would double-flip the colors.
- When the incoming JPEG doesn't exactly match the display's resolution, frames are
  rendered into a sprite buffer and centered instead of pushed directly - slower, but
  avoids corrupting partial-tile edges.
