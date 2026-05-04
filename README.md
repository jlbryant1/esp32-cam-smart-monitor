# ESP32-CAM Smart Monitor

A real-time security camera system built on the ESP32-WROVER that detects motion, identifies people using TensorFlow Lite, and streams alerts to a live web dashboard.

The system uses frame-differencing as a cheap pre-filter to catch movement, then runs a quantized MobileNet model on-device to confirm whether the motion was caused by a person cutting down on false positives from pets, shadows, and other noise. When a person is confirmed, the ESP32 triggers a hardware alert (LED + buzzer), captures a JPEG snapshot, and POSTs it to a Node.js dashboard that displays events in real time via WebSockets.

[Hardware Setup](docs/hardware.jpg)

## Architecture

```
ESP32-WROVER (Camera + TFLite)
        │
        ├── Frame Differencing (motion pre-filter, ~1ms)
        │         │
        │         ▼ motion detected
        ├── TFLite Person Detection (96x96 MobileNet, ~5s inference)
        │         │
        │         ▼ person confirmed (score >= 200/255)
        ├── GPIO Alert ──► LED (GPIO 13) + Buzzer (GPIO 33)
        ├── MJPEG Stream ──► Browser (port 80)
        └── HTTP POST ──► Node.js Dashboard (JPEG + score)
                                │
                                ▼
                          Express + Socket.io
                                │
                                ▼
                          Live Web Dashboard
                     (real-time event feed)
```

## Tech Stack

**Firmware (C++)**
- ESP-IDF / Arduino framework on ESP32-WROVER with 4MB PSRAM
- esp32-camera library for OV2640 sensor interface
- TensorFlow Lite Micro for on-device ML inference
- Pre-trained quantized person detection model (~250KB)
- MJPEG streaming over HTTP

**Dashboard (JavaScript)**
- Node.js + Express backend
- Socket.io for real-time WebSocket push
- Vanilla JS frontend no framework overhead

## Hardware

| Component | Details |
|-----------|---------|
| Board | ESP32-WROVER-KIT (4MB PSRAM) |
| Camera | OV2640 (QVGA, JPEG mode) |
| LED | Standard LED on GPIO 13 |
| Buzzer | Active buzzer on GPIO 33 |

[Wiring](docs/wiring.jpg)

## How It Works

**Motion Detection** — Each frame is decoded from JPEG to RGB, and the green channel of every 4th pixel is compared against the previous frame. If more than 15% of sampled pixels changed by more than a threshold value across 3 consecutive frames, motion is flagged. This runs in ~1ms and filters out the vast majority of idle frames before TFLite ever needs to run.

**Person Detection** — When motion is detected, the current frame is decoded, converted to grayscale, resized to 96x96 using nearest-neighbor interpolation, quantized to int8, and fed into a TFLite Micro interpreter running a pre-trained MobileNet. The model outputs a person confidence score from 0-255. Scores above 200 trigger an alert. Inference takes ~5 seconds on the ESP32's single core which is acceptable for a security use case where the motion pre-filter keeps inference from running continuously.

**Non-blocking Alerts** — The LED and buzzer use a timer-based state machine instead of blocking `delay()` calls, so the camera stream stays smooth even while alerts are active.

**Dashboard** — On person detection, the ESP32 POSTs the raw JPEG frame buffer and confidence score to the dashboard's `/api/event` endpoint. The Express server stores events in memory and pushes them to connected browser clients via Socket.io. The frontend renders captured images, timestamps, and confidence bars in real time.

## Project Structure

```
├── firmware/
│   ├── src/
│   │   ├── main.cpp              # Setup, motion detection, main loop
│   │   ├── app_httpd.cpp         # MJPEG stream server
│   │   ├── person_detect.h       # TFLite inference interface
│   │   ├── person_detect.cpp     # TFLite model loading + inference
│   │   ├── camera_pins.h         # GPIO pin definitions for WROVER
│   │   ├── person_detect_model_data.h
│   │   └── person_detect_model_data.cpp
│   └── platformio.ini
│
└── dashboard/
    ├── server.js                 # Express + Socket.io backend
    ├── package.json
    └── public/
        └── index.html            # Real-time dashboard UI
```

## Setup

### Firmware

1. Install [VS Code](https://code.visualstudio.com/) + [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
2. Open the `firmware/` folder in PlatformIO
3. Copy the TFLite model data files into `src/`:
   ```
   .pio/libdeps/esp-wrover-kit/TensorFlowLite_ESP32/examples/person_detection/
   ```
   Copy `person_detect_model_data.h` and `person_detect_model_data.cpp` to `firmware/src/`
4. Update WiFi credentials in `main.cpp`
5. Update `DASHBOARD_URL` in `main.cpp` with your server's IP
6. Build and flash:
   ```
   pio run --target upload
   ```

### Dashboard

```bash
cd dashboard
npm install
npm start
```

Dashboard runs on `http://localhost:3000`. Detection events appear in real time when the ESP32 identifies a person.

## Tuning

| Parameter | Default | What it does |
|-----------|---------|--------------|
| `MOTION_THRESHOLD` | 15 | Per-pixel change required to count as "different" |
| `MOTION_PERCENT` | 15 | % of pixels that must change to flag motion |
| `MOTION_FRAMES_REQUIRED` | 3 | Consecutive motion frames before triggering TFLite |
| `PERSON_THRESHOLD` | 200 | TFLite score (0-255) required to confirm a person |
| `COOLDOWN_MS` | 5000 | Minimum ms between alerts |

## Challenges and Solutions

- **VSYNC Overflow** — Switching from `CAMERA_GRAB_WHEN_EMPTY` to `CAMERA_GRAB_LATEST` with double frame buffers in PSRAM eliminated frame overflow errors that occurred when the camera outpaced the processor.

- **JPEG vs Raw Pixel Comparison** — Motion detection on compressed JPEG bytes produced meaningless results since compression artifacts vary frame-to-frame. Fixed by decoding to RGB first, then comparing the green channel as a fast luminance approximation.

- **Streaming + Detection Conflict** — Running motion detection on every streamed frame caused choppy video. Limiting detection to every 5th frame during streaming kept the feed smooth while maintaining responsiveness.

- **Blocking Alerts Freezing Stream** — Replaced `delay()`-based LED/buzzer blinking with a non-blocking timer state machine so alerts don't interrupt the MJPEG stream.

## License

MIT
