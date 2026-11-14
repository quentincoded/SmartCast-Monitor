# SmartCast

Smart cast prototype (MTI course, ETH Zurich): a sensing sock under the cast measures pressure,
skin temperature, humidity and movement; an ESP32 sends the readings over Bluetooth LE to the
SmartCast Monitor app on an Android phone.

```
firmware/                 ESP32 firmware (PlatformIO). Currently sends realistic mock data.
  platformio.ini
  src/main.cpp
web/                      SmartCast Monitor app, published to GitHub Pages
  index.html              the whole app (UI, 3D foot, alerts, reports, Bluetooth)
  pathway.html            care pathway flowchart
  manifest.webmanifest, sw.js, icon-*.png   installable on the phone, works offline
tools/foot-model/         rebuilds the 3D foot mesh embedded in web/index.html
.github/workflows/        deploys web/ to GitHub Pages on every push
CLAUDE.md                 notes for Claude Code (build, flash, conventions)
```

## 1. Put the repo on GitHub and turn on Pages (once)

```bash
cd smartcast
git remote add origin git@github.com:<you>/smartcast.git     # create the empty repo on github.com first
git push -u origin main
```

Then on GitHub: **Settings → Pages → Build and deployment → Source: GitHub Actions**.
Every push that changes `web/` redeploys the app. Only `web/` is published; the firmware and tools stay
private in the repo but are not on the website.

App address: `https://<you>.github.io/smartcast/` (flowchart at `.../pathway.html`).

Pages on a **private** repo needs GitHub Pro, Team or Enterprise. GitHub Pro is free for students through the
GitHub Student Developer Pack. With a free account, make the repo public or keep it private without Pages.

## 2. Flash the ESP32 on Linux (no Arduino IDE needed)

Install PlatformIO, either the **PlatformIO IDE extension for VS Code** (it brings the `pio` command) or
just the command-line tool:

```bash
pipx install platformio          # or: pip install --user platformio
```

Let your user access the USB serial port (once, then log out and back in):

```bash
sudo usermod -aG dialout $USER   # Arch/Manjaro: uucp instead of dialout
```

Plug in the ESP32 and flash:

```bash
cd firmware
pio run -t upload                # first run downloads the ESP32 toolchain (a few minutes)
pio device monitor               # shows one JSON line per second, Ctrl+C to quit
```

ESP32-S3 board: `pio run -e esp32s3 -t upload`.

Troubleshooting:
- **No port found / permission denied:** check `ls /dev/ttyUSB* /dev/ttyACM*` after plugging in and
  confirm `groups` lists `dialout`. On Ubuntu, the `brltty` package sometimes grabs CH340 USB adapters:
  `sudo apt remove brltty`.
- **Stuck at "Connecting....":** hold the **BOOT** button on the board until the upload starts.
- Use a USB cable that carries data. Some cables only charge.

In the serial monitor you can type `scenario heel`, `speed 10`, `reset`. The BOOT button cycles scenarios.

## 3. Install the app on the Android phone

1. Open the Pages address in **Chrome** (Web Bluetooth only works in Chrome-based browsers on Android, over https).
2. Chrome menu (⋮) → **Install app**. SmartCast gets a home screen icon and opens full screen.
3. Turn on Bluetooth (on Android 11 and older also Location). Allow "Nearby devices" when Chrome asks.
4. In the app: **Setup → Data source → Bluetooth → Connect sensing sock** → pick **SmartCast-Sock**.
5. The controls under the Connect button start mock scenarios on the ESP32. "Heel pressure building up" at 60×
   gives a warning after about 20 s and a critical alert after about 40 s.

While connected, the app keeps the screen on, vibrates on alerts and reconnects by itself if the link drops.
The same works in Chrome on a laptop.

**Limits:** the app has to stay open (browsers pause Bluetooth in the background), Android + Chrome only (iPhones
do not support Web Bluetooth), one phone connected at a time.

## Data format (ESP32 → app)

One JSON line per sample, ended by `\n`, sent in 20-byte chunks over the Nordic UART Service
(`6e400001-…`, notify on `6e400003-…`, commands written to `6e400002-…`):

```json
{"t":60000,"p":[1688,1525,750,900,600,825,675],"T":[32.4,32.6,33.3,33.4,33.8,32.9,33.2],"rh":71.2,"a":[0.02,-0.99,0.05]}
```

`t` ms since boot (mock clock) · `p` raw ADC 0–4095, `null` = sensor missing · `T` °C · `rh` % RH · `a` g.
Sensor order: heel medial, heel lateral, medial malleolus, lateral malleolus, instep, 5th metatarsal head,
navicular/cuboid. The app converts ADC → mmHg with the table under Setup → Pressure calibration; the firmware
uses the same default table in reverse.

## Moving to real sensors

Keep the BLE code and the line format in `main.cpp` and replace the mock values in `simulate()` with real reads:
`analogRead()` for the FSRs (send raw ADC, no conversion on the ESP32), your temperature and humidity sensors,
and the IMU acceleration in g. Then calibrate and zero the sensors in the app.

Use **ADC1 pins** for the FSRs, because ADC2 clashes with Wi-Fi. Most classic ESP32 dev boards expose only 6 ADC1
pins (GPIO 32–36, 39), one short of 7 FSRs. Add a multiplexer (e.g. CD74HC4067) or use an ESP32-S3
(10 ADC1 channels).

## Later: background monitoring

For alerts with the phone locked, wrap `web/` in a native Android shell with Capacitor, swap Web Bluetooth for
`@capacitor-community/bluetooth-le` and add a foreground service for the connection and notifications.
The UI, 3D view, alerts and reports stay unchanged.

## Changing the 3D foot

The shape is defined in `web/index.html` (`castSDF`, `FP` and `TOES` in the CORE section). After editing it,
run `node tools/foot-model/build-mesh.js` to rebuild the embedded mesh.
