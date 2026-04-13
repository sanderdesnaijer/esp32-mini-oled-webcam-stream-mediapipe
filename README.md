# ESP32 Mini OLED Webcam Stream (MediaPipe + Arduino)

Stream your phone or laptop webcam to a tiny 128x64 SSD1306 OLED. Your browser captures video, processes it into a 1-bit stylized frame (dithered, edges, motion trail, scanlines, skull, glitch, SHODAN), and pushes it to the ESP32 over WiFi. The ESP32 just receives and draws.

Works on **iPhone (Safari)**, **Android (Chrome)**, and any desktop browser. Browsers block `getUserMedia` on any LAN IP unless the page is HTTPS, so the sketch runs both an HTTP and an HTTPS server using a self-signed certificate you generate once.

Keywords: ESP32, SSD1306, mini OLED, MediaPipe, Face Landmarker, webcam, Arduino, PlatformIO, ESP-IDF, HTTPS, iOS Safari camera, Chrome camera, face tracking, 1-bit dithering.

<!-- YouTube walkthrough (TODO: replace thumbnail + video ID) -->
[![Watch the video](docs/thumbnail.jpg)](https://youtu.be/YOUR_VIDEO_ID)

## Features

- Single web page at `https://<esp32-ip>/` with live preview and 1-bit preview side by side.
- 7 styles, live-switchable: Dithered, Edges, Motion Trail, Scanlines, Skull, Glitch, SHODAN. Skull and SHODAN use MediaPipe face landmarks. The rest work on any camera feed.
- Front / back camera toggle (phones).
- One Start / Stop button, threshold + FPS sliders, debug stats panel.
- Works over HTTPS on LAN so camera access is unblocked everywhere.
- No external HTTPS library. Uses the built-in `esp_http_server` + `esp_https_server` that ship with ESP32 Arduino core 3.x.

## Hardware

| Part | Notes |
|---|---|
| ESP32 dev board | Any ESP32 with WiFi. ESP32-WROOM-32 is the classic. |
| 0.96 inch SSD1306 OLED, 128x64, I2C | The 4-pin modules (VCC, GND, SCL, SDA). |
| 4 jumper wires | Female-to-female or female-to-male, depending on your board. |
| USB cable | For flashing. |

Total cost around 10 to 15 USD.

## Wiring

Default pins: `SDA=21`, `SCL=22`.

| OLED | ESP32 |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SCL | GPIO 22 |
| SDA | GPIO 21 |

If your OLED address is not `0x3C`, change `#define OLED_ADDR` near the top of the sketch. Any I2C scanner sketch will tell you the address.

## Software setup (Arduino IDE)

### 1. Install the Arduino IDE

Download from [arduino.cc/en/software](https://www.arduino.cc/en/software). Version 2.x or newer.

### 2. Add ESP32 board support

1. **File > Preferences** (macOS: **Arduino IDE > Settings**).
2. In **Additional Board Manager URLs**, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. **Tools > Board > Boards Manager**, search **esp32**, install **esp32 by Espressif Systems**. You need version **3.0.0 or newer**, because the sketch uses the built-in `esp_https_server` component.

### 3. Install the libraries

**Sketch > Include Library > Manage Libraries**, then install:

- **Adafruit GFX Library**
- **Adafruit SSD1306**

That is all. No HTTPS library is needed because it ships with the ESP32 core.

### 4. Select your board

- **Tools > Board > esp32 > ESP32 Dev Module** (or whichever matches your board).
- **Tools > Port** and pick the ESP32's serial port.

### 5. Configure WiFi

Open `browser-oled.ino` and edit these two lines near the top:

```cpp
const char* ssid     = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";
```

### 6. Generate a certificate

The sketch will not compile until you paste a real self-signed certificate and private key in. This is a one-time step.

**Option A: use the helper script** (macOS / Linux / Git Bash on Windows / WSL):

```
bash scripts/generate_cert.sh
```

It prints two blocks to your terminal. Copy them and paste them over the placeholder `cert_pem[]` and `key_pem[]` blocks in `browser-oled.ino`.

**Option B: plain openssl** (any platform with openssl):

```
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout key.pem -out cert.pem -days 3650 \
  -subj "/CN=esp32.local"
```

Open `cert.pem` and `key.pem` in a text editor. For each line in `cert.pem`, wrap it as `"line\n"` and paste inside the `cert_pem[]` block in the sketch. Same for `key.pem` into `key_pem[]`. Keep the `-----BEGIN-----` and `-----END-----` lines.

**Do not commit your private key to a public repo.** Keep `cert.pem` and `key.pem` in `.gitignore`. The only file that will end up with keys in it is your local `browser-oled.ino`, which is also why you should not commit your edited sketch back upstream.

### 7. Upload

Click the **Upload** arrow. The first compile takes a few minutes because mbedtls and the HTTPS server components get linked in. Subsequent builds are fast.

Open the **Serial Monitor** at 115200 baud:

```
Connecting to WiFi..........
Connected. IP address: 192.168.1.93
HTTP  listening on http://192.168.1.93/
HTTPS listening on https://192.168.1.93/
```

The OLED shows the same URL and waits for a browser to connect.

## Software setup (PlatformIO)

```ini
[env:esp32dev]
platform = espressif32 @ ^6.8.0
board = esp32dev
framework = arduino
monitor_speed = 115200
lib_deps =
    adafruit/Adafruit GFX Library
    adafruit/Adafruit SSD1306
```

Run the cert generator, paste, edit WiFi, `pio run -t upload`.

## First run

1. Wait for the OLED to show the `https://<ip>/` URL.
2. On your phone or laptop (same WiFi), open that exact URL. **Must be https, not http.**
3. The browser will warn about the cert. This is expected, it is self-signed.
   - **iPhone / Safari:** tap **Show Details**, then **visit this website**, then **Visit Website**.
   - **Chrome / Android:** tap **Advanced**, then **Proceed anyway**.
4. Wait for `Ready. Press Start.` at the bottom of the page. The first load downloads the MediaPipe model (a few MB), later loads are cached.
5. Tap **Start** and allow camera access.
6. Pick a style. Your webcam now streams to the OLED.
7. Switch **Front** / **Back** camera anytime from the dropdown.

## How it works

The heavy work is in the browser:

1. `getUserMedia` opens the camera.
2. MediaPipe Face Landmarker runs on the GPU (WebGL).
3. A 128x64 canvas renders the chosen style using the raw frame and, for Skull / SHODAN, the landmarks.
4. The canvas is packed to SSD1306 page format (1024 bytes, 1 bit per pixel) and base64-encoded.
5. A POST to `/frame` on the ESP32 sends it.

The ESP32:

1. Receives the POST body.
2. Base64-decodes with a lookup table (around 40 microseconds for a frame).
3. Copies directly into the SSD1306 buffer, then pushes over I2C at 400 kHz.

That keeps the ESP32 side tiny and fast, so it comfortably handles 10 to 15 FPS. All the vision work stays in the browser.

## Tuning performance

- **FPS slider:** start at 10, lower it if WiFi is slow.
- **Threshold slider:** affects Edges style.
- **I2C clock:** 400 kHz in the sketch, some OLEDs tolerate 800 kHz. Edit `Wire.setClock(400000);`.

## Troubleshooting

**OLED shows nothing.** Check wiring and that `OLED_ADDR` matches your display (`0x3C` or `0x3D`).

**Compile error: "cert_pem is empty".** You skipped step 6. Generate a cert and paste it in.

**"Camera error: undefined is not an object".** You visited `http://` instead of `https://`. The sketch redirects HTTP to HTTPS, but if you typed the IP manually, type `https://`.

**First HTTPS visit keeps getting blocked.** Safari sometimes caches the refusal. Close the tab, clear it, and try again. If that fails, restart Safari.

**Frames flicker or drop.** Lower the FPS slider. Bottleneck is usually WiFi round trip, not the ESP32.

**Different LAN, same WiFi.** Some guest networks block peer-to-peer. Try your main WiFi.

## License

MIT. Credit appreciated but not required.

## Credits

- [MediaPipe Face Landmarker](https://developers.google.com/mediapipe/solutions/vision/face_landmarker) by Google.
- [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306) and [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library) libraries.
- Built on top of the ESP-IDF [`esp_http_server`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_http_server.html) and [`esp_https_server`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_https_server.html) components.
