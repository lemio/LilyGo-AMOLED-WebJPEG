/**
 * @file      webH264.cpp
 * @license   MIT
 * @note      Streams a browser tab, window, or camera to the T4-S3 AMOLED display over
 *            WiFi as H.264, using the browser's WebCodecs VideoEncoder and Espressif's
 *            esp_h264 (tinyh264) software decoder on-device.
 *
 *            Unlike webJPEG.ino, this is a .cpp file, not a .ino: esp_h264 ships as a
 *            prebuilt IDF component wired in via the IDF Component Manager, which the
 *            plain Arduino/SCons build can't consume. This example instead builds
 *            Arduino as an ESP-IDF component (see platformio.ini's [env:webH264]:
 *            framework = arduino, espidf), which needs a real CMake project - hence
 *            CMakeLists.txt/idf_component.yml alongside this file, a root
 *            CMakeLists.txt, sdkconfig.webH264.defaults, and partitions.webH264.csv.
 *            The .ino preprocessor (auto prototypes, implicit Arduino.h) only exists in
 *            the plain-Arduino/SCons builder, so none of that applies here either.
 *
 *            This example is T4-S3 (2.41", RM690B0, 600x450) specific - unlike
 *            webJPEG's auto-detect-any-panel support, the H.264 stream's resolution is
 *            fixed at both ends (browser encoder config and on-device decode buffer
 *            sizing both hardcode 600x450), so it won't adapt to the 1.47"/1.91" boards.
 *
 * Required libraries:
 * - ESPAsyncWebServer / AsyncTCP (esp32async forks, same as webJPEG)
 * - TFT_eSPI (already a repo-wide dependency, used here only for its Sprite class -
 *   this display has no font/text API of its own)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LilyGo_AMOLED.h>
#include <TFT_eSPI.h>

extern "C" {
#include "h264_decode.h"
}

// WiFi credentials. ssid/password have no sensible default (must be set via the browser
// flasher or before compiling) - see examples/webJPEG/webJPEG.ino for the full
// explanation of this placeholder convention.
const char ssid[100] = "|*S*|";
const char password[100] = "|*P*|";

// LilyGo T4-S3 AMOLED panel (RM690B0) native resolution.
static const int DISPLAY_WIDTH = 600;
static const int DISPLAY_HEIGHT = 450;

LilyGo_Class amoled;
AsyncWebServer server(80);
AsyncWebSocket ws("/video-stream");

h264_decoder_t *decoder = NULL;

// Off-screen framebuffer used only for status text between/around video
// frames; amoled has no text/font API of its own, so we draw into this
// TFT_eSPI sprite (backed by PSRAM, never touches real TFT_eSPI hardware
// pins) and push its pixels the same way we push decoded video frames.
TFT_eSPI tft;
TFT_eSprite statusSpr = TFT_eSprite(&tft);

static void showStatus(const char *line1, const char *line2 = nullptr) {
  statusSpr.fillSprite(TFT_BLACK);
  statusSpr.setTextDatum(MC_DATUM);
  statusSpr.setTextColor(TFT_WHITE, TFT_BLACK);
  statusSpr.drawString(line1, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 - (line2 ? 14 : 0), 4);
  if (line2) {
    statusSpr.drawString(line2, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2 + 14, 2);
  }
  amoled.pushColors(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, (uint16_t *)statusSpr.getPointer());
}

// AsyncWebSocket delivers large messages (e.g. a keyframe chunk with
// SPS+PPS+IDR bundled in) across multiple WS_EVT_DATA calls rather than one;
// info->index/len mark this fragment's offset within the full message. This
// buffer reassembles those fragments before handing a complete message to
// the decoder. Allocated once at a fixed size and reused for every message
// (never malloc'd/freed per-frame) so hours of streaming can't fragment the
// PSRAM heap - a classic long-running-crash cause on embedded targets.
static const size_t WS_ASSEMBLY_BUF_SIZE = 128 * 1024;
static uint8_t *wsAssemblyBuf = NULL;
static size_t wsAssemblyLen = 0;

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
                       void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("Client connected from %s\n", client->remoteIP().toString().c_str());
    // A fresh SPS/PPS/IDR sequence landing on an already-active decode
    // session (e.g. a browser reconnect) can crash the underlying tinyh264
    // decoder, so start every new session from a clean decoder state.
    h264_decode_reset(decoder);
    showStatus("Streaming...");
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.println("Client disconnected");
    showStatus("Waiting for stream...", ("ws://" + WiFi.localIP().toString() + "/video-stream").c_str());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->opcode != WS_BINARY) {
      return;
    }

    if (info->len > WS_ASSEMBLY_BUF_SIZE) {
      Serial.printf("Dropping oversized WS message: %llu bytes\n", (unsigned long long)info->len);
      return;
    }
    if (info->index == 0) {
      wsAssemblyLen = 0;
    }
    if (!wsAssemblyBuf || info->index + len > info->len || info->index + len > WS_ASSEMBLY_BUF_SIZE) {
      return;
    }
    memcpy(wsAssemblyBuf + info->index, data, len);
    wsAssemblyLen += len;

    if (!info->final || wsAssemblyLen != info->len) {
      return;
    }

    if (h264_decode_parse(decoder, wsAssemblyBuf, wsAssemblyLen) == 0) {
      uint8_t *rgb_buffer = NULL;
      int width = 0, height = 0;
      if (h264_decode_get_frame(decoder, &rgb_buffer, &width, &height) == 0) {
        if (width <= DISPLAY_WIDTH && height <= DISPLAY_HEIGHT) {
          amoled.pushColors(0, 0, width, height, (uint16_t *)rgb_buffer);
          static uint32_t frameCount = 0;
          if (++frameCount % 30 == 1) {
            Serial.printf("pushed frame #%lu (%dx%d)\n", (unsigned long)frameCount, width, height);
          }
        }
      }
    }

    // Tiny flow-control signal: tells the browser we're ready for another
    // frame. All pacing/skip decisions live client-side (see h264_stream.html)
    // so this device only ever does one thing per message: decode, maybe
    // push, ack. No rate tracking, no timers, no state beyond the decoder.
    client->text("a");
  }
}

void setup() {
  Serial.begin(115200);

  // amoled.begin() auto-detects the panel, but for the 2.41" T4-S3 it
  // defaults to also mounting an SD card; with none inserted, the failure
  // path hits an uninitialized FreeRTOS queue and panics (boot loop). This
  // example is T4-S3 only (see the file header), so call it directly with
  // SD disabled instead of the auto-detecting amoled.begin().
  if (!amoled.beginAMOLED_241(/* disable_sd= */ true)) {
    Serial.println("Failed to detect the LilyGo AMOLED panel!");
    while (1) delay(1000);
  }
  amoled.setRotation(0);
  amoled.setBrightness(175);

  statusSpr.setColorDepth(16);
  statusSpr.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT);

  decoder = h264_decode_open();
  if (!decoder) {
    Serial.println("Fatal: failed to open H.264 decoder!");
    showStatus("Fatal error", "H.264 decoder failed to open");
    while (1) delay(1000);
  }

  wsAssemblyBuf = (uint8_t *)heap_caps_malloc(WS_ASSEMBLY_BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (!wsAssemblyBuf) {
    Serial.println("Fatal: failed to allocate WS assembly buffer!");
    showStatus("Fatal error", "Out of memory");
    while (1) delay(1000);
  }

  showStatus("Connecting to WiFi", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  // h264_stream.html is served from your computer (e.g. `python3 -m http.server`),
  // not from the ESP32: getDisplayMedia() only works in a browser "secure
  // context" (https:// or http://localhost), and this device only serves
  // plain http:// over the LAN. Point h264_stream.html's WebSocket URL at:
  String wsUrl = "ws://" + WiFi.localIP().toString() + "/video-stream";
  Serial.println(wsUrl);
  showStatus("Waiting for stream...", wsUrl.c_str());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();
}

void loop() {
  ws.cleanupClients();
  // loopTask runs at a higher priority than the idle task and never blocks
  // otherwise, so without this the idle task on this core never gets
  // scheduled and the task watchdog trips continuously.
  delay(1);
}
