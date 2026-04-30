#include "esp_http_server.h"
#include "esp_camera.h"
#include "Arduino.h"
#include "person_detect.h"

#define COOLDOWN_MS 5000
#define PERSON_THRESHOLD 200

extern bool detectMotion(camera_fb_t *fb);
extern void triggerAlert();
extern void updateAlert();
extern void postDetectionEvent(camera_fb_t *fb, int personScore);
extern unsigned long lastAlertTime;
extern bool tfliteReady;
extern int motionFrameCount;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;

static esp_err_t stream_handler(httpd_req_t *req)
{
    extern bool streamActive;
    streamActive = true;

    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[64];
    int streamFrameCount = 0;

    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK)
    {
        streamActive = false;
        return res;
    }

    while (true)
    {
        fb = esp_camera_fb_get();
        if (!fb)
        {
            Serial.println("Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        // Only run motion detection every 5th frame to keep stream smooth
        streamFrameCount++;
        if (streamFrameCount % 5 == 0)
        {
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
                        Serial.printf("Motion but no person (score=%d)\n", personScore);
                    }
                    motionFrameCount = 0;
                }
                else
                {
                    Serial.println("Motion detected but TFLite unavailable — skipping");
                }
            }
        }

        updateAlert();

        // Send JPEG frame to browser
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY,
                                    strlen(STREAM_BOUNDARY));
        if (res == ESP_OK)
        {
            size_t hlen = snprintf(part_buf, 64, STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req,
                                        (const char *)fb->buf, fb->len);
        }

        esp_camera_fb_return(fb);
        fb = NULL;

        if (res != ESP_OK)
            break;

        delay(30);
    }

    streamActive = false;
    return res;
}

void startCameraServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL};

    if (httpd_start(&stream_httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        Serial.println("Stream ready at /stream");
    }
}
