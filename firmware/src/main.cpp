#include "Arduino.h"
#include "esp_camera.h"
#include "WiFi.h"
#include "HTTPClient.h"
#include "img_converters.h"
#include "person_detect.h"

#define CAMERA_MODEL_WROVER_KIT
#include "camera_pins.h"

const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";

// Dashboard server URL — update with your PC's IP
// Find it by running "ipconfig" in PowerShell, look for IPv4 Address
#define DASHBOARD_URL "http://YOUR_SERVER_IP:3000"

bool streamActive = false;
bool tfliteReady = false;

#define LED_PIN 13
#define BUZZER_PIN 33
#define MOTION_THRESHOLD 15
#define MOTION_PERCENT 15
#define MOTION_FRAMES_REQUIRED 3
#define WARMUP_FRAMES 10
#define COOLDOWN_MS 5000
#define PERSON_THRESHOLD 200

uint8_t *prevFrame = nullptr;
size_t prevFrameLen = 0;
int motionFrameCount = 0;
int frameCount = 0;
unsigned long lastAlertTime = 0;

// Non-blocking alert state
bool alertActive = false;
unsigned long alertStartTime = 0;
int alertBlinks = 3;
int alertIntervalMs = 200;

void triggerAlert();
void updateAlert();
void startCameraServer();
void postDetectionEvent(camera_fb_t *fb, int personScore);

bool detectMotion(camera_fb_t *fb)
{
  size_t rgb_len = fb->width * fb->height * 3;
  uint8_t *rgb_buf = (uint8_t *)ps_malloc(rgb_len);
  if (!rgb_buf)
  {
    Serial.println("Failed to allocate RGB buffer");
    return false;
  }

  if (!fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb_buf))
  {
    Serial.println("JPEG decode failed");
    free(rgb_buf);
    return false;
  }

  frameCount++;
  if (frameCount < WARMUP_FRAMES)
  {
    free(prevFrame);
    prevFrame = rgb_buf;
    prevFrameLen = rgb_len;
    return false;
  }

  if (prevFrame == nullptr || prevFrameLen != rgb_len)
  {
    free(prevFrame);
    prevFrame = rgb_buf;
    prevFrameLen = rgb_len;
    return false;
  }

  int changedPixels = 0;
  int sampledPixels = 0;

  for (size_t i = 1; i < rgb_len; i += 12)
  {
    if (abs((int)rgb_buf[i] - (int)prevFrame[i]) > MOTION_THRESHOLD)
    {
      changedPixels++;
    }
    sampledPixels++;
  }

  free(prevFrame);
  prevFrame = rgb_buf;
  prevFrameLen = rgb_len;

  float pct = (changedPixels / (float)sampledPixels) * 100.0;
  Serial.printf("Motion: %.1f%%\n", pct);

  if (pct > MOTION_PERCENT)
  {
    motionFrameCount++;
  }
  else
  {
    motionFrameCount = 0;
  }

  return motionFrameCount >= MOTION_FRAMES_REQUIRED;
}

void postDetectionEvent(camera_fb_t *fb, int personScore)
{
  HTTPClient http;
  String url = String(DASHBOARD_URL) + "/api/event?score=" + String(personScore);

  http.begin(url);
  http.addHeader("Content-Type", "image/jpeg");
  http.setTimeout(5000);

  int httpCode = http.POST(fb->buf, fb->len);
  if (httpCode > 0)
  {
    Serial.printf("Dashboard POST: %d (%d bytes sent)\n", httpCode, fb->len);
  }
  else
  {
    Serial.printf("Dashboard POST failed: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

void triggerAlert()
{
  Serial.println("PERSON DETECTED - triggering alert!");
  alertActive = true;
  alertStartTime = millis();
}

void updateAlert()
{
  if (!alertActive)
    return;

  unsigned long elapsed = millis() - alertStartTime;
  unsigned long totalDuration = alertBlinks * alertIntervalMs * 2;

  if (elapsed >= totalDuration)
  {
    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    alertActive = false;
    return;
  }

  unsigned long phase = elapsed % (alertIntervalMs * 2);
  if (phase < (unsigned long)alertIntervalMs)
  {
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
  }
  else
  {
    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
  }
}

void setup()
{
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  if (psramFound())
  {
    Serial.printf("PSRAM found: %d bytes free\n", ESP.getFreePsram());
  }
  else
  {
    Serial.println("WARNING: No PSRAM detected!");
  }

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
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK)
  {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return;
  }

  Serial.println("Initializing TFLite person detection...");
  tfliteReady = setupPersonDetection();
  if (tfliteReady)
  {
    Serial.println("TFLite person detection ready!");
    Serial.printf("PSRAM remaining: %d bytes\n", ESP.getFreePsram());
  }
  else
  {
    Serial.println("WARNING: TFLite init failed.");
  }

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.printf("Dashboard target: %s\n", DASHBOARD_URL);
  startCameraServer();
  Serial.print("Camera ready at: http://");
  Serial.println(WiFi.localIP());

  // Buzzer test on boot
  Serial.println("Testing buzzer...");
  digitalWrite(BUZZER_PIN, HIGH);
  delay(500);
  digitalWrite(BUZZER_PIN, LOW);
  Serial.println("Buzzer test done");
}

void loop()
{
  updateAlert();

  if (streamActive)
  {
    delay(100);
    return;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb)
  {
    delay(500);
    return;
  }

  unsigned long now = millis();
  if (detectMotion(fb) && (now - lastAlertTime > COOLDOWN_MS))
  {
    if (tfliteReady)
    {
      Serial.println("Motion triggered — running person detection...");
      int personScore = detectPerson(fb);

      if (personScore >= PERSON_THRESHOLD)
      {
        lastAlertTime = now;
        triggerAlert();
        postDetectionEvent(fb, personScore);
      }
      else
      {
        Serial.printf("Motion but no person (score=%d, threshold=%d)\n",
                      personScore, PERSON_THRESHOLD);
      }
      motionFrameCount = 0;
    }
    else
    {
      Serial.println("Motion detected but TFLite unavailable — skipping");
    }
  }

  esp_camera_fb_return(fb);
  delay(150);
}
