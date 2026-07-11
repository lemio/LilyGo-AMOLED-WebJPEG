/**
 * @file      webJPEG.cpp
 * @author    Modified for browser-to-display streaming
 * @license   MIT
 * @copyright Copyright (c) 2023  Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2023-06-14
 * @note      Streams a browser tab, window, or camera to the AMOLED display over WiFi.
 *            This is plain WebSocket + MJPEG (canvas.toBlob), not the WebRTC protocol -
 *            no RTCPeerConnection/SDP/ICE. Optimized for maximum performance with
 *            direct rendering.
 *
 * Required libraries:
 * - ESPAsyncWebServer: https://github.com/me-no-dev/ESPAsyncWebServer
 * - AsyncTCP: https://github.com/me-no-dev/AsyncTCP
 * - JPEGDecoder: https://github.com/Bodmer/JPEGDecoder
 */

#include <Arduino.h>
#include <LilyGo_AMOLED.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <JPEGDecoder.h>
#include "qrcodegen.h"

// WiFi credentials. ssid/password have no sensible default (must be set via the browser
// flasher or before compiling); mdnsName's default value doubles as its own flasher
// placeholder, so a board flashed without ever touching the browser flasher still gets
// a working hostname - see flasher-manifest.yml.
const char ssid[100] = "|*S*|";
const char password[100] = "|*P*|";
const char mdnsName[100] = "esp-webjpeg";

// Shown as a QR code so the user can find help/docs if WiFi connection fails
const char githubRepoUrl[] = "https://github.com/lemio/LilyGo-AMOLED-WebJPEG";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");          // color images
AsyncWebSocket wsMono("/ws-mono"); // monochrome images

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
LilyGo_Class amoled;

#define WIDTH  amoled.width()
#define HEIGHT amoled.height()

volatile uint8_t* frameBuffer = nullptr;
volatile size_t frameSize = 0;
volatile bool newFrameAvailable = false;
volatile bool isMonochrome = false;
SemaphoreHandle_t frameMutex;

// Reassembles fragmented WebSocket frames before they're handed off above
uint8_t* wsAssemblyBuffer = nullptr;
size_t wsAssemblySize = 0;
size_t wsExpectedSize = 0;

uint8_t* wsMonoAssemblyBuffer = nullptr;
size_t wsMonoAssemblySize = 0;
size_t wsMonoExpectedSize = 0;

volatile uint32_t frameCount = 0;
volatile uint32_t lastFrameTime = 0;

inline void swapBytes(uint16_t *data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        data[i] = (data[i] >> 8) | (data[i] << 8);
    }
}

