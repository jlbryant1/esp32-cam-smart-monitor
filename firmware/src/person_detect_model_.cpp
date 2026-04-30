#include "person_detect.h"
#include "Arduino.h"
#include "img_converters.h"

// TensorFlow Lite includes
#include <TensorFlowLite_ESP32.h>
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Model data — copy these files from:
// .pio/libdeps/esp-wrover-kit/TensorFlowLite_ESP32/examples/person_detection/
#include "person_detect_model_data.h"

// Model expects 96x96 grayscale input
static const int kInputWidth = 96;
static const int kInputHeight = 96;
static const int kInputSize = kInputWidth * kInputHeight;

// Tensor arena — allocated in PSRAM
static const int kTensorArenaSize = 136 * 1024; // 136KB
static uint8_t *tensor_arena = nullptr;

// TFLite objects
static tflite::MicroErrorReporter micro_error_reporter;
static tflite::AllOpsResolver resolver;
static const tflite::Model *model = nullptr;
static tflite::MicroInterpreter *interpreter = nullptr;
static TfLiteTensor *input = nullptr;
static TfLiteTensor *output = nullptr;

// Nearest-neighbor resize from source grayscale to 96x96 int8
static void resizeGrayscaleToInput(const uint8_t *src, int srcW, int srcH,
                                   int8_t *dst)
{
    for (int y = 0; y < kInputHeight; y++)
    {
        int srcY = y * srcH / kInputHeight;
        for (int x = 0; x < kInputWidth; x++)
        {
            int srcX = x * srcW / kInputWidth;
            // Convert uint8 [0,255] to int8 [-128,127] for quantized model
            dst[y * kInputWidth + x] = (int8_t)((int)src[srcY * srcW + srcX] - 128);
        }
    }
}

// Extract grayscale from RGB888 buffer using luminance approximation
// Fast version: uses green channel which approximates perceived brightness
static void rgb888ToGrayscale(const uint8_t *rgb, uint8_t *gray, int pixelCount)
{
    for (int i = 0; i < pixelCount; i++)
    {
        // Use green channel (offset 1) as fast grayscale approximation
        gray[i] = rgb[i * 3 + 1];
    }
}

bool setupPersonDetection()
{
    // Allocate tensor arena in PSRAM
    tensor_arena = (uint8_t *)ps_malloc(kTensorArenaSize);
    if (!tensor_arena)
    {
        Serial.println("ERROR: Failed to allocate tensor arena in PSRAM");
        return false;
    }
    Serial.printf("Tensor arena allocated: %d bytes in PSRAM\n", kTensorArenaSize);

    // Load the model
    model = tflite::GetModel(g_person_detect_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION)
    {
        Serial.printf("ERROR: Model schema version %d != supported %d\n",
                      model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    // Build interpreter
    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, kTensorArenaSize, &micro_error_reporter);
    interpreter = &static_interpreter;

    // Allocate tensors
    if (interpreter->AllocateTensors() != kTfLiteOk)
    {
        Serial.println("ERROR: AllocateTensors() failed");
        return false;
    }

    input = interpreter->input(0);
    output = interpreter->output(0);

    Serial.printf("TFLite ready — input: [%d x %d x %d], type: %d\n",
                  input->dims->data[1], input->dims->data[2],
                  input->dims->data[3], input->type);

    return true;
}

int detectPerson(camera_fb_t *fb)
{
    if (!interpreter || !input || !output)
    {
        Serial.println("TFLite not initialized");
        return -1;
    }

    // Step 1: Decode JPEG to RGB888
    int width = fb->width;
    int height = fb->height;
    size_t rgb_len = width * height * 3;
    uint8_t *rgb_buf = (uint8_t *)ps_malloc(rgb_len);
    if (!rgb_buf)
    {
        Serial.println("Failed to allocate RGB buffer for TFLite");
        return -1;
    }

    if (!fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb_buf))
    {
        Serial.println("JPEG decode failed in TFLite");
        free(rgb_buf);
        return -1;
    }

    // Step 2: Convert RGB to grayscale
    size_t pixel_count = width * height;
    uint8_t *gray_buf = (uint8_t *)ps_malloc(pixel_count);
    if (!gray_buf)
    {
        Serial.println("Failed to allocate grayscale buffer");
        free(rgb_buf);
        return -1;
    }
    rgb888ToGrayscale(rgb_buf, gray_buf, pixel_count);
    free(rgb_buf); // done with RGB

    // Step 3: Resize to 96x96 and quantize to int8 directly into input tensor
    resizeGrayscaleToInput(gray_buf, width, height, input->data.int8);
    free(gray_buf); // done with grayscale

    // Step 4: Run inference
    unsigned long t0 = millis();
    if (interpreter->Invoke() != kTfLiteOk)
    {
        Serial.println("TFLite Invoke() failed");
        return -1;
    }
    unsigned long inferenceMs = millis() - t0;

    // Step 5: Read output scores
    // output[0] = "no person" score, output[1] = "person" score
    // Both are int8: [-128, 127], we shift to [0, 255] for easier use
    int no_person_score = (int)output->data.int8[0] + 128;
    int person_score = (int)output->data.int8[1] + 128;

    Serial.printf("TFLite: person=%d, no_person=%d (%lums)\n",
                  person_score, no_person_score, inferenceMs);

    return person_score;
}