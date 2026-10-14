// MOSQAI trap camera node (AI-Thinker ESP32-CAM, OV2640, PSRAM required).
//
// The camera watches the tube entrance where the fan pulls mosquitoes in. Work is
// split over the two cores so the camera never stops watching:
//   - loop() (core 1) compares every frame with the empty tube. When something
//     small enters, it captures a short burst into PSRAM and queues it as one event.
//   - uploadTask (core 0) sends queued events to mosqai-ai, which counts the
//     mosquitoes (each event once) and stores the photos that really show some.

#include "esp_camera.h"
#include "jpeg_decoder.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <new>
#include <sys/time.h>
#include <time.h>

// Copy secrets.example.h to secrets.h in this folder (it is git-ignored).
#include "secrets.h"

const String frameUrl  = String(AI_SERVER_BASE_URL) + "/trap/frame";
const String healthUrl = String(AI_SERVER_BASE_URL) + "/health";

// ─── MOTION TRIGGER ───
// Each frame is decoded at 1/4 scale (160x120 greyscale) and compared with a
// slowly-updated background. A small changed area (an insect) starts an event; a
// large one (lights, shadows) only resets the background.
const uint8_t  PIXEL_THRESHOLD      = 30;    // brightness change (0-255) that counts as "changed"
const uint32_t MIN_CHANGED_PIXELS   = 4;     // fewer than this is sensor noise
const float    MAX_CHANGED_FRACTION = 0.25;  // more than this is a lighting change, not an insect
const uint32_t LOOP_DELAY_MS        = 5;     // keep short: a mosquito is in view only briefly
const uint32_t DEBUG_PRINT_MS       = 10000; // print the changed-pixel count to help tuning

// ─── EVENT CAPTURE ───
// The burst starts with the very frame that triggered (the mosquito is in view
// now; the fan may pull it in within a second) and follows it quickly.
const uint8_t  BURST_COUNT          = 3;      // photos per event, including the trigger frame
const uint32_t BURST_INTERVAL_MS    = 150;
const uint32_t EVENT_COOLDOWN_MS    = 1500;   // motion right after an event is the same insect
// A photo of the empty tube is kept and sent with every event, so the server only
// looks at what is new (not the rim, reflections or dust that are always there).
const uint32_t EMPTY_SNAPSHOT_MS    = 20000;
const uint8_t  EVENT_QUEUE_LENGTH   = 6;      // events waiting for upload before new ones are dropped
// A photo is sent at least this often even without motion, so a silent trap is noticed.
const uint32_t HEARTBEAT_MS         = 60UL * 60UL * 1000UL;

// ─── CAMERA AND LIGHT ───
// The on-board white LED stays on so the dark tube is evenly lit and a mosquito
// passing through shows up bright. Keep it low: it gets hot at high brightness.
const uint8_t  LIGHT_BRIGHTNESS = 40;    // 0-255, 0 turns it off
#define FLASH_GPIO_NUM     4
#define FLASH_LEDC_CHANNEL 2             // channel 0 / timer 0 drive the camera clock
// Negative = shorter exposure = less motion blur on a flying mosquito (-2..2).
const int      AE_LEVEL        = -1;
// Fixed exposure removes blur the most but must be tuned to the LED: set true,
// then raise EXPOSURE_VALUE/GAIN_VALUE until a mosquito is clearly visible.
const bool     MANUAL_EXPOSURE = false;
const uint16_t EXPOSURE_VALUE  = 200;    // 0-1200
const uint8_t  GAIN_VALUE      = 10;     // 0-30

// ─── NETWORK ───
const uint32_t WIFI_TIMEOUT_MS = 20000;
const uint16_t HTTP_TIMEOUT_MS = 20000;
const uint8_t  UPLOAD_ATTEMPTS = 3;      // network and server errors are retried; the event id
const uint32_t RETRY_DELAY_MS  = 3000;   // makes a resent photo safe (it is not counted twice)
const char *   BOUNDARY        = "----MosqAiBoundary7MA4YWxk";

