#pragma once

#include "esp_camera.h"

// Call once in setup() after camera init
bool setupPersonDetection();

// Run inference on a camera frame.
// Returns person confidence score: 0-255 (higher = more likely a person)
// Returns -1 on error.
// Note: takes ~2-5 seconds per inference on ESP32.
int detectPerson(camera_fb_t *fb);