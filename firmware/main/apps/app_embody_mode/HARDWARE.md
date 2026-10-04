# Stackchan hardware vs Embody Mode coverage

What each part of the robot is and how much of it Embody Mode exposes. Sources:
- [M5Stack StackChan docs](https://docs.m5stack.com/en/StackChan): chips, addresses, pins
- [CoreS3 docs](https://docs.m5stack.com/en/core/CoreS3)
- the firmware HAL (`stackchan/firmware/main/hal/`, drivers in `hal/drivers/`)

**Legend:**
- **yes**: the basic app covers it.
- **partial**: some of it is exposed.
- **driver, unused**: the firmware can use it, but Embody Mode doesn't.
- **no driver**: the firmware has no support yet.

## Sensors (inputs)

| Part | Chip (I2C / pins) | Firmware | Embody Mode |
|---|---|---|---|
| Touch screen | FT6336U (0x38), 2 points | LVGL input (first point); the board driver now reads both points (0x02..0x0C) | **yes**: taps with x/y, double tap, long press; raw `touch_down`/`touch_up` per finger (id, x, y, duration) and a 50 Hz raw stream of both fingers (binary 0x06, while a browser asks) |
| Camera | GC0308 (0x21), up to 640x480 | `StackChanCamera` (+ `SetSensorSize`, raw register access) | **yes**: live JPEG stream at 320x240 (5 fps) or 640x480 (about 2.3 fps; `camera {on, size}`); `snapshot` switches the sensor to 640x480 for one still (binary 0x07); mirror/flip; raw sensor registers (`camera_reg`) |
| Microphones | 2x, ES7210 codec (0x40), MIC1-3 enabled, 2 TDM slots reach the ESP32 | audio codec (xiaozhi opens 2 channels: mic + echo reference) | **yes**: both channels raw as binary 0x04 (24 kHz, interleaved). Channel 1 is the microphone; channel 0 is the speaker reference (loopback of what the robot plays, for echo cancellation), verified 2026-09-30 with Beep: loud on 0, quiet on 1. A third ES7210 input would need the codec opened with more TDM slots (shared with the AI agent) |
| Accelerometer + gyroscope | BMI270 (0x69) | driver; accel feeds the shake detector | **yes**: `shake` event, telemetry acceleration per axis (g) plus rotation rate (°/s) from a 10 Hz snapshot, and a raw 100 Hz stream (binary 0x05: accel, gyro, magnetometer) while a browser asks (`imu_stream`). Pick-up is commented out upstream. The IMU sits in the CoreS3, so it tilts with the head pitch |
| Magnetometer (compass) | BMM150 (0x10) on the BMI270's AUX I2C | `BMI270::beginMagnetometer()` (Bosch SensorAPI AUX: manual setup, then data mode; Bosch compensation with the factory trim) | **yes**: `mag_x/y/z_ut` (compensated µT), `mag_raw_x/y/z`, `mag_rhall`; regular preset at 10 Hz. First reading ≈300 µT total vs ≈50 µT Earth field: a large hard-iron offset from the speaker and servo magnets, so a compass needs calibration (left to apps) |
| Proximity + ambient light | LTR-553ALS-WA (0x23) | `hal/drivers/LTR553` (minimal, polled) | **yes**: `light_lux`, raw `light_ch0`/`light_ch1`, `proximity` telemetry; auto-brightness from the room light (on by default); `proximity_near`/`_far` events, and an approach wakes the screen |
| Head touch panel, 3 zones | Si12T (0x68) | driver: per-zone intensity, gestures | **yes**: `head_press` with per-zone intensity (0–3), `head_release` with duration, swipe forward/back. Any of them wakes the screen |
| Servo feedback | 2x Feetech SCS0009 (serial bus) | position, moving, current, load (stall protection) | **yes**: angles, and per servo telemetry: load (%), temperature, supply voltage, raw position (0–1000), raw speed, raw current (units not documented), moving (one feedback read every 2 s) |
| IR receiver | IRM56384 on the Touch board (**G10**) | `hal/drivers/IrRemote` (RMT RX) | **yes**: `ir_received` with raw timings, NEC decoded (verified with an NEC remote) |
| NFC reader | ST25R3916 (0x50), IRQ not wired | `hal/drivers/ST25R3916` (minimal NFC-A port of UiFlow2's driver) | **yes**: `nfc_tag` with UID/type and the first NDEF record (URI or text) of Type 2 tags, `nfc_removed`, `nfc {on}`. Polled twice a second; the field is on only while polling. Verified with an NTAG (7-byte UID); NDEF text not yet tried on a written tag |
| Battery (PMU) | AXP2101 (0x34) | level, charging | **yes**: battery %, charging, and raw: battery/USB/system voltage, charge state and phase, die temperature, status registers 0x00/0x01; `usb_plugged`/`usb_unplugged`, `battery_inserted`/`battery_removed` events |
| Battery monitor (body) | INA226 (0x41), 10 mΩ shunt R18 between BAT+ and BAT_IN | `hal/drivers/INA226` (minimal) | **yes**: voltage, current, power, raw shunt µV. **+ = drain, − = charging** (verified: +17 mA idle, −125 mA while charging). BAT+ is on M-Bus pin 30 (BATTERY), so it is most likely the same battery the AXP2101 charges (4.121 vs 4.125 V) |
| USB ports (head CoreS3 + stand) | both feed the same AXP2101 VBUS and the same ESP32-S3 USB data lines | `usb_serial_jtag_is_connected()` | **yes**: `usb_data` (a computer sends USB frames; a charger does not), `core_vbus_v`, plug events. **Which port is used cannot be detected** (tested 2026-09-30: identical readings) |
| Power button | on the AXP2101 PWRON | IRQ status 0x49 (enabled in 0x41) | **yes**: `power_button {press: short|long}`; a short press wakes the screen. Held ~4 s, the AXP2101 still powers off |
| RTC | BM8563 (0x51) | time sync (PCF8563 driver), `Hal::getRtcUnix` | **yes**: `rtc_unix` next to `system_unix` in telemetry. The alarm (for timed power-off) is not used yet |
| Chip temperature, Wi-Fi | ESP32-S3 | available | **yes**: Wi-Fi RSSI, free memory, chip temperature (`chip_temp_c`) |

## Actuators and outputs

| Part | Chip (I2C / pins) | Firmware | Embody Mode |
|---|---|---|---|
| Display | 2.0" ILI9342C/E, 320x240 | LVGL, avatar, backlight | **yes**: face, emotions, speech, stickers, pictures, QR, blank screen, brightness |
| Head yaw servo (360°) | SCS0009 | angle + speed, `rotate()` (continuous), torque on/off | **yes**: angle ±128°, nod/shake/home; continuous `rotate` for 1–30 s after a Yes on the robot's screen (a cable in the head would wind up), stopped when the head stops turning or a cable is plugged in. Torque is released automatically at rest (firmware default), so the head can be moved by hand; `hold {seconds}` keeps it powered for 30 s–5 min (off by default). Hand moves show in the angle telemetry (no derived event, by choice) |
| Head pitch servo (90°) | SCS0009 | same | **partial**: angle **5–85°** (M5Stack safe range), nod. Torque released at rest (see yaw) |
| RGB LEDs, 12 | WS2812C x12, two rows, via the PY32L020 body expander (0x6F) | `setRgbColor(0..11)`, `NeonLight` left 0–5 / right 6–11 with fades | **yes**: one colour for all, per-LED colours, and effects drawn on the robot (rainbow, breathe, chase, blink; colour, speed, optional duration) |
| Speaker | AW88298 1 W (0x36) | codec output, volume, xiaozhi sounds | **yes**: talk from the browser, sound files, beep (raw 24 kHz PCM, LAN), volume |
| IR transmitter | IR LED on top of the robot near the display unit, S8050 from BUS_5V (**G5**; found with a phone camera) | `hal/drivers/IrRemote` (RMT TX, 38 kHz) | **yes**: `ir_send` NEC or raw, `repeat` (hold: NEC repeat codes) and `frames` (whole code up to 5×); self-test (`loopback`) passed: the robot decodes its own frame. The proximity sensor pauses while it sends. **Weak:** R1 = 51 Ω from 5 V gives ~70 mA peaks (carrier 38 kHz, 50 % duty); an NEC ceiling light reacted with the top of the robot pointed at it, but not reliably. The LED points up, near the display unit |
| Power/charge LED (red) | on the AXP2101 (CHGLED, reg 0x69) | on at boot | **yes**: `power_led` on/off/blink/fast/charging |
| Servo power | PY32 pin 0 | `setServoPowerEnabled()` | **yes**: `servo_power {on}`, telemetry `servo_power` |
| Laser | GPIO2 = CoreS3 **Port A**, probably a Grove accessory | `setLaserEnabled()`, used by ESPNOW.REMOTE | **no** (optional hardware) |
| microSD | slot | not used | **no** (designed: per-app folders next to the internal store) |
| Grove ports | CoreS3 Port A/B/C, Power board J1/J2 | not used | **no** (later, needs hardware to test) |

## What is not covered

Embody Mode covers screen, touch, camera, both microphone channels, speaker, both servos (by angle and continuous yaw, with load, temperature and voltage), all 12 LEDs with effects, head-touch zones and gestures, IMU and magnetometer, light and proximity with auto-brightness, NFC, IR send and receive, RTC, chip temperature, battery and power. Not yet:

- **The laser** (Port A accessory), when fitted.
- **NFC beyond reading:** writing tags, MIFARE Classic sectors, ISO14443-4 (bank cards, phones).
- **A third microphone input** of the ES7210 (needs the codec opened with more TDM slots, shared with the AI agent).
- **microSD:** designed (per-app folders, [device storage](https://github.com/mj41/home-w42-eu/blob/main/docs/device-storage.md)), not built.
- **Grove ports** and the **RTC alarm** (for timed power-off, planned).

## Safety

- **Pitch:** M5Stack says to keep the Y (pitch) servo within **5–85°**. Extreme angles can stall it and damage it permanently. Embody Mode clamps pitch to 5–85° in both the firmware and the dashboard; the HAL itself allows 3–87°.
- **By hand:** the firmware releases servo torque about 0.2 s after each move (`_auto_torque_release_enabled`, on by default). At rest the head is free and auto angle sync tracks it. Don't turn it during a move (nod, shake, slider), when torque is on. Keep release-at-rest as the default: holding costs battery, heat and noise.
