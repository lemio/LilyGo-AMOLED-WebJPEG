<h1 align = "center">LilyGo-AMOLED-WebJPEG</h1>
<p align="center"><i>Use a LilyGo AMOLED display as an external monitor for prototyping</i></p>

[![PlatformIO CI](https://github.com/lemio/LilyGo-AMOLED-WebJPEG/actions/workflows/platformio.yml/badge.svg)](https://github.com/lemio/LilyGo-AMOLED-WebJPEG/actions/workflows/platformio.yml)

## What this is

Designing content for a small display is fiddly: draw a frame, convert it to a C array
with something like image2cpp, reflash, look at it on the real hardware, repeat. This
project skips that loop entirely - point your browser at anything (a browser tab, a
Figma mirror, a p5 sketch, a screen recording, your webcam) and it shows up on the
physical AMOLED display over WiFi, live, in full colour. Change the design, the page
just re-renders - no reflashing.

It's a fork of LilyGO's [LilyGo-AMOLED-Series](https://github.com/Xinyuan-LilyGO/LilyGo-AMOLED-Series)
board-support library, trimmed down to two streaming examples (see below) plus a
browser-based flasher - no PlatformIO install required just to try it.

This is the same idea as an
[earlier project of mine](https://www.hackster.io/lemio/using-oled-display-as-external-monitor-afda57)
that pushed pixels from a Processing sketch to a monochrome OLED over serial, one bit
per pixel - just rebuilt around WiFi and the browser's own screen-capture APIs instead
of a serial link and a script watching your cursor, and full colour instead of 1-bit.

## Which example do I want?

| | [webJPEG](examples/webJPEG) | [webH264](examples/webH264) |
| --- | --- | --- |
| Codec | MJPEG (one JPEG per frame) | H.264 (WebCodecs `VideoEncoder`) |
| Boards | 1.47" Lite, 1.91", 2.41"/T4-S3 (auto-detected) | 2.41"/T4-S3 only (fixed 600x450) |
| Frame rate | Up to ~30fps, tunable | ~3-5fps (decoder-bound) |
| Bandwidth | Higher for the same visual quality | Much lower - H.264 compresses far better |
| Best for | Most boards, smoothest motion | T4-S3 owners who want a sharper image at lower bandwidth and don't mind a slower decoder |

Both examples share the same streaming page and WiFi setup flow - see
[stream.html](examples/stream.html), which auto-detects which one is running on the
board you point it at.

## Flashing

Don't want to install PlatformIO just to try this? Every push to `main` is built
automatically and published as ready-to-flash firmware at
**[lemio.github.io/LilyGo-AMOLED-WebJPEG](https://lemio.github.io/LilyGo-AMOLED-WebJPEG/)**:

1. Open that link in **Chrome or Edge** (Web Serial support is required).
2. Pick **WebJPEG Stream Display** or **WebH264 Stream Display** (see the comparison
   table above).
3. Fill in your WiFi SSID, password, and optionally an mDNS hostname - these get
   patched into the firmware at flash time, no recompiling needed.
4. Plug the board in over USB and follow the on-screen flashing steps.
5. Once it reboots and connects to WiFi, the display shows its IP address and mDNS
   hostname - visit that address in a browser to start streaming (see "Setup flow"
   below for exactly what that looks like, and each example's own README for the
   streaming controls).

### Flashing with PlatformIO

Prefer the command line, or need to set WiFi credentials at compile time instead? See
[3️⃣ PlatformIO Quick Start](#3️⃣-platformio-quick-start-recommended) below, then:

```bash
pio run -e webJPEG --target upload   # or -e webH264
```

Before compiling, either use the browser flasher above to set your WiFi credentials, or
edit the placeholder values directly in that example's `.cpp` file:

```cpp
const char ssid[100] = "|*S*|";           // replace with your WiFi SSID
const char password[100] = "|*P*|";       // replace with your WiFi password
const char mdnsName[100] = "esp-webjpeg"; // reachable at http://esp-webjpeg.local
```

## Setup flow

Both examples show the same sequence on the display after flashing:

1. **Connecting to WiFi** - your SSID, a spinner, and a progress bar while it attempts
   to connect (30 attempts, ~15 seconds).
2. **On success:** "WiFi Connected", your SSID, the board's IP address, its mDNS
   hostname (`http://esp-webjpeg.local` / `http://esp-webh264.local`), then "Waiting for
   stream...".
3. **On failure:** a QR code linking back to this repo, so you can pull out your phone
   and find setup help without needing a second computer.

Visit the address from step 2 in a browser - the board redirects you to the shared
[stream.html](examples/stream.html) page (see "Why the redirect" below), which
auto-detects which example is running and shows the matching controls.

### Why the redirect

`getDisplayMedia()`/`getUserMedia()` (screen share / camera capture) only work in a
"secure context" - HTTPS, or literal `localhost`/`127.0.0.1`. A LAN address or `.local`
mDNS name served over plain `http://`, which is all the board can do, doesn't qualify -
so serving the streaming page directly from the board would load fine but silently have
no working "Start Streaming" button. Redirecting to the same page hosted over HTTPS on
GitHub Pages fixes that. Two consequences worth knowing:

- **You need internet access** to reach GitHub Pages, even though the actual video
  stream stays entirely on your LAN afterwards. If you don't have internet access, open
  [stream.html](examples/stream.html) as a local file instead (`file://` also counts as
  a secure context, and it doesn't need to reach GitHub Pages at all) and fill in
  `espAddress` yourself.
- **The WebSocket connection back to the board is `ws://`, not `wss://`** (the board
  can't easily serve TLS). Browsers block that as "mixed content" by default when the
  page itself is HTTPS, so the stream will fail to start the first time. In Chrome/Edge:
  click the padlock/tune icon left of the address bar → **Site settings** → **Insecure
  content** → **Allow**, then reload. This is a one-time exception per browser, tied to
  the `lemio.github.io` origin - it doesn't weaken anything else, since the "insecure"
  traffic only ever goes to your own board on your own LAN.

## 1️⃣ Support Product

| Product(PinMap)               | SOC        | Flash | PSRAM    | Resolution | Size      |
| ----------------------------- | ---------- | ----- | -------- | ---------- | --------- |
| [T-Display-AMOLED-Lite][1]    | ESP32-S3R8 | 16MB  | 8MB(OPI) | 194x368    | 1.47 Inch |
| [T-Display-S3 AMOLED][2]      | ESP32-S3R8 | 16MB  | 8MB(OPI) | 240x536    | 1.91 Inch |
| [T4-S3][3]                    | ESP32-S3R8 | 16MB  | 8MB(OPI) | 450x600    | 2.41 Inch |

[1]: https://www.lilygo.cc/products/t-display-amoled-lite
[2]: https://www.lilygo.cc/products/t-display-s3-amoled
[3]: https://www.lilygo.cc/products/t4-s3

## 2️⃣ Examples

```txt
examples/
├── stream.html  # Shared browser streaming page for both examples below - auto-detects which one is running
├── webJPEG      # MJPEG over WebSocket - works on all boards above (see comparison table)
└── webH264      # H.264 over WebSocket - T4-S3 only, sharper image, lower bandwidth
```

See each example's own README ([webJPEG](examples/webJPEG/README.md),
[webH264](examples/webH264/README.md)) for streaming controls, troubleshooting, and
technical notes.

## 3️⃣ PlatformIO Quick Start (Recommended)

1. Install [Visual Studio Code](https://code.visualstudio.com/) and [Python](https://www.python.org/)
2. Search for the `PlatformIO` extension in VS Code's Extensions panel and install it,
   then restart VS Code.
3. `File` → `Open Folder` → select this repo's root folder.
4. Wait for PlatformIO to finish installing dependencies (progress shown in its own
   output panel).
5. In the PlatformIO sidebar (the alien-head icon), pick the `webJPEG` or `webH264`
   environment, then **Build** (✔) → **Upload** (→).
6. Connect the board over USB, then click **Upload** (→) again if it didn't
   auto-detect the port.
7. Click the plug icon to open the Serial Monitor and watch it boot.
8. If it can't be written, or the USB device keeps flashing, see the **FAQ** below.

## 4️⃣ FAQ

1. **Can't find the upload port / USB has been used for other functions:** the board
   uses USB as its JTAG/upload port, which needs `USB CDC On Boot` enabled to also
   print serial output. If the port disappears, enter upload mode manually:

   1. Connect the board via USB
   2. Press and hold **BOOT**, then, still holding it, press and release **RST**
   3. Release **BOOT**
   4. Upload

2. **1.47" AMOLED (T-Display-AMOLED-Lite)** does not support hardware screen rotation.
3. **Running on battery/external power instead of USB-C:** turn off `USB CDC On Boot` -
   otherwise the board waits for USB access at startup. In `platformio.ini`:

   ```ini
   build_flags =
       ; Enable -DARDUINO_USB_CDC_ON_BOOT will start printing and wait for terminal access during startup
       ; -DARDUINO_USB_CDC_ON_BOOT=1

       ; Enable -UARDUINO_USB_CDC_ON_BOOT will turn off printing and will not block when using the battery
       -UARDUINO_USB_CDC_ON_BOOT
   ```

   Note this also turns off Serial output over USB-C; output moves to GPIO43/GPIO44
   instead.

## 5️⃣ Resource

| Product(PinMap)               | schematic                                                | Dimensions                                        | PCB 3D                                                  | Shell                                  |
| ----------------------------- | -------------------------------------------------------- | ------------------------------------------------- | ------------------------------------------------------- | --------------------------------------- |
| [T-Display-AMOLED-Lite][1]    | [schematic](./schematic/T-Display_AMOLED-Lite.pdf)       | [DWG](./dimensions/T-Display-AMOLED-Lite.DWG)     | [STP](./dimensions/T-Display-AMOLED-Lite-PCB-3D.7z)     | x                                       |
| [T-Display-S3 AMOLED][2]      | [schematic](./schematic/T-Display-S3-AMOLED-Touch.pdf)   | [DWG](./dimensions/T-Display-S3-AMOLED-Touch.DWG) | [STP](./dimensions/T-Display-S3-AMOLED-Touch-PCB-3D.7z) | [STP](./shell/T-Display-S3-AMOLED.zip) |
| [T4-S3][3]                    | [schematic](./schematic/T-Display-S3%20AMOLED-Touch.pdf) | [DWG](./dimensions/T4-S3-AMOLED.DWG)              | [STP](./dimensions/T4-S3-AMOLED-PCB-3D.7z)              | [STP](./shell/T4-S3-AMOLED.zip)        |

##### Datasheets

- [ESP32S3-R8](https://www.espressif.com.cn/en/support/documents/technical-documents?keys=&field_type_tid%5B%5D=842)
- [T-Display-AMOLED-Lite AMOLED Driver (SH8501B0)](./datasheet/SH8501B0%20DataSheet.pdf)
- [T-Display-S3 AMOLED Driver (RM67162)](./datasheet/RM67162%20DataSheet.pdf)
- [T4-S3 AMOLED Driver (RM690B0)](./datasheet/RM690B0%20DataSheet_V0.2.pdf)
- [Touchpad Driver (CHSC5816)](https://github.com/lewisxhe/SensorLib/blob/master/datasheet/CHSC5816%E8%A7%A6%E6%8E%A7%E8%8A%AF%E7%89%87%E4%BD%BF%E7%94%A8%E8%AF%B4%E6%98%8EV1-20221114.pdf)
- [PMU (AXP2101)](https://github.com/lewisxhe/XPowersLib/blob/master/datasheet/AXP2101_Datasheet_V1.0_en.pdf)
- [Light Sensor (CM32181)](https://github.com/lewisxhe/SensorLib/blob/master/datasheet/CM32181A3OP.pdf)

## License

MIT - see [LICENSE](./LICENSE). Built on
[LilyGo-AMOLED-Series](https://github.com/Xinyuan-LilyGO/LilyGo-AMOLED-Series) by
LilyGO/Xinyuan-LilyGO.