void drawJPEG(uint8_t *jpegData, size_t jpegSize) {
    uint32_t t1 = millis();

    if (!JpegDec.decodeArray(jpegData, jpegSize)) {
        Serial.println("JPEG decode failed!");
        return;
    }

    uint32_t t2 = millis();

    uint16_t w = JpegDec.width;
    uint16_t h = JpegDec.height;

    if (w == 0 || h == 0) {
        Serial.println("Invalid JPEG dimensions!");
        return;
    }

    // Pushed straight to the display if the JPEG already matches its size, otherwise
    // rendered into a sprite and centered.
    bool directRender = (w == WIDTH && h == HEIGHT);

    Serial.printf("JPEG: %dx%d | Display: %dx%d | Direct: %s | MCU: %dx%d\n",
                  w, h, WIDTH, HEIGHT, directRender ? "YES" : "NO",
                  JpegDec.MCUWidth, JpegDec.MCUHeight);

    if (!directRender) {
        spr.fillSprite(TFT_BLACK);
    }

    int16_t offsetX = (WIDTH - w) >> 1;
    int16_t offsetY = (HEIGHT - h) >> 1;
    if (offsetX < 0) offsetX = 0;
    if (offsetY < 0) offsetY = 0;

    uint16_t mcu_w = JpegDec.MCUWidth;
    uint16_t mcu_h = JpegDec.MCUHeight;

    uint32_t t3 = millis();

    while (JpegDec.read()) {
        uint16_t *pImg = JpegDec.pImage;

        uint16_t mcu_x = JpegDec.MCUx * mcu_w;
        uint16_t mcu_y = JpegDec.MCUy * mcu_h;
        if (mcu_x >= w || mcu_y >= h) continue;

        // Clip this MCU block to the image bounds
        uint16_t valid_w = (mcu_x + mcu_w <= w) ? mcu_w : (w - mcu_x);
        uint16_t valid_h = (mcu_y + mcu_h <= h) ? mcu_h : (h - mcu_y);
        if (valid_w == 0 || valid_h == 0) continue;

        int16_t destX = offsetX + mcu_x;
        int16_t destY = offsetY + mcu_y;
        if (destX >= WIDTH || destY >= HEIGHT) continue;

        // Clip again to the display bounds
        uint16_t render_w = valid_w;
        uint16_t render_h = valid_h;
        if (destX + render_w > WIDTH) {
            render_w = WIDTH - destX;
        }
        if (destY + render_h > HEIGHT) {
            render_h = HEIGHT - destY;
        }
        if (render_w == 0 || render_h == 0) continue;

        // Edge blocks need only their valid portion copied out of the MCU buffer.
        // Byte-swap only for direct rendering - the sprite path swaps internally.
        if (render_w != mcu_w || render_h != mcu_h) {
            uint16_t tempBuffer[render_w * render_h];
            for (uint16_t row = 0; row < render_h; row++) {
                for (uint16_t col = 0; col < render_w; col++) {
                    uint16_t pixel = pImg[row * mcu_w + col];
                    tempBuffer[row * render_w + col] = directRender ? ((pixel >> 8) | (pixel << 8)) : pixel;
                }
            }

            if (directRender) {
                amoled.pushColors(destX, destY, render_w, render_h, tempBuffer);
            } else {
                spr.pushImage(destX, destY, render_w, render_h, tempBuffer);
            }
        } else {
            if (directRender) {
                swapBytes(pImg, mcu_w * mcu_h);
                amoled.pushColors(destX, destY, render_w, render_h, pImg);
            } else {
                spr.pushImage(destX, destY, render_w, render_h, pImg);
            }
        }
    }

    uint32_t t4 = millis();

    if (!directRender) {
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    }

    uint32_t t5 = millis();

    Serial.printf("Timing: Decode=%lums | Setup=%lums | Render=%lums | Push=%lums | Total=%lums\n",
                  t2-t1, t3-t2, t4-t3, t5-t4, t5-t1);
}