#define GRAY_WIDTH   160
#define GRAY_HEIGHT  120
#define GRAY_PIXELS  (GRAY_WIDTH * GRAY_HEIGHT)

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

// A JPEG copied out of the camera driver into PSRAM.
struct Photo {
  uint8_t * data = nullptr;
  size_t len = 0;
  int64_t capturedAtMs = 0;  // unix ms; 0 while the clock is not NTP-synced
};

struct CaptureEvent {
  const char * reason;       // "motion", "heartbeat" or "startup"
  char eventId[24];          // empty except for motion events
  Photo frames[BURST_COUNT];
  uint8_t frameCount;
  Photo background;          // the empty tube, for motion events
};

uint8_t background[GRAY_PIXELS];
uint8_t current[GRAY_PIXELS];
uint8_t * rgbScratch = nullptr;  // GRAY_PIXELS * 3, allocated in setup()

Photo emptyTube;                 // owned by loop(); events get their own copy
bool emptyTubeStale = false;
QueueHandle_t eventQueue = nullptr;
uint32_t bootId = 0;
uint32_t eventCounter = 0;

unsigned long lastEventMs = 0;
unsigned long lastQueuedMs = 0;
unsigned long lastEmptySnapshotMs = 0;
unsigned long lastDebugMs = 0;

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  if (!psramFound()) {
    Serial.println("[CRITICAL]: No PSRAM found; it is needed to hold photo bursts.");
    delay(10000);
    ESP.restart();
  }
  rgbScratch = (uint8_t *)ps_malloc(GRAY_PIXELS * 3);
  eventQueue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(CaptureEvent *));
  if (!rgbScratch || !eventQueue || !initCamera()) {
    Serial.println("[CRITICAL]: Startup failed, restarting in 10 s.");
    delay(10000);
    ESP.restart();
  }

  if (!connectWiFi()) {
    delay(10000);
    ESP.restart();
  }
  // Photos carry their capture time once the clock is synced (UTC).
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  pingServerHealth();
  bootId = esp_random();

  ledcAttachChannel(FLASH_GPIO_NUM, 5000, 8, FLASH_LEDC_CHANNEL);
  ledcWrite(FLASH_GPIO_NUM, LIGHT_BRIGHTNESS);

  // Let exposure settle under the light, then take the empty tube as background.
  delay(1000);
  for (int i = 0; i < 5; i++) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      continue;
    }
    decodeToGray(fb, background);
    if (i == 4) {
      copyPhoto(fb->buf, fb->len, nowUnixMs(), emptyTube);
      lastEmptySnapshotMs = millis();
      // One photo at start so the server knows the trap is up.
      CaptureEvent * ev = newEvent("startup");
      if (ev) {
        addFrame(ev, fb);
        queueEvent(ev);
      }
    }
    esp_camera_fb_return(fb);
  }

  xTaskCreatePinnedToCore(uploadTask, "upload", 12288, nullptr, 1, nullptr, 0);
  Serial.println("[READY]: Watching the tube entrance...");
}

void loop() {
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[ERROR]: Camera capture failed.");
    delay(1000);
    return;
  }

  if (!decodeToGray(fb, current)) {
    esp_camera_fb_return(fb);
    delay(LOOP_DELAY_MS);
    return;
  }

  uint32_t changed = countChangedPixels();
  unsigned long now = millis();

  if (now - lastDebugMs > DEBUG_PRINT_MS) {
    Serial.printf("[MOTION]: %u changed pixels (trigger at %u)\n", changed, MIN_CHANGED_PIXELS);
    lastDebugMs = now;
  }

  if (changed > GRAY_PIXELS * MAX_CHANGED_FRACTION) {
    // Whole scene changed: lighting, not an insect.
    memcpy(background, current, GRAY_PIXELS);
    emptyTubeStale = true;
    esp_camera_fb_return(fb);
  } else if (changed >= MIN_CHANGED_PIXELS && now - lastEventMs > EVENT_COOLDOWN_MS) {
    lastEventMs = now;
    captureMotionEvent(fb, changed);  // returns fb to the driver
  } else {
    blendIntoBackground();
    if (changed < MIN_CHANGED_PIXELS) {
      handleStillFrame(fb, now);
    }
    esp_camera_fb_return(fb);
  }

  delay(LOOP_DELAY_MS);
}

