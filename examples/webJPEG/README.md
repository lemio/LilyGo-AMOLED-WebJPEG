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
   different mDNS hostname) in Chrome, Edge, or Firefox. The board doesn't serve the
   streaming page itself - it 302-redirects you to the HTTPS copy of
   [webrtc_stream.html](./webrtc_stream.html) hosted on GitHub Pages, pre-filling the
   `espAddress` field with the board's address. See "Why the redirect" below for why,
   and the one manual step it requires.
3. The Display Size dropdown auto-detects your board's resolution (via `/boardinfo`) -
   override it manually if detection fails or you're pointing at a different board.
   Pick a source (webcam or screen share), then "▶️ Start Streaming" and grant the
   permission prompt. Whatever you capture is automatically cropped and scaled to fill
   the display exactly, with no manual pre-resizing (e.g. via devtools) needed.

### Why the redirect

`getDisplayMedia()`/`getUserMedia()` (screen share / camera capture) only work in a
"secure context" - HTTPS, or literal `localhost`/`127.0.0.1`. A LAN address or `.local`
mDNS name served over plain `http://`, which is all this board can do, doesn't qualify -
so serving the streaming page directly from the board would load fine but silently have
no working "Start Streaming" button. Redirecting to the same page hosted over HTTPS on
GitHub Pages fixes that. Two consequences of this, worth knowing:

- **You need internet access** to reach GitHub Pages, even though the actual video
  stream stays entirely on your LAN afterwards. If you don't have internet access, open
  [webrtc_stream.html](./webrtc_stream.html) as a local file instead (`file://` also
  counts as a secure context, and it doesn't need to reach GitHub Pages at all) and fill
  in `espAddress` yourself.
- **The WebSocket connection back to the board is `ws://`, not `wss://`** (the board
  can't easily serve TLS). Browsers block that as "mixed content" by default when the
  page itself is HTTPS, so the stream will fail to start the first time. In Chrome/Edge:
  click the padlock/tune icon left of the address bar → **Site settings** → **Insecure
  content** → **Allow**, then reload. This is a one-time exception per browser, tied to
  the `lemio.github.io` origin - it doesn't weaken anything else, since the "insecure"
  traffic is only ever going to your own board on your own LAN.
- **`fetch()` calls back to the board (like `/boardinfo`, used for auto-detecting
  display size) are cross-origin too**, since the page's origin is now `github.io`, not
  the board. `webJPEG.ino` sends `Access-Control-Allow-Origin: *` on every response to
  allow this - safe here since every endpoint is read-only board/status info or the
  streaming WebSocket, nothing sensitive.

### URL query parameters

`webrtc_stream.html` accepts these query parameters, useful for bookmarking a specific
setup (the board's own `/` redirect only ever sets `espAddress`):

- `espAddress` - ESP32 URL (set automatically by the board's redirect)
- `displaySize` - `WxH` or one of the predefined sizes
- `customWidth`, `customHeight` - used when `displaySize=custom`
- `sourceType` - `camera` or `screen`
- `frameRate` - FPS (1-30)
- `quality` - JPEG quality (0.1-1.0)
- `monoMode` - `true`/`false`

Example: `webrtc_stream.html?espAddress=http://192.168.1.88&displaySize=600x450&frameRate=10`

## Troubleshooting

- **"Start Streaming" does nothing / permission prompt never appears:** you're probably
  on the board's own `http://` page rather than the HTTPS redirect target - check the
  address bar says `https://lemio.github.io/...`. If it already does, see "Why the
  redirect" above for the one-time insecure-content exception the WebSocket needs.
- **Image looks wrong (bands, wrong colors, partial screen):** the display size selected
  in the browser must match your physical board. Auto-detection should get this right;
  if it didn't (e.g. `/boardinfo` unreachable), set it manually from the table above.
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
