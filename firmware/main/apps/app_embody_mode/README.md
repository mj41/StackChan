# Embody Mode

Launcher app (first icon) that lets you control this Stack-chan from a browser through [stackchan-server](https://github.com/mj41/stackchan-server). The server's readme defines the protocol, and the design lives in `../stackchan-mj/docs/design.md`.

## On the robot

1. **Wi-Fi:** opening the app starts Wi-Fi (loading page) and connects to `CONFIG_STACKCHAN_EMBODY_SERVER_URL`.
2. **QR code:** the screen shows the server's one-time pairing URL as a QR code, next to its 8-character code.
3. **Face:** once a browser pairs, the face appears.
   - **Tap:** sends a `screen_tap` event with x/y.
   - **Long press:** toggles the QR code, so another viewer can pair.
   - **Double tap:** blanks the screen (manual screensaver).
4. **LIVE badge:** a red **LIVE** badge shows while the camera or microphone streams.
5. **Screensaver:** the screen goes black and the previous view (face, picture or QR) comes back on wake. The LIVE badge stays visible. A "touch" below means the screen, a head press or swipe, or a new NFC tag. There are two kinds:
   - **Auto:** after `CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S` (default 60 s, 0 = never) without touch, command, picture or live media. Every command, even `ping`, counts as use, so a remotely used robot never blanks by itself. Touch or a command wakes it.
   - **Manual:** a double tap or the `screensaver {"on": true}` command. It stays blank until a touch or `screensaver {"on": false}`.
   - **Reporting:** events `screensaver_on {manual}` and `screensaver_off`, and telemetry `screensaver` (0 off, 1 auto, 2 manual).
6. **Standby:** the `standby {"minutes"}` command (1–120) sends a `standby` event and then disconnects.
   - **While away:** backlight, LEDs, camera and mic are off and the screen is blank. The servos already release torque when idle.
   - **Coming back:** when the time is up, or immediately on a touch (screen or head), it restores the brightness and reconnects, then sends `standby_end {touched}`.
   - **Events:** any events raised while offline wait and go out after reconnecting.
7. **Closing:** swiping up closes the app and warm-reboots to the launcher. Wi-Fi can't be stopped cleanly, which is the same reason AVATAR reboots.

## Configuration

The Kconfig menu "Embody Mode" (`main/Kconfig.projbuild`) has two options, both set in `firmware/sdkconfig.defaults.local`. That file is gitignored because it holds the token:

```
CONFIG_STACKCHAN_EMBODY_SERVER_URL="ws://192.168.1.10:8765"   # or wss://chan.w42.eu
CONFIG_STACKCHAN_EMBODY_TOKEN="<the server's robot-token file>"
```

- **Existing `sdkconfig` wins:** defaults files only fill options missing from the existing `sdkconfig`. After changing the overlay, edit the same lines in `sdkconfig` too.
- **Reconfigure first:** after adding sources or Kconfig options, run `idf.py reconfigure` before `idf.py build`.

The robot ID is `stackchan-<factory MAC, lowercase>`.

Other options in the same menu:

- `CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S`: seconds without touch before the screen blanks. Default 60; 0 disables it.
- `CONFIG_STACKCHAN_EMBODY_ONLY`: the launcher installs only Embody Mode and SETUP, and ignores "start AI.AGENT on boot". Default off.

## What it does

| Area | Details |
|---|---|
| Commands | `ping` (answered in `embody_client` with the queue time), `nod`, `shake`, `look`, `home`, `emotion`, `say`, `sticker`, `face`, `leds`, `brightness`, `volume`, `screensaver`, `standby`, `camera`, `mic`, `nfc`, and the `speaker` capability (audio arrives as binary `0x03`). `nfc` is listed only when the reader answers at startup. Pitch is clamped to 5–85° (M5Stack safe range) |
| Pictures | binary `0x10` JPEG, decoded with `jpeg_dec::decode_to_lvgl` and shown over the face |
| Camera | `StreamCaptures()`, then `image_to_jpeg` (quality 25), sent as binary `0x01` every 200 ms while on |
| Microphone | a FreeRTOS task reads the audio codec at 24 kHz, channel 1 (as in the SETUP mic test). Sent as binary `0x02` in 50 ms messages |
| Speaker | binary `0x03` PCM is resampled to the codec's 24 kHz and queued (max ~3 s). A FreeRTOS task writes 20 ms chunks with `OutputData` and switches output off after 0.5 s of silence. The mouth moves while it plays, and the audio counts as use for the screensaver |
| LEDs | `leds` args: `left`/`right` fade a whole side (NeonLight); `pixels` sets up to 12 single LEDs (left 0–5, then right 6–11; `null` skips one); `effect` `rainbow`, `breathe`, `chase`, `blink` or `off`, with `color`, `speed` (0.2–5) and `seconds` (0 = until the next `leds`). Effects are drawn at 25 fps in the app loop, both sides mirrored; a timed effect restores the side colours when it ends |
| Light, proximity | `hal/drivers/LTR553` (LTR-553ALS in the CoreS3, 0x23), read in the app loop: proximity every 200 ms, light every 500 ms. **Auto-brightness** (on by default when the sensor answers) maps the smoothed lux on a log scale to 10–100 % backlight (≈55 % at 100 lux) and changes it only in steps of 4 % or more; `brightness {"value"}` ends it, `brightness {"auto": true}` turns it back on; paused during standby. **Approach:** `proximity_near` / `proximity_far` events against a slowly adapting baseline (+150 / +60); coming near wakes the screen like a touch, but not standby |
| Infrared | `hal/drivers/IrRemote`: RMT TX on G5 (IR LED on top of the robot; 38 kHz carrier, 50 %), RX on G10 (1 MHz ticks, 12 ms frame end, 512-symbol buffer via ping-pong). Received frames become `ir_received` with the raw mark/space timings and, when it decodes, NEC `address`/`command`; NEC repeats and short noise are skipped. `ir_send` sends NEC or raw timings, with `repeat` (NEC repeat codes every 108 ms, or the raw frame again after 40 ms: "hold") and `frames` (the whole code up to 5×, 108 ms apart, for weak links); the robot ignores its own echo for 150 ms unless `loopback` is set (self-test). The proximity sensor pauses while sending. The on-board LED is weak (~70 mA), so range is short |
| Power | Telemetry from the INA226 (`hal/drivers/INA226`, body battery: `body_battery_v`, `body_current_ma` + drain / − charge, `body_power_mw`, `body_shunt_uv`) and the AXP2101 (`core_battery_v`, `core_vbus_v`, `core_system_v`, `core_charge`, `core_charge_phase`, `pmic_temp_c`, `pmic_status1/2`). The AXP2101 IRQ status (0x49) is polled every 100 ms: `power_button {press}` (a short press wakes the screen), `usb_plugged`/`usb_unplugged`, `battery_inserted`/`battery_removed` |
| Hold | `hold {"seconds": 30..300}` keeps both servos powered at their current angle (auto torque release off), then restores release at rest; `0` ends it early, as do standby and closing the app. Events `hold_on`/`hold_off`, telemetry `hold_s` |
| Power LED | `power_led {"mode"}`: the AXP2101 CHGLED (reg 0x69): off, blink (1 Hz), fast (4 Hz), on (the boot default), charging (the charger drives it) |
| NFC | `hal/drivers/ST25R3916`: a minimal ISO14443A reader ported from M5Stack's UiFlow2 driver (MIT). A FreeRTOS task (priority 1, core 1) polls twice a second with the RF field on only for the ~30 ms of each poll; the chip's IRQ pin isn't wired on StackChan. It reads 4- and 7-byte UIDs and, for Type 2 tags (NTAG, Ultralight), the first NDEF record (URI or text). It stops during standby. `nfc {"on"}` switches polling (on by default) |
| Telemetry (every 2 s) | battery, charging, head yaw/pitch, Wi-Fi RSSI, free heap, uptime, brightness, volume, screensaver (0/1/2); sensors: `imu_ax_g`/`imu_ay_g`/`imu_az_g` and `imu_gyro_dps` (HAL snapshot, 10 Hz), `mag_x/y/z_ut`, `mag_raw_x/y/z`, `mag_rhall` (BMM150 through the BMI270's AUX interface), `yaw_`/`pitch_load_pct` and `_temp_c` plus `servo_voltage_v` (one servo feedback read per servo, in the app loop that owns the servo bus), `chip_temp_c` (ESP32-S3 sensor), `light_lux`, `proximity` (0–2047, only while on), `proximity_on`, `auto_brightness` (0/1) |
| Events | IMU shake, head-touch press (zones `z0`–`z2`, 0–3) / release (`ms`) and swipes, screen taps, screensaver on/off, standby, `nfc_tag {uid, type, atqa, sak, text}` and `nfc_removed {uid}` (after two missed polls) |

## Code

- `embody_client.{h,cpp}`: WebSocket client, using xiaozhi's `WebSocket` and ArduinoJson.
  - It runs from the app loop the way `hal/hal_ws_avatar.cpp` does. Incoming frames are queued by the socket task and handled in `update()`.
  - It reconnects with backoff from 1 s up to 30 s.
- `app_embody_mode.{h,cpp}`: UI, commands, motion gestures, pictures, camera and microphone.
  - Commands and pictures are queued during `update()` and applied under the LVGL lock.
  - Events from HAL tasks, the NFC task and LVGL callbacks go through a mutex-protected queue. Event data can hold numbers and strings.

## Notes

- **Launcher order:** this app is launcher index 0 and AI.AGENT is 1. Other apps return via hardcoded `requestWarmReboot(<index>)` values, which are +1 compared with upstream.
- **Update hole closed:** `patches/xiaozhi-esp32.patch` disables xiaozhi's OTA server check (`CONFIG_STACKCHAN_XIAOZHI_OTA_CHECK`, off). Firmware is never installed from a server.
- **Send limit:** xiaozhi's `WebSocket::Send` rejects messages over 65,535 bytes. That limits camera frames, not pictures, which the robot only receives.