// ─── EVENTS ───
void captureMotionEvent(camera_fb_t * triggerFb, uint32_t changed) {
  Serial.printf("[MOTION]: Triggered by %u changed pixels.\n", changed);
  CaptureEvent * ev = newEvent("motion");
  if (!ev) {
    esp_camera_fb_return(triggerFb);
    return;
  }
  snprintf(ev->eventId, sizeof(ev->eventId), "%08lx-%lu",
           (unsigned long)bootId, (unsigned long)++eventCounter);

  addFrame(ev, triggerFb);
  esp_camera_fb_return(triggerFb);

  for (uint8_t i = 1; i < BURST_COUNT; i++) {
    delay(BURST_INTERVAL_MS);
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      break;
    }
    addFrame(ev, fb);
    if (i == BURST_COUNT - 1) {
      // What we just captured becomes the new normal, so an insect that lingers
      // in view does not start event after event.
      decodeToGray(fb, background);
    }
    esp_camera_fb_return(fb);
  }

  if (emptyTube.data) {
    copyPhoto(emptyTube.data, emptyTube.len, emptyTube.capturedAtMs, ev->background);
  }
  queueEvent(ev);
}

void handleStillFrame(camera_fb_t * fb, unsigned long now) {
  // Refresh the empty-tube photo while nothing is moving, well clear of any event.
  bool snapshotDue = emptyTubeStale || now - lastEmptySnapshotMs > EMPTY_SNAPSHOT_MS;
  if (snapshotDue && now - lastEventMs > 2 * EVENT_COOLDOWN_MS) {
    Photo fresh;
    if (copyPhoto(fb->buf, fb->len, nowUnixMs(), fresh)) {
      freePhoto(emptyTube);
      emptyTube = fresh;
      emptyTubeStale = false;
      lastEmptySnapshotMs = now;
    }
  }

  if (now - lastQueuedMs > HEARTBEAT_MS) {
    CaptureEvent * ev = newEvent("heartbeat");
    if (ev) {
      addFrame(ev, fb);
      queueEvent(ev);
    }
  }
}

CaptureEvent * newEvent(const char * reason) {
  CaptureEvent * ev = new (std::nothrow) CaptureEvent();
  if (!ev) {
    Serial.println("[ERROR]: Out of memory for a new event.");
    return nullptr;
  }
  ev->reason = reason;
  return ev;
}

void addFrame(CaptureEvent * ev, camera_fb_t * fb) {
  if (ev->frameCount < BURST_COUNT && copyPhoto(fb->buf, fb->len, nowUnixMs(), ev->frames[ev->frameCount])) {
    ev->frameCount++;
  }
}

void queueEvent(CaptureEvent * ev) {
  lastQueuedMs = millis();
  if (ev->frameCount == 0) {
    Serial.printf("[ERROR]: No photo could be stored for the %s event.\n", ev->reason);
    freeEvent(ev);
  } else if (xQueueSend(eventQueue, &ev, 0) != pdTRUE) {
    Serial.printf("[WARNING]: Upload queue full, %s event dropped.\n", ev->reason);
    freeEvent(ev);
  }
}

void freeEvent(CaptureEvent * ev) {
  for (uint8_t i = 0; i < ev->frameCount; i++) {
    freePhoto(ev->frames[i]);
  }
  freePhoto(ev->background);
  delete ev;
}

