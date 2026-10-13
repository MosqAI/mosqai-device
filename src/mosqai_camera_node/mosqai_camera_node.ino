#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Wi-Fi, AI server address, trap id and service token live in secrets.h, which
// git ignores. Copy secrets.example.h to secrets.h and fill it in.
#include "secrets.h"

const String serverUrl = String(AI_SERVER_URL) + "/api/v1/trap/frame";
const String healthUrl = String(AI_SERVER_URL) + "/api/v1/health";
const String trapId = TRAP_ID;

// Plain client for a local http:// server, TLS client for a hosted https:// one.
WiFiClient plainClient;
WiFiClientSecure tlsClient;

void beginRequest(HTTPClient &http, const String &url) {
  if (url.startsWith("https://")) {
    // Encrypted, but the server certificate is not checked yet (no CA bundle on the board).
    tlsClient.setInsecure();
    http.begin(tlsClient, url);
  } else {
    http.begin(plainClient, url);
  }
}

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // ─── CAMERA INITIALIZATION SECTOR ───
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size = FRAMESIZE_VGA; 
  config.jpeg_quality = 10;         
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera initialization failed with error 0x%x", err);
    return;
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to local Wi-Fi Infrastructure");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[SUCCESS]: Wi-Fi Connection Established.");
  
  pingServerHealth();
}

void loop() {
  captureAndTransmitFrame();
  
  Serial.println("Entering standard background delay window...");
  delay(3600000); 
}

void pingServerHealth() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    beginRequest(http, healthUrl);
    int httpCode = http.GET();
    if (httpCode == 200) {
      Serial.println("[BOOTSTRAP]: Handshake verified! AI Cluster is responsive.");
    } else {
      Serial.printf("[WARNING]: Handshake failed. Server returned HTTP code: %d\n", httpCode);
    }
    http.end();
  }
}

void captureAndTransmitFrame() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR]: WiFi connection dropped. Aborting transit.");
    return;
  }

  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[CRITICAL]: Camera capture frame buffer failed!");
    return;
  }

  HTTPClient http;
  beginRequest(http, serverUrl);
  // The AI service only accepts frames that carry the shared service token.
  http.addHeader("X-Service-Token", AI_SERVICE_TOKEN);
  // Hosted servers on free plans can take ~50 s to wake up.
  http.setTimeout(60000);

  String boundary = "----MosqAiBoundaryGrid777";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  String head = "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"trap_id\"\r\n\r\n";
  head += trapId + "\r\n";
  head += "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"file\"; filename=\"capture.jpg\"\r\n";
  head += "Content-Type: image/jpeg\r\n\r\n";

  String tail = "\r\n--" + boundary + "--\r\n";
  size_t totalLength = head.length() + fb->len + tail.length();
  
  uint8_t *buffer = (uint8_t *)malloc(totalLength);
  if (!buffer) {
    Serial.println("[ERROR]: Memory allocation failed for payload packaging.");
    esp_camera_fb_return(fb);
    return;
  }

  memcpy(buffer, head.c_str(), head.length());
  memcpy(buffer + head.length(), fb->buf, fb->len);
  memcpy(buffer + head.length() + fb->len, tail.c_str(), tail.length());

  Serial.println("Transmitting payload frame metrics array upstream...");
  int httpCode = http.POST(buffer, totalLength);

  if (httpCode > 0) {
    Serial.printf("[SERVER RESPONSE CODE]: %d\n", httpCode);
    String responseBody = http.getString();
    Serial.println("Payload Return Summary: " + responseBody);
  } else {
    Serial.printf("[CRITICAL CONNECTION FAILURE]: Error code -> %s\n", http.errorToString(httpCode).c_str());
  }

  free(buffer);
  esp_camera_fb_return(fb); 
  http.end();
}