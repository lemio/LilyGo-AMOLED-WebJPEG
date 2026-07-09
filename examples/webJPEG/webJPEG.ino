/**
 * @file      webJPEG.ino
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

// Web server on port 80
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");        // WebSocket endpoint for color images
AsyncWebSocket wsMono("/ws-mono"); // WebSocket endpoint for monochrome images

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
LilyGo_Class amoled;

#define WIDTH  amoled.width()
#define HEIGHT amoled.height()

// Frame buffer for received image
volatile uint8_t* frameBuffer = nullptr;
volatile size_t frameSize = 0;
volatile bool newFrameAvailable = false;
volatile bool isMonochrome = false;  // Flag to indicate if frame is monochrome
SemaphoreHandle_t frameMutex;

// WebSocket frame assembly buffer (for fragmented frames)
uint8_t* wsAssemblyBuffer = nullptr;
size_t wsAssemblySize = 0;
size_t wsExpectedSize = 0;

// Monochrome WebSocket assembly buffer
uint8_t* wsMonoAssemblyBuffer = nullptr;
size_t wsMonoAssemblySize = 0;
size_t wsMonoExpectedSize = 0;

// Performance tracking
volatile uint32_t frameCount = 0;
volatile uint32_t lastFrameTime = 0;

// Helper function to swap bytes in RGB565 format
inline void swapBytes(uint16_t *data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        data[i] = (data[i] >> 8) | (data[i] << 8);
    }
}

void drawJPEG(uint8_t *jpegData, size_t jpegSize) {
    uint32_t t1 = millis();
    
    // Decode JPEG (already scaled to display size by web interface)
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
    
    // Check if JPEG matches display size exactly - if so, render directly
    bool directRender = (w == WIDTH && h == HEIGHT);
    
    Serial.printf("JPEG: %dx%d | Display: %dx%d | Direct: %s | MCU: %dx%d\n", 
                  w, h, WIDTH, HEIGHT, directRender ? "YES" : "NO", 
                  JpegDec.MCUWidth, JpegDec.MCUHeight);
    
    if (!directRender) {
        // Need to center - use sprite buffer
        spr.fillSprite(TFT_BLACK);
    }
    
    // Calculate centering offset
    int16_t offsetX = (WIDTH - w) >> 1;  // Fast divide by 2
    int16_t offsetY = (HEIGHT - h) >> 1;
    if (offsetX < 0) offsetX = 0;
    if (offsetY < 0) offsetY = 0;
    
    // Pre-fetch MCU dimensions
    uint16_t mcu_w = JpegDec.MCUWidth;
    uint16_t mcu_h = JpegDec.MCUHeight;
    
    uint32_t t3 = millis();
    
    // Render MCU blocks
    while (JpegDec.read()) {
        uint16_t *pImg = JpegDec.pImage;
        
        // Calculate MCU position in the image
        uint16_t mcu_x = JpegDec.MCUx * mcu_w;
        uint16_t mcu_y = JpegDec.MCUy * mcu_h;
        
        // Skip if MCU is completely outside image bounds
        if (mcu_x >= w || mcu_y >= h) continue;
        
        // Calculate actual valid pixels in this MCU (clipped to image bounds)
        uint16_t valid_w = (mcu_x + mcu_w <= w) ? mcu_w : (w - mcu_x);
        uint16_t valid_h = (mcu_y + mcu_h <= h) ? mcu_h : (h - mcu_y);
        
        if (valid_w == 0 || valid_h == 0) continue;
        
        // Calculate destination on display
        int16_t destX = offsetX + mcu_x;
        int16_t destY = offsetY + mcu_y;
        
        // Skip if destination is outside display bounds
        if (destX >= WIDTH || destY >= HEIGHT) continue;
        
        // Calculate actual render dimensions (clipped to display bounds)
        uint16_t render_w = valid_w;
        uint16_t render_h = valid_h;
        
        if (destX + render_w > WIDTH) {
            render_w = WIDTH - destX;
        }
        if (destY + render_h > HEIGHT) {
            render_h = HEIGHT - destY;
        }
        
        if (render_w == 0 || render_h == 0) continue;
        
        // For edge blocks, we need to extract only the valid portion from the MCU
        if (render_w != mcu_w || render_h != mcu_h) {
            // Create temporary buffer for the valid portion
            uint16_t tempBuffer[render_w * render_h];
            
            // Copy valid pixels row by row from MCU buffer
            for (uint16_t row = 0; row < render_h; row++) {
                for (uint16_t col = 0; col < render_w; col++) {
                    uint16_t pixel = pImg[row * mcu_w + col];
                    // Swap bytes ONLY when rendering directly to display
                    // Sprite handles byte swapping internally
                    tempBuffer[row * render_w + col] = directRender ? ((pixel >> 8) | (pixel << 8)) : pixel;
                }
            }
            
            if (directRender) {
                amoled.pushColors(destX, destY, render_w, render_h, tempBuffer);
            } else {
                spr.pushImage(destX, destY, render_w, render_h, tempBuffer);
            }
        } else {
            // Full MCU block
            if (directRender) {
                // Swap bytes for direct rendering to display
                swapBytes(pImg, mcu_w * mcu_h);
                amoled.pushColors(destX, destY, render_w, render_h, pImg);
            } else {
                // Sprite handles byte swapping - don't swap here
                spr.pushImage(destX, destY, render_w, render_h, pImg);
            }
        }
    }
    
    uint32_t t4 = millis();
    
    // Push sprite to display only if we used buffering
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
    
    // Render MCU blocks - grayscale to display
    while (JpegDec.read()) {
        uint16_t *pImg = JpegDec.pImage;
        
        uint16_t mcu_x = JpegDec.MCUx * mcu_w;
        uint16_t mcu_y = JpegDec.MCUy * mcu_h;
        
        uint16_t render_w = (mcu_x + mcu_w <= w) ? mcu_w : (w - mcu_x);
        uint16_t render_h = (mcu_y + mcu_h <= h) ? mcu_h : (h - mcu_y);
        
        // Bounds check - ensure we don't exceed image dimensions
        if (mcu_x >= w || mcu_y >= h) continue;
        if (render_w == 0 || render_h == 0) continue;
        
        int16_t destX = offsetX + mcu_x;
        int16_t destY = offsetY + mcu_y;
        
        if (destX >= WIDTH || destY >= HEIGHT) continue;
        
        // Clip width/height to display bounds
        if (destX + render_w > WIDTH) {
            render_w = WIDTH - destX;
        }
        if (destY + render_h > HEIGHT) {
            render_h = HEIGHT - destY;
        }
        
        // Handle partial MCU blocks at edges
        if (render_w != mcu_w || render_h != mcu_h) {
            // Edge block - extract only valid pixels to avoid padding artifacts
            uint16_t tempBuffer[render_w * render_h];
            for (uint16_t row = 0; row < render_h; row++) {
                for (uint16_t col = 0; col < render_w; col++) {
                    uint16_t pixel = pImg[row * mcu_w + col];
                    // Swap bytes ONLY when rendering directly to display
                    tempBuffer[row * render_w + col] = directRender ? ((pixel >> 8) | (pixel << 8)) : pixel;
                }
            }
            
            if (directRender) {
                amoled.pushColors(destX, destY, render_w, render_h, tempBuffer);
            } else {
                spr.pushImage(destX, destY, render_w, render_h, tempBuffer);
            }
        } else {
            // Full MCU block
            if (directRender) {
                // Swap bytes for direct rendering
                swapBytes(pImg, render_w * render_h);
                amoled.pushColors(destX, destY, render_w, render_h, pImg);
            } else {
                // Sprite handles byte swapping - don't swap here
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

        // Set up mDNS
        if (MDNS.begin(mdnsName)) {
            Serial.print("mDNS responder started: ");
            Serial.print(mdnsName);
            Serial.println(".local");
        } else {
            Serial.println("Error setting up mDNS");
        }

        // Display WiFi info on screen
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
    // Serve board info as JSON
    server.on("/boardinfo", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "{";
        json += "\"name\":\"" + String(amoled.getName()) + "\",";
        json += "\"width\":" + String(WIDTH) + ",";
        json += "\"height\":" + String(HEIGHT) + ",";
        json += "\"boardId\":" + String(amoled.getBoardID());
        json += "}";
        request->send(200, "application/json", json);
    });
    
    // Serve the same page as webrtc_stream.html, inlined so visiting the device's own
    // address works out of the box with no separate file to open. Keep these two in
    // sync by hand - there's no build step that generates one from the other.
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Video Stream Client</title>
    <style>
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            text-align: center;
            margin: 0;
            padding: 20px;
            background: linear-gradient(135deg, #1a1a1a 0%, #2d2d2d 100%);
            color: white;
            min-height: 100vh;
        }
        .container {
            max-width: 800px;
            margin: 0 auto;
            background: rgba(0, 0, 0, 0.5);
            padding: 30px;
            border-radius: 15px;
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
        }
        h1 {
            color: #4CAF50;
            margin-bottom: 10px;
        }
        video {
            max-width: 100%;
            width: 640px;
            border: 3px solid #333;
            border-radius: 10px;
            margin: 20px 0;
            background: #000;
        }
        button {
            padding: 15px 40px;
            font-size: 18px;
            margin: 10px;
            cursor: pointer;
            background: #4CAF50;
            color: white;
            border: none;
            border-radius: 8px;
            transition: all 0.3s;
            font-weight: bold;
            box-shadow: 0 4px 12px rgba(76, 175, 80, 0.3);
        }
        button:hover {
            background: #45a049;
            transform: translateY(-2px);
            box-shadow: 0 6px 16px rgba(76, 175, 80, 0.4);
        }
        button:active {
            transform: translateY(0);
        }
        button:disabled {
            background: #666;
            cursor: not-allowed;
            box-shadow: none;
            transform: none;
        }
        .status {
            padding: 15px;
            margin: 15px 0;
            border-radius: 8px;
            font-weight: bold;
            font-size: 16px;
        }
        .success { background: #4CAF50; }
        .error { background: #f44336; }
        .info { background: #2196F3; }
        .warning { background: #ff9800; }
        .stats {
            background: rgba(33, 33, 33, 0.8);
            padding: 15px;
            border-radius: 8px;
            margin: 15px 0;
            font-family: 'Courier New', monospace;
        }
        .stats div {
            margin: 5px 0;
        }
        .config {
            background: rgba(33, 33, 33, 0.8);
            padding: 15px;
            border-radius: 8px;
            margin: 15px 0;
            text-align: left;
        }
        .config label {
            display: inline-block;
            width: 150px;
            margin: 10px 0;
        }
        .config input {
            width: 200px;
            padding: 8px;
            border-radius: 5px;
            border: 1px solid #555;
            background: #222;
            color: white;
        }
        .quality-slider {
            width: 200px;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🎥 ESP32 AMOLED Video Stream</h1>
        <p>Stream your webcam to ESP32 display via WiFi</p>
        
        <div class="config">
            <h3>⚙️ Configuration</h3>
            <div>
                <label>ESP32 Address:</label>
                <input type="text" id="espAddress" value="http://esp-webjpeg.local" placeholder="http://esp-webjpeg.local">
                <button onclick="detectBoardConfig()" style="padding: 8px 15px; font-size: 14px; margin-left: 10px;">🔍 Detect Board</button>
            </div>
            <div>
                <label>Display Size:</label>
                <select id="displaySize" style="width: 200px; padding: 8px; border-radius: 5px; border: 1px solid #555; background: #222; color: white;">
                    <option value="536x240" selected>536x240 (1.91" AMOLED)</option>
                    <option value="600x450">600x450 (2.41" AMOLED)</option>
                    <option value="194x368">194x368 (1.47" AMOLED)</option>
                    <option value="custom">Custom...</option>
                </select>
            </div>
            <div id="customSizeDiv" style="display: none;">
                <label>Custom Size:</label>
                <input type="number" id="customWidth" placeholder="Width" style="width: 90px; padding: 8px; border-radius: 5px; border: 1px solid #555; background: #222; color: white;">
                x
                <input type="number" id="customHeight" placeholder="Height" style="width: 90px; padding: 8px; border-radius: 5px; border: 1px solid #555; background: #222; color: white;">
            </div>
            <div>
                <label>Source:</label>
                <select id="sourceType" style="width: 200px; padding: 8px; border-radius: 5px; border: 1px solid #555; background: #222; color: white;">
                    <option value="camera">📹 Webcam</option>
                    <option value="screen">🖥️ Screen Share</option>
                </select>
            </div>
            <div>
                <label>Frame Rate (FPS):</label>
                <input type="number" id="frameRate" value="6" min="1" max="30">
            </div>
            <div>
                <label>JPEG Quality:</label>
                <input type="range" id="quality" class="quality-slider" value="0.7" min="0.1" max="1.0" step="0.1">
                <span id="qualityValue">0.7</span>
            </div>
            <div>
                <label>
                    <input type="checkbox" id="monoMode" style="margin-right: 8px;">
                    🎨 Monochrome Mode (faster, smaller files)
                </label>
            </div>
        </div>

        <video id="video" autoplay playsinline></video>
        <canvas id="canvas" style="display:none;"></canvas>
        
        <div>
            <button id="startBtn" onclick="startStreaming()">▶️ Start Streaming</button>
            <button id="stopBtn" onclick="stopStreaming()" disabled>⏹️ Stop Streaming</button>
        </div>
        
        <div id="status" class="status info">📡 Ready to stream</div>
        
        <div class="stats">
            <h3>📊 Statistics</h3>
            <div id="stats">
                <div>Frames Sent: <span id="frameCount">0</span></div>
                <div>Errors: <span id="errorCount">0</span></div>
                <div>Data Sent: <span id="dataSize">0 KB</span></div>
                <div>Actual FPS: <span id="actualFPS">0</span></div>
                <div>Avg Frame Size: <span id="avgFrameSize">0 KB</span></div>
            </div>
        </div>

        <div style="margin-top: 20px; font-size: 12px; color: #888;">
            <p>💡 Tips:</p>
            <ul style="text-align: left; display: inline-block;">
                <li>Make sure your ESP32 is connected to WiFi</li>
                <li>ESP32 should be accessible at <code>esp-webjpeg.local</code> unless you set a different mDNS hostname when flashing</li>
                <li>Select the correct display size for your AMOLED screen</li>
                <li>Video is automatically scaled to match display dimensions</li>
                <li>Choose between webcam or screen capture</li>
                <li>Screen share works in Chrome, Edge, and Firefox</li>
                <li>Lower frame rate or quality if stream is laggy</li>
                <li>Grant camera/screen permissions when prompted</li>
            </ul>
        </div>
    </div>

    <script>
        const video = document.getElementById('video');
        const canvas = document.getElementById('canvas');
        const ctx = canvas.getContext('2d');
        const statusDiv = document.getElementById('status');
        const startBtn = document.getElementById('startBtn');
        const stopBtn = document.getElementById('stopBtn');
        const espAddressInput = document.getElementById('espAddress');
        const frameRateInput = document.getElementById('frameRate');
        const qualitySlider = document.getElementById('quality');
        const qualityValue = document.getElementById('qualityValue');
        const sourceTypeSelect = document.getElementById('sourceType');
        const displaySizeSelect = document.getElementById('displaySize');
        const customSizeDiv = document.getElementById('customSizeDiv');
        const customWidthInput = document.getElementById('customWidth');
        const customHeightInput = document.getElementById('customHeight');
        const monoModeCheckbox = document.getElementById('monoMode');
        
        let streaming = false;
        let frameCount = 0;
        let errorCount = 0;
        let totalDataSize = 0;
        let lastFrameTime = 0;
        let fpsHistory = [];
        let frameSizeHistory = [];
        
        // WebSocket connection
        let ws = null;
        let wsConnected = false;
        let reconnectAttempts = 0;
        let maxReconnectAttempts = 10;
        let reconnectDelay = 2000; // Start with 2 seconds
        let reconnectTimer = null;

        // Update quality value display
        qualitySlider.addEventListener('input', (e) => {
            qualityValue.textContent = e.target.value;
        });

        // Handle display size selection
        displaySizeSelect.addEventListener('change', (e) => {
            if (e.target.value === 'custom') {
                customSizeDiv.style.display = 'block';
            } else {
                customSizeDiv.style.display = 'none';
            }
        });

        // Function to get current display dimensions
        function getDisplayDimensions() {
            const sizeValue = displaySizeSelect.value;
            if (sizeValue === 'custom') {
                const w = parseInt(customWidthInput.value) || 536;
                const h = parseInt(customHeightInput.value) || 240;
                return { width: w, height: h };
            } else {
                const [width, height] = sizeValue.split('x').map(Number);
                return { width, height };
            }
        }

        function updateStatus(message, type) {
            statusDiv.textContent = message;
            statusDiv.className = 'status ' + type;
        }

        function updateStats(frameSize) {
            document.getElementById('frameCount').textContent = frameCount;
            document.getElementById('errorCount').textContent = errorCount;
            document.getElementById('dataSize').textContent = (totalDataSize / 1024).toFixed(2);
            
            // Calculate FPS
            const now = Date.now();
            if (lastFrameTime > 0) {
                const fps = 1000 / (now - lastFrameTime);
                fpsHistory.push(fps);
                if (fpsHistory.length > 10) fpsHistory.shift();
                const avgFps = fpsHistory.reduce((a, b) => a + b, 0) / fpsHistory.length;
                document.getElementById('actualFPS').textContent = avgFps.toFixed(1);
            }
            lastFrameTime = now;

            // Track frame sizes
            if (frameSize) {
                frameSizeHistory.push(frameSize);
                if (frameSizeHistory.length > 10) frameSizeHistory.shift();
                const avgSize = frameSizeHistory.reduce((a, b) => a + b, 0) / frameSizeHistory.length;
                document.getElementById('avgFrameSize').textContent = (avgSize / 1024).toFixed(2);
            }
        }

        function connectWebSocket() {
            return new Promise((resolve, reject) => {
                const espAddress = espAddressInput.value.trim();
                const isMono = monoModeCheckbox.checked;
                const wsEndpoint = isMono ? '/ws-mono' : '/ws';
                const wsUrl = espAddress.replace('http://', 'ws://').replace('https://', 'wss://') + wsEndpoint;
                
                updateStatus(`🔌 Connecting to ${isMono ? 'monochrome' : 'color'} WebSocket...`, 'info');
                
                ws = new WebSocket(wsUrl);
                ws.binaryType = 'arraybuffer';
                
                ws.onopen = () => {
                    wsConnected = true;
                    reconnectAttempts = 0; // Reset reconnect counter on successful connection
                    reconnectDelay = 2000; // Reset delay
                    console.log('WebSocket connected!');
                    resolve();
                };
                
                ws.onerror = (error) => {
                    console.error('WebSocket error:', error);
                    wsConnected = false;
                    reject(error);
                };
                
                ws.onclose = () => {
                    console.log('WebSocket closed');
                    const wasConnected = wsConnected;
                    wsConnected = false;
                    
                    if (streaming && wasConnected) {
                        // Connection was lost while streaming - attempt to reconnect
                        updateStatus('⚠️ Connection lost - attempting to reconnect...', 'warning');
                        attemptReconnect();
                    }
                };
            });
        }

        function attemptReconnect() {
            if (!streaming) {
                // User stopped streaming, don't reconnect
                return;
            }
            
            if (reconnectAttempts >= maxReconnectAttempts) {
                updateStatus('❌ Max reconnection attempts reached', 'error');
                stopStreaming();
                return;
            }
            
            reconnectAttempts++;
            updateStatus(`🔄 Reconnecting (attempt ${reconnectAttempts}/${maxReconnectAttempts})...`, 'warning');
            
            // Clear any existing timer
            if (reconnectTimer) {
                clearTimeout(reconnectTimer);
            }
            
            // Exponential backoff: double the delay each time, max 30 seconds
            const currentDelay = Math.min(reconnectDelay * Math.pow(1.5, reconnectAttempts - 1), 30000);
            
            reconnectTimer = setTimeout(async () => {
                try {
                    await connectWebSocket();
                    updateStatus('✅ Reconnected successfully!', 'success');
                } catch (err) {
                    console.error('Reconnection failed:', err);
                    // Will trigger onclose which will call attemptReconnect again
                }
            }, currentDelay);
        }

        async function startStreaming() {
            try {
                // Connect WebSocket first
                await connectWebSocket();
                
                // Now request media stream
                const sourceType = sourceTypeSelect.value;
                let stream;
                
                if (sourceType === 'screen') {
                    // Request screen capture
                    updateStatus('🖥️ Requesting screen share...', 'info');
                    stream = await navigator.mediaDevices.getDisplayMedia({ 
                        video: { 
                            width: { ideal: 1920 },
                            height: { ideal: 1080 },
                            frameRate: { ideal: 30 }
                        },
                        audio: false
                    });
                } else {
                    // Request camera access
                    updateStatus('🎥 Requesting camera access...', 'info');
                    stream = await navigator.mediaDevices.getUserMedia({ 
                        video: { 
                            width: { ideal: 640 },
                            height: { ideal: 480 },
                            facingMode: 'user'
                        } 
                    });
                }
                
                video.srcObject = stream;
                streaming = true;
                startBtn.disabled = true;
                stopBtn.disabled = false;
                frameCount = 0;
                errorCount = 0;
                totalDataSize = 0;
                fpsHistory = [];
                frameSizeHistory = [];
                
                updateStatus('✅ Streaming active', 'success');
                
                // Wait for video to be ready
                video.onloadedmetadata = () => {
                    sendFrames();
                };
                
                // Handle stream ending (user stops screen share)
                stream.getVideoTracks()[0].addEventListener('ended', () => {
                    console.log('Stream ended by user');
                    stopStreaming();
                });
                
            } catch (err) {
                console.error('Error starting stream:', err);
                
                if (err.name === 'NotAllowedError') {
                    updateStatus('❌ Permission denied', 'error');
                } else if (err.name === 'NotFoundError') {
                    updateStatus('❌ Source not found', 'error');
                } else if (err.name === 'NotSupportedError') {
                    updateStatus('❌ Screen capture not supported', 'error');
                } else {
                    updateStatus('❌ Error: ' + err.message, 'error');
                }
                
                // Clean up on error
                if (ws && wsConnected) {
                    ws.close();
                    ws = null;
                }
                streaming = false;
            }
        }

        function stopStreaming() {
            streaming = false;
            
            // Clear reconnection timer
            if (reconnectTimer) {
                clearTimeout(reconnectTimer);
                reconnectTimer = null;
            }
            reconnectAttempts = 0;
            
            // Stop media stream
            if (video.srcObject) {
                video.srcObject.getTracks().forEach(track => track.stop());
                video.srcObject = null;
            }
            
            // Close WebSocket
            if (ws && wsConnected) {
                ws.close();
                ws = null;
                wsConnected = false;
            }
            
            startBtn.disabled = false;
            stopBtn.disabled = true;
            updateStatus('⏹️ Streaming stopped', 'info');
        }

        async function sendFrames() {
            if (!streaming || !wsConnected || !ws) return;

            // Get display dimensions from settings
            const displayDims = getDisplayDimensions();
            const isMono = monoModeCheckbox.checked;
            
            // Set canvas size to match ESP32 display
            if (video.videoWidth && video.videoHeight) {
                canvas.width = displayDims.width;
                canvas.height = displayDims.height;

                // Draw current video frame to canvas (scaled to display size)
                ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
                
                // Convert to grayscale if monochrome mode
                if (isMono) {
                    const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);
                    const data = imageData.data;
                    
                    // Convert to grayscale using luminance formula
                    for (let i = 0; i < data.length; i += 4) {
                        const gray = 0.299 * data[i] + 0.587 * data[i + 1] + 0.114 * data[i + 2];
                        data[i] = gray;     // R
                        data[i + 1] = gray; // G
                        data[i + 2] = gray; // B
                        // Alpha stays the same
                    }
                    
                    ctx.putImageData(imageData, 0, 0);
                }

                // Convert to JPEG blob
                const quality = parseFloat(qualitySlider.value);
                
                console.log(`Sending frame: ${canvas.width}x${canvas.height}, quality: ${quality}`);
                
                canvas.toBlob(async (blob) => {
                    if (blob && streaming && wsConnected) {
                        try {
                            // Send via WebSocket - MUCH faster than HTTP POST!
                            const arrayBuffer = await blob.arrayBuffer();
                            
                            if (ws.readyState === WebSocket.OPEN) {
                                ws.send(arrayBuffer);
                                
                                console.log(`Sent ${arrayBuffer.byteLength} bytes via WebSocket`);
                                
                                frameCount++;
                                totalDataSize += arrayBuffer.byteLength;
                                updateStats(arrayBuffer.byteLength);
                            }
                            
                        } catch (err) {
                            errorCount++;
                            updateStats();
                            console.error('WebSocket send error:', err);
                            updateStatus('⚠️ Connection issue', 'warning');
                        }
                    }
                    
                    // Schedule next frame based on desired frame rate
                    if (streaming) {
                        const fps = parseInt(frameRateInput.value) || 6;
                        const delay = 1000 / fps;
                        setTimeout(sendFrames, delay);
                    }
                }, 'image/jpeg', quality);
            } else {
                // Video not ready, try again
                if (streaming) {
                    setTimeout(sendFrames, 100);
                }
            }
        }

        // Parse URL query parameters and populate form fields
        function loadQueryParameters() {
            const urlParams = new URLSearchParams(window.location.search);

            // ESP32 Address: if this page was loaded directly from the ESP32 (not
            // opened as a local file), default to that same origin - always correct
            // regardless of which mDNS hostname was configured at flash time.
            if (urlParams.has('espAddress')) {
                espAddressInput.value = urlParams.get('espAddress');
            } else if (window.location.origin.startsWith('http')) {
                espAddressInput.value = window.location.origin;
            }

            // Display Size
            if (urlParams.has('displaySize')) {
                const size = urlParams.get('displaySize');
                // Check if it's a predefined size or custom
                const validSizes = ['536x240', '600x450', '194x368'];
                if (validSizes.includes(size)) {
                    displaySizeSelect.value = size;
                } else if (size === 'custom' || size.includes('x')) {
                    displaySizeSelect.value = 'custom';
                    customSizeDiv.style.display = 'block';
                    
                    // Parse custom dimensions if provided in WxH format
                    if (size !== 'custom' && size.includes('x')) {
                        const [w, h] = size.split('x').map(Number);
                        if (!isNaN(w) && !isNaN(h)) {
                            customWidthInput.value = w;
                            customHeightInput.value = h;
                        }
                    }
                }
            }
            
            // Custom Width
            if (urlParams.has('customWidth')) {
                const w = parseInt(urlParams.get('customWidth'));
                if (!isNaN(w)) {
                    customWidthInput.value = w;
                    if (displaySizeSelect.value !== 'custom') {
                        displaySizeSelect.value = 'custom';
                        customSizeDiv.style.display = 'block';
                    }
                }
            }
            
            // Custom Height
            if (urlParams.has('customHeight')) {
                const h = parseInt(urlParams.get('customHeight'));
                if (!isNaN(h)) {
                    customHeightInput.value = h;
                    if (displaySizeSelect.value !== 'custom') {
                        displaySizeSelect.value = 'custom';
                        customSizeDiv.style.display = 'block';
                    }
                }
            }
            
            // Source Type
            if (urlParams.has('sourceType')) {
                const source = urlParams.get('sourceType');
                if (source === 'camera' || source === 'screen') {
                    sourceTypeSelect.value = source;
                }
            }
            
            // Frame Rate
            if (urlParams.has('frameRate')) {
                const fps = parseInt(urlParams.get('frameRate'));
                if (!isNaN(fps) && fps >= 1 && fps <= 30) {
                    frameRateInput.value = fps;
                }
            }
            
            // JPEG Quality
            if (urlParams.has('quality')) {
                const q = parseFloat(urlParams.get('quality'));
                if (!isNaN(q) && q >= 0.1 && q <= 1.0) {
                    qualitySlider.value = q;
                    qualityValue.textContent = q;
                }
            }
            
            // Monochrome Mode
            if (urlParams.has('monoMode')) {
                const mono = urlParams.get('monoMode');
                monoModeCheckbox.checked = (mono === 'true' || mono === '1' || mono === 'yes');
            }
        }

        // Auto-detect board configuration from ESP32
        async function detectBoardConfig() {
            try {
                const espAddress = espAddressInput.value.trim();
                const response = await fetch(espAddress + '/boardinfo');
                if (response.ok) {
                    const boardInfo = await response.json();
                    console.log('Detected board:', boardInfo);
                    
                    // Auto-set display size based on detected board
                    const detectedSize = `${boardInfo.width}x${boardInfo.height}`;
                    
                    // Check if it's a predefined size
                    const options = Array.from(displaySizeSelect.options).map(opt => opt.value);
                    if (options.includes(detectedSize)) {
                        displaySizeSelect.value = detectedSize;
                        customSizeDiv.style.display = 'none';
                    } else {
                        // Use custom size
                        displaySizeSelect.value = 'custom';
                        customWidthInput.value = boardInfo.width;
                        customHeightInput.value = boardInfo.height;
                        customSizeDiv.style.display = 'block';
                    }
                    
                    updateStatus(`✅ Detected: ${boardInfo.name} (${boardInfo.width}x${boardInfo.height})`, 'success');
                    return true;
                }
            } catch (err) {
                console.log('Board auto-detection failed:', err);
                updateStatus('⚠️ Could not auto-detect board. Using manual settings.', 'warning');
            }
            return false;
        }

        // Auto-detect ESP32 if on local network
        window.addEventListener('load', async () => {
            loadQueryParameters();
            
            // Try to auto-detect board configuration
            await detectBoardConfig();
            
            console.log('Ready to stream to ESP32');
        });
    </script>
</body>
</html>
)rawliteral";
        request->send(200, "text/html", html);
    });
    
    // Test endpoint
    server.on("/test", HTTP_GET, [](AsyncWebServerRequest *request) {
        Serial.println("Test endpoint hit!");
        request->send(200, "text/plain", "ESP32 server is working!");
    });
    
    // WebSocket event handler - MUCH faster than HTTP POST!
    ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, 
                   AwsEventType type, void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT) {
            Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
        } else if (type == WS_EVT_DISCONNECT) {
            Serial.printf("WebSocket client #%u disconnected\n", client->id());
            // Clean up assembly buffer
            if (wsAssemblyBuffer) {
                free(wsAssemblyBuffer);
                wsAssemblyBuffer = nullptr;
                wsAssemblySize = 0;
            }
        } else if (type == WS_EVT_DATA) {
            AwsFrameInfo *info = (AwsFrameInfo*)arg;
            
            Serial.printf("WS Data: final=%d, index=%u, len=%u, total=%u, opcode=%d\n",
                         info->final, info->index, len, info->len, info->opcode);
            
            // Only handle binary frames (JPEG images)
            if (info->opcode == WS_BINARY || info->opcode == WS_CONTINUATION) {
                
                // First chunk of new frame
                if (info->index == 0) {
                    wsExpectedSize = info->len;
                    wsAssemblySize = 0;
                    
                    // Free old buffer
                    if (wsAssemblyBuffer) {
                        free(wsAssemblyBuffer);
                        wsAssemblyBuffer = nullptr;
                    }
                    
                    // Allocate buffer for complete frame
                    wsAssemblyBuffer = (uint8_t*)malloc(wsExpectedSize);
                    if (!wsAssemblyBuffer) {
                        Serial.printf("Malloc failed for %u bytes!\n", wsExpectedSize);
                        return;
                    }
                    Serial.printf("Allocated %u bytes for frame assembly\n", wsExpectedSize);
                }
                
                // Copy chunk to assembly buffer
                if (wsAssemblyBuffer && (wsAssemblySize + len <= wsExpectedSize)) {
                    memcpy(wsAssemblyBuffer + wsAssemblySize, data, len);
                    wsAssemblySize += len;
                    Serial.printf("Copied %u bytes, total: %u/%u\n", len, wsAssemblySize, wsExpectedSize);
                }
                
                // Check if frame is complete
                if (info->final && wsAssemblySize == wsExpectedSize) {
                    Serial.printf("Complete frame assembled: %u bytes\n", wsAssemblySize);
                    
                    // Try to transfer to display buffer
                    if (xSemaphoreTake(frameMutex, 0) == pdTRUE) {
                        // Free old display buffer
                        if (frameBuffer) {
                            free((void*)frameBuffer);
                        }
                        
                        // Transfer ownership
                        frameBuffer = (volatile uint8_t*)wsAssemblyBuffer;
                        frameSize = wsAssemblySize;
                        newFrameAvailable = true;
                        
                        // Clear assembly buffer pointer (ownership transferred)
                        wsAssemblyBuffer = nullptr;
                        wsAssemblySize = 0;
                        
                        Serial.println("Frame buffered successfully!");
                        xSemaphoreGive(frameMutex);
                    } else {
                        Serial.println("Mutex busy - frame dropped");
                        free(wsAssemblyBuffer);
                        wsAssemblyBuffer = nullptr;
                        wsAssemblySize = 0;
                    }
                }
            } else {
                Serial.printf("Ignoring non-binary frame (opcode=%d)\n", info->opcode);
            }
        }
    });
    
    // Add WebSocket to server
    server.addHandler(&ws);
    
    // Monochrome WebSocket handler - for faster grayscale streaming
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
            
            Serial.printf("Mono WS Data: final=%d, index=%u, len=%u, total=%u\n",
                         info->final, info->index, len, info->len);
            
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
                    Serial.printf("Mono allocated %u bytes\n", wsMonoExpectedSize);
                }
                
                if (wsMonoAssemblyBuffer && (wsMonoAssemblySize + len <= wsMonoExpectedSize)) {
                    memcpy(wsMonoAssemblyBuffer + wsMonoAssemblySize, data, len);
                    wsMonoAssemblySize += len;
                }
                
                if (info->final && wsMonoAssemblySize == wsMonoExpectedSize) {
                    Serial.printf("Mono frame complete: %u bytes\n", wsMonoAssemblySize);
                    
                    if (xSemaphoreTake(frameMutex, 0) == pdTRUE) {
                        if (frameBuffer) {
                            free((void*)frameBuffer);
                        }
                        
                        frameBuffer = (volatile uint8_t*)wsMonoAssemblyBuffer;
                        frameSize = wsMonoAssemblySize;
                        newFrameAvailable = true;
                        isMonochrome = true;  // Mark as monochrome
                        
                        wsMonoAssemblyBuffer = nullptr;
                        wsMonoAssemblySize = 0;
                        
                        Serial.println("Mono frame buffered!");
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
    
    // Initialize display
    if (!amoled.begin()) {
        while (1) {
            Serial.println("Display init failed!"); 
            delay(1000);
        }
    }
    
    // Create sprite
    spr.createSprite(WIDTH, HEIGHT);
    spr.setSwapBytes(true);
    
    // Create mutex for frame buffer
    frameMutex = xSemaphoreCreateMutex();
    
    // Show startup screen
    spr.fillSprite(TFT_BLACK);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setTextDatum(TC_DATUM);
    spr.drawString("webJPEG", WIDTH/2, HEIGHT/2 - 20, 2);
    spr.drawString("Starting...", WIDTH/2, HEIGHT/2 + 10, 2);
    spr.setTextDatum(TL_DATUM);
    amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    delay(1000);
    
    // Connect to WiFi
    setupWiFi();
    
    // Start web server
    if (WiFi.status() == WL_CONNECTED) {
        setupWebServer();
    }
}

void loop()
{
    static unsigned long lastCheck = 0;
    
    // Check for new frames
    if (newFrameAvailable) {
        uint32_t startTime = millis();
        
        // Take mutex with minimal blocking
        if (xSemaphoreTake(frameMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            
            if (frameBuffer && frameSize > 0) {
                // Cast away volatile for function call
                uint8_t* bufPtr = (uint8_t*)frameBuffer;
                size_t bufSize = frameSize;
                bool isMono = isMonochrome;
                
                // Draw JPEG to display (color or monochrome)
                if (isMono) {
                    drawMonoJPEG(bufPtr, bufSize);
                } else {
                    drawJPEG(bufPtr, bufSize);
                }
                
                // Track performance
                frameCount++;
                lastFrameTime = millis() - startTime;
                
                // Free the buffer immediately
                free(bufPtr);
                frameBuffer = nullptr;
                frameSize = 0;
                isMonochrome = false;
            }
            
            newFrameAvailable = false;
            xSemaphoreGive(frameMutex);
        }
    }
    
    // Periodic status update with FPS info
    unsigned long now = millis();
    if (now - lastCheck > 30000) {  // Every 30 seconds
        if (frameCount > 0) {
            Serial.printf("Frames: %lu | Last: %lums\n", frameCount, lastFrameTime);
        }
        lastCheck = now;
    }
    
    // Minimal delay - let other tasks run
    vTaskDelay(1);  // More efficient than delay()
}