bool copyPhoto(const uint8_t * data, size_t len, int64_t capturedAtMs, Photo & out) {
  out.data = (uint8_t *)ps_malloc(len);
  if (!out.data) {
    out.len = 0;
    Serial.println("[ERROR]: PSRAM full, photo not stored.");
    return false;
  }
  memcpy(out.data, data, len);
  out.len = len;
  out.capturedAtMs = capturedAtMs;
  return true;
}

void freePhoto(Photo & photo) {
  free(photo.data);
  photo.data = nullptr;
  photo.len = 0;
}

int64_t nowUnixMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) {
    return 0;  // not NTP-synced yet; the server uses its arrival time instead
  }
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// ─── CAMERA ───
bool initCamera() {
  camera_config_t config = {};
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
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 10;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.fb_count = 2;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CRITICAL]: Camera initialization failed with error 0x%x\n", err);
    return false;
  }

  sensor_t * sensor = esp_camera_sensor_get();
  if (sensor) {
    // Night mode lengthens exposure, which blurs a flying insect.
    sensor->set_aec2(sensor, 0);
    if (MANUAL_EXPOSURE) {
      sensor->set_exposure_ctrl(sensor, 0);
      sensor->set_aec_value(sensor, EXPOSURE_VALUE);
      sensor->set_gain_ctrl(sensor, 0);
      sensor->set_agc_gain(sensor, GAIN_VALUE);
    } else {
      sensor->set_ae_level(sensor, AE_LEVEL);
    }
  }
  return true;
}

// ─── GREYSCALE DECODING ───
// Decodes the VGA JPEG at 1/4 scale into rgbScratch, then to greyscale.
bool decodeToGray(camera_fb_t * fb, uint8_t * out) {
  esp_jpeg_image_cfg_t cfg = {};
  cfg.indata = fb->buf;
  cfg.indata_size = fb->len;
  cfg.outbuf = rgbScratch;
  cfg.outbuf_size = GRAY_PIXELS * 3;
  cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
  cfg.out_scale = JPEG_IMAGE_SCALE_1_4;

  esp_jpeg_image_output_t info;
  if (esp_jpeg_decode(&cfg, &info) != ESP_OK || info.width != GRAY_WIDTH || info.height != GRAY_HEIGHT) {
    Serial.println("[ERROR]: Could not decode frame for motion detection.");
    return false;
  }

  for (uint32_t i = 0; i < GRAY_PIXELS; i++) {
    const uint8_t * rgb = rgbScratch + i * 3;
    out[i] = (rgb[0] * 77 + rgb[1] * 150 + rgb[2] * 29) >> 8;
  }
  return true;
}

uint32_t countChangedPixels() {
  uint32_t changed = 0;
  for (uint32_t i = 0; i < GRAY_PIXELS; i++) {
    if (abs((int)current[i] - (int)background[i]) > PIXEL_THRESHOLD) {
      changed++;
    }
  }
  return changed;
}

void blendIntoBackground() {
  // Slow running average so gradual light changes and settled insects fade in.
  for (uint32_t i = 0; i < GRAY_PIXELS; i++) {
    background[i] = (background[i] * 7 + current[i]) / 8;
  }
}

// ─── NETWORK ───
bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to local Wi-Fi Infrastructure");

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - started > WIFI_TIMEOUT_MS) {
      Serial.println("\n[ERROR]: Wi-Fi connection timed out.");
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[SUCCESS]: Wi-Fi Connection Established.");
  return true;
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  Serial.println("[WARNING]: Wi-Fi dropped, reconnecting...");
  WiFi.reconnect();
  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
    delay(500);
  }
}

void pingServerHealth() {
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(healthUrl);
  int httpCode = http.GET();
  if (httpCode == 200) {
    Serial.println("[BOOTSTRAP]: Handshake verified! AI Cluster is responsive.");
  } else {
    Serial.printf("[WARNING]: Handshake failed. Server returned HTTP code: %d\n", httpCode);
  }
  http.end();
}

