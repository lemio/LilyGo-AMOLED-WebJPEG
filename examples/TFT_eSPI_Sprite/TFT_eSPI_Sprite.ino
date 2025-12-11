/**
 * @file      TFT_eSPI_Sprite.ino
 * @author    Modified for WebRTC streaming
 * @license   MIT
 * @copyright Copyright (c) 2023  Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2023-06-14
 * @note      Display WebRTC video stream from browser on AMOLED display
 *            Optimized for maximum performance with direct rendering
 * 
 * Required libraries:
 * - ESPAsyncWebServer: https://github.com/me-no-dev/ESPAsyncWebServer
 * - AsyncTCP: https://github.com/me-no-dev/AsyncTCP
 * - LodePNG (included with LVGL) - better for screenshots!
 */

#include <LilyGo_AMOLED.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>

// Include LodePNG implementation - this compiles the C code
#define LODEPNG_COMPILE_DISK 0
#define LODEPNG_COMPILE_ALLOCATORS 1
#define LODEPNG_COMPILE_ENCODER 0  // We only need decoder
#define LODEPNG_COMPILE_ANCILLARY_CHUNKS 0
#define LODEPNG_COMPILE_ERROR_TEXT 1
#include "../../libdeps/lvgl/src/extra/libs/png/lodepng.c"

// WiFi credentials
const char* ssid = "ACNPhone";
const char* password = "testtest";
const char* mdnsName = "esp";

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

void drawPNG(uint8_t *pngData, size_t pngSize) {
    uint32_t t1 = millis();
    
    // Decode PNG using lodepng
    unsigned char* rgba = NULL;
    unsigned width, height;
    
    unsigned error = lodepng_decode32(&rgba, &width, &height, pngData, pngSize);
    
    if (error) {
        Serial.printf("PNG decode error %u: %s\n", error, lodepng_error_text(error));
        return;
    }
    
    uint32_t t2 = millis();
    
    if (width == 0 || height == 0) {
        Serial.println("Invalid PNG dimensions!");
        free(rgba);
        return;
    }
    
    // Check if PNG matches display size exactly
    bool directRender = (width == WIDTH && height == HEIGHT);
    
    Serial.printf("PNG: %dx%d | Display: %dx%d | Direct: %s\n", 
                  width, height, WIDTH, HEIGHT, directRender ? "YES" : "NO");
    
    uint32_t t3 = millis();
    
    // Convert RGBA to RGB565
    size_t pixelCount = width * height;
    uint16_t* rgb565 = (uint16_t*)malloc(pixelCount * sizeof(uint16_t));
    if (!rgb565) {
        Serial.println("RGB565 malloc failed!");
        free(rgba);
        return;
    }
    
    // Convert RGBA8888 to RGB565 with byte swapping
    for (size_t i = 0; i < pixelCount; i++) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        // Convert to RGB565 and swap bytes for display
        uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        rgb565[i] = (color >> 8) | (color << 8);
    }
    
    free(rgba);
    
    uint32_t t4 = millis();
    
    if (!directRender) {
        // Center the image
        spr.fillSprite(TFT_BLACK);
        int16_t offsetX = (WIDTH - width) >> 1;
        int16_t offsetY = (HEIGHT - height) >> 1;
        if (offsetX < 0) offsetX = 0;
        if (offsetY < 0) offsetY = 0;
        
        spr.pushImage(offsetX, offsetY, width, height, rgb565);
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    } else {
        // Direct render - fastest!
        amoled.pushColors(0, 0, width, height, rgb565);
    }
    
    free(rgb565);
    
    uint32_t t5 = millis();
    
    Serial.printf("Timing: Decode=%lums | Convert=%lums | Render=%lums | Total=%lums\n",
                  t2-t1, t4-t3, t5-t4, t5-t1);
}

void drawMonoPNG(uint8_t *pngData, size_t pngSize) {
    uint32_t t1 = millis();
    
    // Decode PNG using lodepng
    unsigned char* rgba = NULL;
    unsigned width, height;
    
    unsigned error = lodepng_decode32(&rgba, &width, &height, pngData, pngSize);
    
    if (error) {
        Serial.printf("Mono PNG decode error %u: %s\n", error, lodepng_error_text(error));
        return;
    }
    
    uint32_t t2 = millis();
    
    if (width == 0 || height == 0) {
        Serial.println("Invalid PNG dimensions!");
        free(rgba);
        return;
    }
    
    Serial.printf("Mono PNG: %dx%d | Display: %dx%d\n", width, height, WIDTH, HEIGHT);
    
    bool directRender = (width == WIDTH && height == HEIGHT);
    
    uint32_t t3 = millis();
    
    // Convert RGBA to grayscale RGB565
    size_t pixelCount = width * height;
    uint16_t* rgb565 = (uint16_t*)malloc(pixelCount * sizeof(uint16_t));
    if (!rgb565) {
        Serial.println("RGB565 malloc failed!");
        free(rgba);
        return;
    }
    
    // Convert RGBA to grayscale RGB565
    for (size_t i = 0; i < pixelCount; i++) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        // Calculate grayscale
        uint8_t gray = (r * 30 + g * 59 + b * 11) / 100;
        // Convert to RGB565 and swap bytes
        uint16_t color = ((gray & 0xF8) << 8) | ((gray & 0xFC) << 3) | (gray >> 3);
        rgb565[i] = (color >> 8) | (color << 8);
    }
    
    free(rgba);
    
    uint32_t t4 = millis();
    
    if (!directRender) {
        spr.fillSprite(TFT_BLACK);
        int16_t offsetX = (WIDTH - width) >> 1;
        int16_t offsetY = (HEIGHT - height) >> 1;
        if (offsetX < 0) offsetX = 0;
        if (offsetY < 0) offsetY = 0;
        
        spr.pushImage(offsetX, offsetY, width, height, rgb565);
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    } else {
        amoled.pushColors(0, 0, width, height, rgb565);
    }
    
    free(rgb565);
    
    uint32_t t5 = millis();
    
    Serial.printf("Mono Timing: Decode=%lums | Convert=%lums | Render=%lums | Total=%lums\n",
                  t2-t1, t4-t3, t5-t4, t5-t1);
}

