# SmartCast: notes for Claude Code

Smart cast prototype: ESP32 firmware (sends sensor data over BLE) + a single-file web app (SmartCast Monitor)
hosted on GitHub Pages and installed on Android via Chrome. See README.md for the full picture.

## Layout
- `firmware/` PlatformIO project, Arduino framework. `src/main.cpp` is the whole firmware (mock data today).
- `web/index.html` the whole app: HTML + CSS + JS + embedded 3D mesh (`const MESH=` line, ~700 KB, don't print it).
  Pure logic sits between `/*CORE-START*/` and `/*CORE-END*/` and has no DOM access, so it can be tested in Node.
- `web/pathway.html` care pathway flowchart (static).
- `tools/foot-model/build-mesh.js` regenerates the `MESH` line from `castSDF` in the CORE section.
- `.github/workflows/pages.yml` deploys `web/` to GitHub Pages on push to `main`.

## Firmware: build, flash, monitor
```bash
cd firmware
pio run                          # build only
pio run -t upload                # flash (auto-detects the port)
pio device list                  # find the port if auto-detect fails
pio run -t upload --upload-port /dev/ttyUSB0
pio device monitor               # 115200 baud; needs a real terminal, so ask the user to run it
                                 # interactively, or read a few lines non-interactively with:
timeout 8 pio device monitor --quiet --no-reconnect 2>/dev/null | head -n 5
```
- Default env `esp32dev` (classic ESP32). For ESP32-S3: `-e esp32s3`.
- Serial port permission errors: user must be in `dialout` (Arch: `uucp`), then log out/in. Don't use sudo for pio.
- "Connecting...." timeout: ask the user to hold the BOOT button during upload.
- If `pio` is missing: `pipx install platformio` (or the VS Code PlatformIO extension's
  `~/.platformio/penv/bin/pio`).
- Serial commands (send as a line): `scenario normal|heel|swelling|warm|wet|dropout`, `speed 1|10|60`, `reset`.

## Contract between firmware and app (keep in sync)
- BLE Nordic UART Service: service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`, TX notify `…0003`, RX write `…0002`.
  Device name prefix `SmartCast` (the app filters on it).
- One JSON line per sample terminated by `\n`, sent in 20-byte chunks; app reassembles at `\n`.
- Keys: `t` (ms, device clock), `p` (7 raw ADC 0–4095 or null), `T` (7 °C or null), `rh` (% RH), `a` ([x,y,z] g).
  Replies to commands: `{"ack":"..."}`.
- Sensor order (index 0..6): heel medial, heel lateral, medial malleolus, lateral malleolus, instep,
  5th metatarsal head, navicular/cuboid. Defined as `SENS` in web/index.html and as array order in main.cpp.
- Default calibration table (ADC→mmHg) is `DEFAULT_CAL` in index.html and `CAL_ADC/CAL_MMHG` in main.cpp.
  Change both together. App side parsing: `ingest()` and `bleLine()` in index.html.

## Web app conventions
- No build step, no framework. Libraries from cdnjs: three.js r128, jsPDF 2.5.1 (also cached in `sw.js`).
- Test locally: `cd web && python3 -m http.server 8000`, then open http://localhost:8000 in Chrome
  (localhost counts as secure, so Web Bluetooth works there too).
- After changing web files that should reach phones offline, bump `CACHE` in `web/sw.js`.
- Check JS syntax after edits: extract the last `<script>` block and run `node --check` on it.