void drawMonoJPEG(uint8_t *jpegData, size_t jpegSize) {
    uint32_t t1 = millis();

    // Decode JPEG (grayscale)
    if (!JpegDec.decodeArray(jpegData, jpegSize)) {
        Serial.println("Mono JPEG decode failed!");
        return;
    }

    uint32_t t2 = millis();

    uint16_t w = JpegDec.width;
    uint16_t h = JpegDec.height;

    if (w == 0 || h == 0) {
        Serial.println("Invalid JPEG dimensions!");
        return;
    }

    Serial.printf("Mono JPEG: %dx%d | Display: %dx%d\n", w, h, WIDTH, HEIGHT);

    bool directRender = (w == WIDTH && h == HEIGHT);

    if (!directRender) {
        spr.fillSprite(TFT_BLACK);
    }

    int16_t offsetX = (WIDTH - w) >> 1;
    int16_t offsetY = (HEIGHT - h) >> 1;
    if (offsetX < 0) offsetX = 0;
    if (offsetY < 0) offsetY = 0;

    uint16_t mcu_w = JpegDec.MCUWidth;
    uint16_t mcu_h = JpegDec.MCUHeight;

    uint32_t t3 = millis();

    while (JpegDec.read()) {
        uint16_t *pImg = JpegDec.pImage;

        uint16_t mcu_x = JpegDec.MCUx * mcu_w;
        uint16_t mcu_y = JpegDec.MCUy * mcu_h;
        if (mcu_x >= w || mcu_y >= h) continue;

        uint16_t render_w = (mcu_x + mcu_w <= w) ? mcu_w : (w - mcu_x);
        uint16_t render_h = (mcu_y + mcu_h <= h) ? mcu_h : (h - mcu_y);
        if (render_w == 0 || render_h == 0) continue;

        int16_t destX = offsetX + mcu_x;
        int16_t destY = offsetY + mcu_y;
        if (destX >= WIDTH || destY >= HEIGHT) continue;

        if (destX + render_w > WIDTH) {
            render_w = WIDTH - destX;
        }
        if (destY + render_h > HEIGHT) {
            render_h = HEIGHT - destY;
        }

        if (render_w != mcu_w || render_h != mcu_h) {
            uint16_t tempBuffer[render_w * render_h];
            for (uint16_t row = 0; row < render_h; row++) {
                for (uint16_t col = 0; col < render_w; col++) {
                    uint16_t pixel = pImg[row * mcu_w + col];
                    tempBuffer[row * render_w + col] = directRender ? ((pixel >> 8) | (pixel << 8)) : pixel;
                }
            }

            if (directRender) {
                amoled.pushColors(destX, destY, render_w, render_h, tempBuffer);
            } else {
                spr.pushImage(destX, destY, render_w, render_h, tempBuffer);
            }
        } else {
            if (directRender) {
                swapBytes(pImg, render_w * render_h);
                amoled.pushColors(destX, destY, render_w, render_h, pImg);
            } else {
                spr.pushImage(destX, destY, render_w, render_h, pImg);
            }
        }
    }

    uint32_t t4 = millis();

    if (!directRender) {
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    }

    uint32_t t5 = millis();

    Serial.printf("Mono Timing: Decode=%lums | Setup=%lums | Render=%lums | Push=%lums | Total=%lums\n",
                  t2-t1, t3-t2, t4-t3, t5-t4, t5-t1);
}

// Renders `text` as a QR code into the sprite, centered at (centerX, centerY),
// scaled as large as possible within a maxSize x maxSize box.
#define QR_MAX_VERSION 10
void drawQRCode(const char *text, int16_t centerX, int16_t centerY, int16_t maxSize, uint16_t fgColor, uint16_t bgColor) {
    uint8_t qrTemp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    uint8_t qrOut[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];

    bool ok = qrcodegen_encodeText(text, qrTemp, qrOut, qrcodegen_Ecc_MEDIUM,
                                    qrcodegen_VERSION_MIN, QR_MAX_VERSION,
                                    qrcodegen_Mask_AUTO, true);
    if (!ok) {
        Serial.println("QR code generation failed!");
        return;
    }

    int size = qrcodegen_getSize(qrOut);
    int quietZoneModules = 2;
    int scale = maxSize / (size + quietZoneModules * 2);
    if (scale < 1) scale = 1;

    int totalSize = (size + quietZoneModules * 2) * scale;
    int startX = centerX - totalSize / 2;
    int startY = centerY - totalSize / 2;

    spr.fillRect(startX, startY, totalSize, totalSize, bgColor);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (qrcodegen_getModule(qrOut, x, y)) {
                spr.fillRect(startX + (quietZoneModules + x) * scale,
                             startY + (quietZoneModules + y) * scale,
                             scale, scale, fgColor);
            }
        }
    }
}

