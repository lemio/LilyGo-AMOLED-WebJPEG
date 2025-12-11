/**
 * @file      TFT_eSPI_Sprite.ino
 * @author    Modified for WebRTC streaming
 * @license   MIT
 * @copyright Copyright (c) 2023  Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2023-06-14
 * @note      Display WebRTC video stream from browser on AMOLED display
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
#include <JPEGDecoder.h>

// WiFi credentials
const char* ssid = "ACNPhone";
const char* password = "testtest";
const char* mdnsName = "esp";

// Web server on port 80
AsyncWebServer server(80);

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
LilyGo_Class amoled;

#define WIDTH  amoled.width()
#define HEIGHT amoled.height()

// Frame buffer for received image
uint8_t* frameBuffer = nullptr;
size_t frameSize = 0;
bool newFrameAvailable = false;
SemaphoreHandle_t frameMutex;


void drawJPEG(uint8_t *jpegData, size_t jpegSize) {
    // Decode JPEG (already scaled to display size by web interface)
    JpegDec.decodeArray(jpegData, jpegSize);
    
    if (JpegDec.width > 0 && JpegDec.height > 0) {
        Serial.printf("JPEG decoded: %dx%d\n", JpegDec.width, JpegDec.height);
        
        // Clear sprite
        spr.fillSprite(TFT_BLACK);
        
        // Render JPEG MCU blocks directly without scaling
        uint16_t *pImg;
        uint16_t mcu_w = JpegDec.MCUWidth;
        uint16_t mcu_h = JpegDec.MCUHeight;
        uint32_t max_x = JpegDec.width;
        uint32_t max_y = JpegDec.height;
        
        // Center the image if it's smaller than display
        int offsetX = (WIDTH - JpegDec.width) / 2;
        int offsetY = (HEIGHT - JpegDec.height) / 2;
        if (offsetX < 0) offsetX = 0;
        if (offsetY < 0) offsetY = 0;
        
        while (JpegDec.read()) {
            pImg = JpegDec.pImage;
            
            // Calculate position
            int mcu_x = JpegDec.MCUx * mcu_w;
            int mcu_y = JpegDec.MCUy * mcu_h;
            
            // Calculate the width and height of the MCU block to render
            uint32_t win_w = (mcu_x + mcu_w <= max_x) ? mcu_w : (max_x % mcu_w);
            uint32_t win_h = (mcu_y + mcu_h <= max_y) ? mcu_h : (max_y % mcu_h);
            
            // Draw MCU block directly to sprite
            int destX = offsetX + mcu_x;
            int destY = offsetY + mcu_y;
            
            // Only draw if within display bounds
            if (destX < WIDTH && destY < HEIGHT) {
                spr.pushImage(destX, destY, win_w, win_h, pImg);
            }
        }
        
        // Push sprite to display
        amoled.pushColors(0, 0, WIDTH, HEIGHT, (uint16_t *)spr.getPointer());
    } else {
        Serial.println("JPEG decode failed!");
    }
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

            // Convert to JPEG blob
            canvas.toBlob(async (blob) => {
                if (blob && streaming) {
                    try {
                        const response = await fetch('/upload', {
                            method: 'POST',
                            body: blob,
                            headers: { 'Content-Type': 'image/jpeg' }
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
            }, 'image/jpeg', 0.7);
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
    
    // Receive video frames - using body handler instead of upload handler
    server.on("/upload", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            // This is called after body is received
            Serial.printf("Upload complete, received body\n");
            request->send(200, "text/plain", "OK");
        },
        NULL, // No file upload handler
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            // Body handler - receives the raw POST data
            static uint8_t* tempBuffer = nullptr;
            static size_t tempSize = 0;
            
            if (index == 0) {
                // Start of new frame
                Serial.printf("Upload start, total size: %d bytes\n", total);
                if (tempBuffer) {
                    free(tempBuffer);
                }
                tempBuffer = (uint8_t*)malloc(total + 100); // Allocate exact size needed
                tempSize = 0;
                
                if (!tempBuffer) {
                    Serial.println("Failed to allocate temp buffer!");
                    return;
                }
            }
            
            // Append data
            if (tempBuffer && tempSize + len <= total) {
                memcpy(tempBuffer + tempSize, data, len);
                tempSize += len;
                Serial.printf("Body chunk: %d bytes (total: %d/%d)\n", len, tempSize, total);
            } else {
                Serial.printf("Buffer overflow! tempSize=%d, len=%d, total=%d\n", tempSize, len, total);
            }
            
            // Check if complete
            if (index + len == total && tempBuffer) {
                // Frame complete - copy to main buffer
                Serial.printf("Frame complete: %d bytes total\n", tempSize);
                
                if (xSemaphoreTake(frameMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (frameBuffer) {
                        free(frameBuffer);
                    }
                    frameBuffer = tempBuffer;
                    frameSize = tempSize;
                    tempBuffer = nullptr;
                    tempSize = 0;
                    newFrameAvailable = true;
                    xSemaphoreGive(frameMutex);
                    Serial.println("✓ Frame ready for display!");
                } else {
                    Serial.println("✗ Mutex timeout!");
                    free(tempBuffer);
                    tempBuffer = nullptr;
                    tempSize = 0;
                }
            }
        }
    );

    
    server.begin();
    Serial.println("Web server started");
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Starting WebRTC Stream Display...");

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
        Serial.println("Processing new frame...");
        xSemaphoreTake(frameMutex, portMAX_DELAY);
        
        if (frameBuffer && frameSize > 0) {
            Serial.printf("Drawing JPEG: %d bytes\n", frameSize);
            // Draw JPEG to display
            drawJPEG(frameBuffer, frameSize);
            
            // Free the buffer
            free(frameBuffer);
            frameBuffer = nullptr;
            frameSize = 0;
        } else {
            Serial.println("Frame buffer is null or empty!");
        }
        
        newFrameAvailable = false;
        xSemaphoreGive(frameMutex);
    }
    
    // Periodic status update
    if (millis() - lastCheck > 10000) {
        Serial.println("Waiting for frames... (try http://172.20.10.11/test)");
        lastCheck = millis();
    }
    
    delay(10); // Small delay to prevent watchdog issues
}