void setupWiFi() {
    Serial.println("Connecting to WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
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
        spr.setTextColor(TFT_GREEN, TFT_BLACK);
        spr.setTextSize(1);
        spr.drawString("WiFi Connected!", 10, 20, 2);
        spr.drawString(WiFi.localIP().toString(), 10, 45, 2);
        spr.drawString("http://esp.local", 10, 70, 2);
        spr.setTextColor(TFT_YELLOW, TFT_BLACK);
        spr.drawString("Waiting for stream...", 10, 100, 2);
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    } else {
        Serial.println("\nWiFi connection failed!");
        spr.fillSprite(TFT_BLACK);
        spr.setTextColor(TFT_RED, TFT_BLACK);
        spr.drawString("WiFi Failed!", 10, HEIGHT/2, 2);
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    }
}

void setupWebServer() {
    // Serve the HTML page
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Video Stream</title>
    <style>
        body { font-family: Arial; text-align: center; margin: 20px; background: #1a1a1a; color: white; }
        video { max-width: 100%; border: 2px solid #333; margin: 20px 0; }
        button { padding: 15px 30px; font-size: 18px; margin: 10px; cursor: pointer; 
                 background: #4CAF50; color: white; border: none; border-radius: 5px; }
        button:hover { background: #45a049; }
        button:disabled { background: #666; cursor: not-allowed; }
        .status { padding: 10px; margin: 10px 0; border-radius: 5px; }
        .success { background: #4CAF50; }
        .error { background: #f44336; }
        .info { background: #2196F3; }
    </style>
</head>
<body>
    <h1>ESP32 AMOLED Video Stream</h1>
    <video id="video" autoplay playsinline></video>
    <canvas id="canvas" style="display:none;"></canvas>
    <br>
    <button id="startBtn" onclick="startStreaming()">Start Streaming</button>
    <button id="stopBtn" onclick="stopStreaming()" disabled>Stop Streaming</button>
    <div id="status" class="status info">Ready to stream</div>
    <div id="stats"></div>

    <script>
        const video = document.getElementById('video');
        const canvas = document.getElementById('canvas');
        const ctx = canvas.getContext('2d');
        const statusDiv = document.getElementById('status');
        const statsDiv = document.getElementById('stats');
        const startBtn = document.getElementById('startBtn');
        const stopBtn = document.getElementById('stopBtn');
        let streaming = false;
        let frameCount = 0;
        let errorCount = 0;

        function updateStatus(message, type) {
            statusDiv.textContent = message;
            statusDiv.className = 'status ' + type;
        }

        async function startStreaming() {
            try {
                const stream = await navigator.mediaDevices.getUserMedia({ 
                    video: { width: 640, height: 480 } 
                });
                video.srcObject = stream;
                streaming = true;
                startBtn.disabled = true;
                stopBtn.disabled = false;
                updateStatus('Streaming...', 'success');
                sendFrames();
            } catch (err) {
                updateStatus('Error: ' + err.message, 'error');
                console.error(err);
            }
        }

        function stopStreaming() {
            streaming = false;
            if (video.srcObject) {
                video.srcObject.getTracks().forEach(track => track.stop());
            }
            startBtn.disabled = false;
            stopBtn.disabled = true;
            updateStatus('Stopped', 'info');
        }

        async function sendFrames() {
            if (!streaming) return;

            // Set canvas size to match video
            canvas.width = video.videoWidth || 640;
            canvas.height = video.videoHeight || 480;

            // Draw current video frame to canvas
            ctx.drawImage(video, 0, 0, canvas.width, canvas.height);

            // Convert to PNG blob (better for screenshots!)
            canvas.toBlob(async (blob) => {
                if (blob && streaming) {
                    try {
                        const response = await fetch('/upload', {
                            method: 'POST',
                            body: blob,
                            headers: { 'Content-Type': 'image/png' }
                        });
                        
                        if (response.ok) {
                            frameCount++;
                            statsDiv.textContent = `Frames sent: ${frameCount} | Errors: ${errorCount}`;
                        } else {
                            errorCount++;
                            console.error('Upload failed:', response.status);
                        }
                    } catch (err) {
                        errorCount++;
                        console.error('Upload error:', err);
                    }
                }
                
                // Send next frame (adjust delay for frame rate, ~15 FPS)
                if (streaming) {
                    setTimeout(sendFrames, 66);
                }
            }, 'image/png');
        }
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
            
            // Only handle binary frames (PNG images)
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
    Serial.println("Starting WebRTC Stream Display...");

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
    spr.drawString("WebRTC Stream", WIDTH/2 - 50, HEIGHT/2 - 20, 2);
    spr.drawString("Starting...", WIDTH/2 - 40, HEIGHT/2 + 10, 2);
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
                
                // Draw PNG to display (color or monochrome)
                if (isMono) {
                    drawMonoPNG(bufPtr, bufSize);
                } else {
                    drawPNG(bufPtr, bufSize);
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