void setupWiFi() {
    Serial.println("Connecting to WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    const int maxAttempts = 30;
    const char spinnerFrames[] = {'|', '/', '-', '\\'};
    int attempts = 0;

    while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
        spr.fillSprite(TFT_BLACK);
        spr.setTextDatum(TC_DATUM);

        spr.setTextColor(TFT_WHITE, TFT_BLACK);
        spr.drawString("Connecting to WiFi", WIDTH / 2, HEIGHT * 0.10, 2);

        spr.setTextColor(TFT_CYAN, TFT_BLACK);
        spr.drawString(ssid, WIDTH / 2, HEIGHT * 0.10 + 22, 2);

        // Spinner: rotates once per attempt to show the connection is progressing
        char spinner[2] = { spinnerFrames[attempts % 4], '\0' };
        spr.setTextColor(TFT_WHITE, TFT_BLACK);
        spr.drawString(spinner, WIDTH / 2, HEIGHT * 0.52, 4);

        // Progress bar: fills based on attempts remaining before timeout
        int barWidth = WIDTH * 0.6;
        int barHeight = 8;
        int barX = (WIDTH - barWidth) / 2;
        int barY = HEIGHT - 28;
        spr.drawRect(barX, barY, barWidth, barHeight, TFT_DARKGREY);
        int fillWidth = (barWidth - 2) * attempts / maxAttempts;
        spr.fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, TFT_CYAN);

        spr.setTextDatum(TL_DATUM);
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());

        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected!");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());

        if (MDNS.begin(mdnsName)) {
            Serial.print("mDNS responder started: ");
            Serial.print(mdnsName);
            Serial.println(".local");
        } else {
            Serial.println("Error setting up mDNS");
        }

        spr.fillSprite(TFT_BLACK);
        spr.setTextDatum(TC_DATUM);
        spr.setTextColor(TFT_GREEN, TFT_BLACK);
        spr.drawString("WiFi Connected", WIDTH / 2, HEIGHT * 0.08, 2);
        spr.setTextColor(TFT_WHITE, TFT_BLACK);
        spr.drawString(ssid, WIDTH / 2, HEIGHT * 0.08 + 22, 2);
        spr.drawString(WiFi.localIP().toString(), WIDTH / 2, HEIGHT * 0.08 + 44, 2);
        spr.drawString("http://" + String(mdnsName) + ".local", WIDTH / 2, HEIGHT * 0.08 + 66, 2);
        spr.setTextColor(TFT_YELLOW, TFT_BLACK);
        spr.drawString("Waiting for stream...", WIDTH / 2, HEIGHT * 0.08 + 92, 2);
        spr.setTextDatum(TL_DATUM);
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    } else {
        Serial.println("\nWiFi connection failed!");
        spr.fillSprite(TFT_BLACK);
        spr.setTextDatum(TC_DATUM);

        // Font 1 keeps this long line from overflowing narrower displays
        spr.setTextColor(TFT_RED, TFT_BLACK);
        spr.drawString("Can't connect to WiFi network", WIDTH / 2, HEIGHT * 0.05, 1);
        spr.setTextColor(TFT_WHITE, TFT_BLACK);
        spr.drawString(ssid, WIDTH / 2, HEIGHT * 0.05 + 14, 2);

        // QR code links to the project repo so the user can look up setup help
        int textBottom = HEIGHT * 0.05 + 14 + 18;
        int captionHeight = 16;
        int qrMaxSize = min((int)WIDTH, (int)HEIGHT - textBottom - captionHeight) * 0.9;
        int qrCenterY = textBottom + (HEIGHT - captionHeight - textBottom) / 2;
        drawQRCode(githubRepoUrl, WIDTH / 2, qrCenterY, qrMaxSize, TFT_WHITE, TFT_BLACK);

        spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
        spr.drawString("Scan for setup help", WIDTH / 2, HEIGHT - 16, 2);

        spr.setTextDatum(TL_DATUM);
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    }
}