// ─── UPLOAD (core 0) ───
void uploadTask(void * unused) {
  CaptureEvent * ev;
  for (;;) {
    if (xQueueReceive(eventQueue, &ev, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    for (uint8_t i = 0; i < ev->frameCount; i++) {
      uploadWithRetry(ev, i);
    }
    freeEvent(ev);
  }
}

bool uploadWithRetry(const CaptureEvent * ev, uint8_t index) {
  for (uint8_t attempt = 1; attempt <= UPLOAD_ATTEMPTS; attempt++) {
    ensureWiFi();
    int httpCode = postPhoto(ev, index);
    if (httpCode == 201) {
      return true;
    }
    // A 4xx (bad token, bad request) will not fix itself; only retry network and server errors.
    if (httpCode >= 400 && httpCode < 500) {
      return false;
    }
    if (attempt < UPLOAD_ATTEMPTS) {
      delay(RETRY_DELAY_MS * attempt);
    }
  }
  Serial.printf("[ERROR]: Gave up on %s photo %u.\n", ev->reason, index + 1);
  return false;
}

String formField(const char * name, const String & value) {
  return String("--") + BOUNDARY + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n";
}

String fileHeader(const char * name) {
  return String("--") + BOUNDARY + "\r\nContent-Disposition: form-data; name=\"" + name +
         "\"; filename=\"" + name + ".jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
}

String unixSeconds(int64_t ms) {
  char text[24];
  snprintf(text, sizeof(text), "%lu.%03u", (unsigned long)(ms / 1000), (unsigned)(ms % 1000));
  return String(text);
}

// Sends one photo of an event as multipart/form-data; returns the HTTP code (<= 0 on network errors).
int postPhoto(const CaptureEvent * ev, uint8_t index) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR]: Wi-Fi not connected.");
    return -1;
  }
  const Photo & photo = ev->frames[index];
  const Photo & empty = ev->background;

  String fields = formField("trap_id", TRAP_ID) + formField("reason", ev->reason);
  if (ev->eventId[0]) {
    fields += formField("event_id", ev->eventId);
  }
  if (photo.capturedAtMs > 0) {
    fields += formField("captured_at", unixSeconds(photo.capturedAtMs));
  }
  String frameHead = fileHeader("file");
  String backgroundHead = empty.data ? String("\r\n") + fileHeader("background") : String();
  String tail = String("\r\n--") + BOUNDARY + "--\r\n";

  size_t totalLength = fields.length() + frameHead.length() + photo.len + tail.length();
  if (empty.data) {
    totalLength += backgroundHead.length() + empty.len;
  }
  uint8_t * body = (uint8_t *)ps_malloc(totalLength);
  if (!body) {
    Serial.println("[ERROR]: Memory allocation failed for payload packaging.");
    return -1;
  }

  size_t offset = 0;
  auto append = [&](const uint8_t * data, size_t len) {
    memcpy(body + offset, data, len);
    offset += len;
  };
  append((const uint8_t *)fields.c_str(), fields.length());
  append((const uint8_t *)frameHead.c_str(), frameHead.length());
  append(photo.data, photo.len);
  if (empty.data) {
    append((const uint8_t *)backgroundHead.c_str(), backgroundHead.length());
    append(empty.data, empty.len);
  }
  append((const uint8_t *)tail.c_str(), tail.length());

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(frameUrl);
  http.addHeader("Content-Type", String("multipart/form-data; boundary=") + BOUNDARY);
  http.addHeader("X-Service-Token", AI_SERVICE_TOKEN);

  Serial.printf("Transmitting %s photo %u/%u %s...\n", ev->reason, index + 1, ev->frameCount, ev->eventId);
  int httpCode = http.POST(body, totalLength);
  free(body);

  if (httpCode > 0) {
    Serial.printf("[SERVER RESPONSE CODE]: %d\n", httpCode);
    Serial.println("Payload Return Summary: " + http.getString());
  } else {
    Serial.printf("[CRITICAL CONNECTION FAILURE]: Error code -> %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
  return httpCode;
}