void setupWebServer() {
    // The streaming page is hosted on GitHub Pages (see the "/" redirect below), so its
    // fetch("/boardinfo") calls back to this device are cross-origin. Every endpoint
    // here is read-only board/status info or the streaming WebSocket - nothing
    // sensitive or mutating - so a wildcard is fine.
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

    // Serve board info as JSON - "variant" lets the shared stream.html page tell this
    // firmware apart from webH264's and adjust its controls/streaming protocol
    // accordingly (see examples/webH264/webH264.cpp's matching /boardinfo).
    server.on("/boardinfo", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{";
        json += "\"variant\":\"jpeg\",";
        json += "\"name\":\"" + String(amoled.getName()) + "\",";
        json += "\"width\":" + String(WIDTH) + ",";
        json += "\"height\":" + String(HEIGHT) + ",";
        json += "\"boardId\":" + String(amoled.getBoardID());
        json += "}";
        request->send(200, "application/json", json);
    });

    // Redirect to the HTTPS-hosted copy of the streaming page on GitHub Pages.
    // getDisplayMedia()/getUserMedia() require a "secure context" - plain http:// on a
    // LAN address or mDNS hostname doesn't qualify (only literal localhost/127.0.0.1
    // do), so serving the streaming UI directly from here would silently fail to offer
    // screen share. See examples/webJPEG/README.md for the tradeoffs this introduces
    // (needs internet access to reach GitHub Pages, and a one-time browser permission
    // for the WebSocket connection back to this device, since it's ws:// not wss://).
    // This same page also serves webH264 - see examples/stream.html.
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String url = "https://lemio.github.io/LilyGo-AMOLED-WebJPEG/stream.html?espAddress=http://" + request->host();
        request->redirect(url);
    });

    ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client,
                   AwsEventType type, void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT) {
            Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
        } else if (type == WS_EVT_DISCONNECT) {
            Serial.printf("WebSocket client #%u disconnected\n", client->id());
            if (wsAssemblyBuffer) {
                free(wsAssemblyBuffer);
                wsAssemblyBuffer = nullptr;
                wsAssemblySize = 0;
            }
        } else if (type == WS_EVT_DATA) {
            AwsFrameInfo *info = (AwsFrameInfo*)arg;

            // Only binary frames (JPEG images) carry a video frame
            if (info->opcode == WS_BINARY || info->opcode == WS_CONTINUATION) {

                if (info->index == 0) {
                    wsExpectedSize = info->len;
                    wsAssemblySize = 0;

                    if (wsAssemblyBuffer) {
                        free(wsAssemblyBuffer);
                        wsAssemblyBuffer = nullptr;
                    }

                    wsAssemblyBuffer = (uint8_t*)malloc(wsExpectedSize);
                    if (!wsAssemblyBuffer) {
                        Serial.printf("Malloc failed for %u bytes!\n", wsExpectedSize);
                        return;
                    }
                }

                if (wsAssemblyBuffer && (wsAssemblySize + len <= wsExpectedSize)) {
                    memcpy(wsAssemblyBuffer + wsAssemblySize, data, len);
                    wsAssemblySize += len;
                }

                if (info->final && wsAssemblySize == wsExpectedSize) {
                    if (xSemaphoreTake(frameMutex, 0) == pdTRUE) {
                        if (frameBuffer) {
                            free((void*)frameBuffer);
                        }

                        // Ownership of wsAssemblyBuffer transfers to frameBuffer
                        frameBuffer = (volatile uint8_t*)wsAssemblyBuffer;
                        frameSize = wsAssemblySize;
                        newFrameAvailable = true;
                        wsAssemblyBuffer = nullptr;
                        wsAssemblySize = 0;

                        xSemaphoreGive(frameMutex);
                    } else {
                        Serial.println("Mutex busy - frame dropped");
                        free(wsAssemblyBuffer);
                        wsAssemblyBuffer = nullptr;
                        wsAssemblySize = 0;
                    }
                }
            }
        }
    });

    server.addHandler(&ws);

    wsMono.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT) {
            Serial.printf("Mono WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
        } else if (type == WS_EVT_DISCONNECT) {
            Serial.printf("Mono WebSocket client #%u disconnected\n", client->id());
            if (wsMonoAssemblyBuffer) {
                free(wsMonoAssemblyBuffer);
                wsMonoAssemblyBuffer = nullptr;
                wsMonoAssemblySize = 0;
            }
        } else if (type == WS_EVT_DATA) {
            AwsFrameInfo *info = (AwsFrameInfo*)arg;

            if (info->opcode == WS_BINARY || info->opcode == WS_CONTINUATION) {

                if (info->index == 0) {
                    wsMonoExpectedSize = info->len;
                    wsMonoAssemblySize = 0;

                    if (wsMonoAssemblyBuffer) {
                        free(wsMonoAssemblyBuffer);
                        wsMonoAssemblyBuffer = nullptr;
                    }

                    wsMonoAssemblyBuffer = (uint8_t*)malloc(wsMonoExpectedSize);
                    if (!wsMonoAssemblyBuffer) {
                        Serial.printf("Mono malloc failed for %u bytes!\n", wsMonoExpectedSize);
                        return;
                    }
                }

                if (wsMonoAssemblyBuffer && (wsMonoAssemblySize + len <= wsMonoExpectedSize)) {
                    memcpy(wsMonoAssemblyBuffer + wsMonoAssemblySize, data, len);
                    wsMonoAssemblySize += len;
                }

                if (info->final && wsMonoAssemblySize == wsMonoExpectedSize) {
                    if (xSemaphoreTake(frameMutex, 0) == pdTRUE) {
                        if (frameBuffer) {
                            free((void*)frameBuffer);
                        }

                        frameBuffer = (volatile uint8_t*)wsMonoAssemblyBuffer;
                        frameSize = wsMonoAssemblySize;
                        newFrameAvailable = true;
                        isMonochrome = true;

                        wsMonoAssemblyBuffer = nullptr;
                        wsMonoAssemblySize = 0;

                        xSemaphoreGive(frameMutex);
                    } else {
                        Serial.println("Mono mutex busy - dropped");
                        free(wsMonoAssemblyBuffer);
                        wsMonoAssemblyBuffer = nullptr;
                        wsMonoAssemblySize = 0;
                    }
                }
            }
        }
    });

    server.addHandler(&wsMono);


    server.begin();
    Serial.println("Web server started");
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Starting webJPEG display...");

    delay(3000);

    if (!amoled.begin()) {
        while (1) {
            Serial.println("Display init failed!");
            delay(1000);
        }
    }

    spr.createSprite(WIDTH, HEIGHT);
    spr.setSwapBytes(true);

    frameMutex = xSemaphoreCreateMutex();

    spr.fillSprite(TFT_BLACK);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setTextDatum(TC_DATUM);
    spr.drawString("webJPEG", WIDTH/2, HEIGHT/2 - 20, 2);
    spr.drawString("Starting...", WIDTH/2, HEIGHT/2 + 10, 2);
    spr.setTextDatum(TL_DATUM);
    amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    delay(1000);

    setupWiFi();

    if (WiFi.status() == WL_CONNECTED) {
        setupWebServer();
    }
}

void loop()
{
    static unsigned long lastCheck = 0;

    if (newFrameAvailable) {
        uint32_t startTime = millis();

        if (xSemaphoreTake(frameMutex, pdMS_TO_TICKS(10)) == pdTRUE) {

            if (frameBuffer && frameSize > 0) {
                uint8_t* bufPtr = (uint8_t*)frameBuffer;
                size_t bufSize = frameSize;
                bool isMono = isMonochrome;

                if (isMono) {
                    drawMonoJPEG(bufPtr, bufSize);
                } else {
                    drawJPEG(bufPtr, bufSize);
                }

                frameCount++;
                lastFrameTime = millis() - startTime;

                free(bufPtr);
                frameBuffer = nullptr;
                frameSize = 0;
                isMonochrome = false;
            }

            newFrameAvailable = false;
            xSemaphoreGive(frameMutex);
        }
    }

    // Status line every 30 seconds
    unsigned long now = millis();
    if (now - lastCheck > 30000) {
        if (frameCount > 0) {
            Serial.printf("Frames: %lu | Last: %lums\n", frameCount, lastFrameTime);
        }
        lastCheck = now;
    }

    vTaskDelay(1); // yields so the idle task can run; delay() would block longer than needed
}
