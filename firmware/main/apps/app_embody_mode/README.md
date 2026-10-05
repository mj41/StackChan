# Embody Mode

To set up a robot, see [SETUP.md](SETUP.md).

Launcher app (first icon) that lets you control this Stackchan from a browser through [s-w42-eu-raw](https://github.com/mj41/s-w42-eu-raw). The protocol is the [device wire protocol](https://github.com/mj41/home-w42-eu/blob/main/docs/wire-protocol.md) of [home-w42-eu](https://github.com/mj41/home-w42-eu); the s-w42-eu-raw readme lists the robot's commands, and the trust design is [design.md](https://github.com/mj41/home-w42-eu/blob/main/docs/implementations/stackchan-trust.md) in stackchan-mj.

> **A proof of concept, vibe coded.** Written with AI agents and tested on real hardware at
> home, but neither the code nor its security has been reviewed by humans. Use it on your
> own network, and don't trust it with anything private yet.
>
> **Want more?** Ask in the [issues](https://github.com/mj41/home-w42-eu/issues), and ideally [sponsor mj41](https://github.com/sponsors/mj41) on GitHub:
> mj41 codes for attention food.

| The face, while a browser drives it | The QR screen: scan to pair, or type the code |
|---|---|
| ![Embody Mode: the robot's face with a speech bubble](screenshots/face.jpg) | ![Embody Mode: the QR screen with the pairing code](screenshots/qr-screen.jpg) |

Both are the robot's own screen (320x240), taken with the `screen_snapshot` command. The
QR screen's buttons are described under [Servers](#servers).

## On the robot

1. **Wi-Fi:** opening the app starts Wi-Fi (loading page) and connects to the default server in its list ([Servers](#servers)). Without a server set up (the release firmware before its setup over USB), it shows "Set up: sm.w42.eu" and contacts nothing, not even Wi-Fi.
2. **QR code:** the screen shows the server's one-time pairing URL as a QR code, next to its 8-character code.
3. **Face:** once a browser pairs, the face appears.
   - **Tap:** sends a `screen_tap` event with x/y.
   - **Long press:** reported as `screen_long_press {x, y}` (free for apps).
   - **QR button:** swipe up from the bottom: next to Home, **QR** opens the QR screen again (so another viewer can pair, or to switch servers; see [Servers](#servers)).
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

## Servers

The robot keeps a **server list** in NVS (namespace `embody`): the built-in server from Kconfig (always first; empty in the release build), its **managers'** apps (a Stackchan manager: set up over USB, changed later online by signed lists), and servers written **over USB** without a manager (below). **Apps never change the list:** adding, removing and the start app are the managers' (and the person's at the robot, on the QR screen). Tokens stay on the robot; the `servers` event (sent after registering and on every change) lists names, URLs, origins and whether there is a token.

- **QR screen:** swipe up from the bottom; next to Home, **QR** opens it, and while it is open that button reads **APP** and goes back to the app. The top row is `Pin  name n/m  Next ▶`; the bottom-right button reads **Back to app** on the connected server and **Connect** on another one.
  - **Next** browses the list; the connection stays, and another server's code comes only after Connect.
  - **Connect** switches to the shown server: the robot reconnects and, once connected, goes back to that app's face (its QR code: the QR button). The `n/m` counts only real apps (the release's empty "Set up" entry is skipped while there are others).
  - **Each app starts clean:** on every switch the last app's sprites and picture go, the face returns to neutral and the LEDs turn off (an app cannot clear them itself when its traffic is end-to-end encrypted).
  - **Which app runs:** after every connection the app's name shows as a card at the top for 2.5 s, then as a small label in the top-right corner (hidden on the QR screen and a blank screen), so apps that use the robot's own face (Raw data, Sbot) can be told apart.
  - **Pin** makes the shown one the **default** used at start (blue); tapping Pin on the default **unpins** it.
  - With no default, Embody Mode starts as a **chooser**: it contacts nothing until Connect.
- **From a browser:** `server_switch {server: url or name}` only (an app may suggest another app). It is **asked on the robot's screen first** ("Connect to …?", 60 s; events `server_asking`, then `server_refused` without a Yes), as a manager's new start app is ("Start with …?"); the QR screen's own buttons need no question.
- **On the QR screen,** the card shows the shown server's address, whether it is on the local network or the internet, and whether the connection is encrypted (TLS).

## Setup over USB

A computer writes servers, their tokens, autostart and Wi-Fi into the robot's settings over the USB cable ([usb_setup.h](usb_setup.h), started from `main()`, so it also works in the launcher): [sm.w42.eu](https://sm.w42.eu) in Chrome, or `s-w42-eu-usb` from [s-w42-eu-manager](https://github.com/mj41/s-w42-eu-manager#over-usb-from-a-terminal-s-w42-eu-usb). Lines on the USB serial port, each `@stackchan <JSON>`; the rest of the port's output (logs) stays as it was:

- `{"op":"hello"}` → the robot id, model, firmware version (M5Stack's version it is based on, `mj41`, the Embody Mode release: `1.5.1-mj41-v0.3.1`; `1.5.1-mj41-dev` in an own build; the robot reports the same to its servers), protocol 1, whether automation is built in, and the `original` and `previous` firmware records when it has them.
- `{"op":"provision","server":{"name","url","token"},"default":true,"servers":[{"name","url","token"}],"pin":"<url>","autostart":true,"wifi":{"ssid","password"},"original":{},"previous":{}}` → `{"ok":true,"applied":[…]}`. Every part is optional.
  - `server` is added to the list (origin "added"), and with `default` made the default. `servers` adds more at once; `pin` makes one of them the default. `autostart` applies only with automation built in.
  - **A new default server is confirmed on the robot:** its screen asks "Connect to …?" (Yes/No, 60 s). Without a Yes nothing is changed, and the answer is the error "not confirmed on the robot: nothing changed". Other servers, Wi-Fi and autostart need no tap.
  - `original`: the firmware the robot had before its first setup (identity and the SHA-256 of the backup the setup page saved), stored once in NVS (`embody/orig_fw`) and never replaced.
  - `previous`: the firmware it had before the latest setup with a backup (NVS `embody/prev_fw`), replaced each time.
  - With these two records, the setup page restores the original or the previous backup, after checking the file's SHA-256. Older backups go back with `esptool.py write_flash 0x0 <file>`.
  - `manager: {key, name, version, remote_apps, ask_pin}`: set up by a Stackchan manager (a home's own, or sm.w42.eu). A robot may have **several managers** ([managers.h](managers.h); NVS `embody/managers`), each managing only its own apps (origin `manager:<id>`, the id being the first 12 hex characters of the SHA-256 of its key); the QR screen shows them all. This setup's `servers` replace that manager's apps; servers no manager owns (earlier setups, tests) go; other managers' apps stay. `key` is the manager's public key (P-256): with `remote_apps` (default on) the robot accepts later app lists from it (`ManagedApps`, relayed by the server it is on: names the manager, signed with its key, for this robot id, a higher `version` than that manager's last), which replace that manager's apps; new apps come with their token, removed ones go; a new start app is asked on the screen when `ask_pin` (default on). After applying one it sends `AppsVersion {versions: "<id>:<version>,…"}` (never sealed, also with end-to-end encryption) and connects with the same string as the label `apps_ver`, so each manager knows the robot has its list. These choices change only over USB.
- `{"op":"status"}` → `{"ok":true,"embody":{server, name, state, status, qr, shown, default, question, servers:[{name, url, origin}]},"managers":[{id, name, version, remote_apps, ask_pin}]?}`: what Embody Mode does now (no tokens; `embody` null when it is not running), for tests and tools (`s-w42-eu-usb status`).
- `{"op":"pair"}` → `{"ok":true,"url"}`: the pairing link Embody Mode shows now (an error while it has none). The setup page opens it, so the computer that set the robot up is paired at once.
- `{"op":"restart"}` → a restart, into Embody Mode when automation is built in.
- **With automation built in**, a program on the computer can do what a person at the robot does (`s-w42-eu-usb screenshot`, `tap`, `launch`):
  - `{"op":"screenshot"}` → `{"ok":true,"width","height","jpeg":"<base64>","question":bool}`: the active screen as a JPEG (the robot's own questions are on the top layer and not in it; `question` says one is open).
  - `{"op":"tap","x","y","ms"?}`: a touch through a virtual pointer (100 ms, or longer for a long press). The first tap asks on the screen **"Let the computer on USB use the screen?"**; the Yes is kept in NVS (`embody/usb_ctrl`) until no USB host is there for a few seconds (the cable is unplugged). Taps are refused while one of the robot's own questions is open, and a press in progress is released when one opens: **only the person at the robot answers them.**
  - `{"op":"launch","app"}`: restart into a launcher app once (`"launcher"`: none); an unknown name answers with the list of apps.

So nobody needs to build firmware for a token: the **official release** (built by CI on `embody-v*` tags with `sdkconfig.defaults.release`; `./container.sh release` runs the same build locally) has no server and no token inside, and Embody Mode shows "Set up: sm.w42.eu" until it is set up. Having the robot on the cable is the proof of ownership, like scanning its QR code.

## Configuration

For your own builds, the Kconfig menu "Embody Mode" (`main/Kconfig.projbuild`) has these options. Set them in `firmware/sdkconfig.defaults.local` (gitignored, because it can hold a token); how, and why `sdkconfig` wins after the first build: [SETUP.md, C.2](SETUP.md#2-choose-how-the-robot-gets-its-server). The release build never reads that file (`sdkconfig.defaults.release`).

- `CONFIG_STACKCHAN_EMBODY_SERVER_URL`, `CONFIG_STACKCHAN_EMBODY_TOKEN`: the built-in server and its token, first in the server list. Empty by default and in the release build: the robot is then set up over USB.
- `CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S`: seconds without touch before the screen blanks. Default 60; 0 disables it.
- `CONFIG_STACKCHAN_EMBODY_CAR`: the optional TPBot car. Default on; it stays off until `car_enable` ([What it does](#what-it-does)).
- `CONFIG_STACKCHAN_EMBODY_ONLY`: the launcher installs only Embody Mode and SETUP, and ignores "start AI.AGENT on boot". Default off.
- `CONFIG_STACKCHAN_EMBODY_AUTOMATION`: lets a server, or an AI agent through it, run the robot without anyone touching it. **Default off** in your own builds; without it none of this is compiled in. **On in the release build**, so the setup over USB can turn autostart on. With it, the robot lists three more commands ([automation.h](automation.h), the launcher's `check_boot_app`):
  - `automation {"autostart": bool}`: open Embody Mode after every power-on or restart. Stored on the robot and **off until a server sets it**; the robot reports it as the `automation {autostart}` event after it registers.
  - `restart`: restart the robot, back into Embody Mode (fresh Wi-Fi and connection).
  - `launch {"app": "<name>"}`: restart into another launcher app once, by its launcher name (`AVATAR`, `AI.AGENT`, `DANCE`, `SETUP`…; `""` = stay in the launcher). An unknown name gives a `launch_unknown {app}` event. A server cannot reach the robot inside another app; a restart (or autostart after a power cycle) brings it back.
  - Home in Embody Mode still leaves the robot in the launcher: autostart applies only to a power-on or a restart, never to the way back from an app. AI.AGENT's own "start on boot" setting wins over autostart.
  - **Boot-loop guard** ([boot_guard.cpp](boot_guard.cpp)): crash restarts in a row (panic, watchdog) are counted in RTC memory; after 3, neither autostart nor a one-time launch opens an app, and the robot stays in the launcher. A normal boot, or a minute in Embody Mode, counts from zero again.

After adding sources or Kconfig options, run `idf.py reconfigure` before `idf.py build`. The robot ID is `stackchan-<factory MAC, lowercase>`.

## What it does

| Area | Details |
|---|---|
| Commands | `ping` (answered in `embody_client` with the queue time), `nod`, `shake`, `look`, `home`, `emotion`, `say`, `sticker`, `face`, `leds`, `brightness`, `volume`, `screensaver`, `standby`, `camera`, `mic`, `nfc`, and the `speaker` capability (audio arrives as binary `0x03`). `nfc` is listed only when the reader answers at startup. Pitch is clamped to 5–85° (M5Stack safe range) |
| Pictures | binary `0x10` JPEG, decoded with `jpeg_dec::decode_to_lvgl` and shown over the face |
| Camera | `StreamCaptures()`, then `image_to_jpeg` (quality 25), sent as binary `0x01` every 200 ms while on |
| Microphone | a FreeRTOS task reads all codec input channels at 24 kHz (2: channel 1 is the mic, channel 0 the speaker reference for echo cancellation, verified with Beep). Sent as binary `0x04` (rate, channel count, interleaved s16le) in 50 ms messages |
| IMU stream | `imu_stream {"on"}` (sent by the server while a browser has `?imu=1` open) makes the HAL IMU task sample at 100 Hz and buffer; the app sends binary `0x05` every 100 ms: count, then per sample uint32 ms + 9 float32 (accel m/s², gyro °/s, magnetic µT). The shake detector keeps 10 Hz |
| Speaker | binary `0x03` PCM is resampled to the codec's 24 kHz and queued (max ~3 s). A FreeRTOS task writes 20 ms chunks with `OutputData` and switches output off after 0.5 s of silence. The mouth moves while it plays, and the audio counts as use for the screensaver |
| LEDs | `leds` args: `left`/`right` fade a whole side (NeonLight); `pixels` sets up to 12 single LEDs (left 0–5, then right 6–11; `null` skips one); `effect` `rainbow`, `breathe`, `chase`, `blink` or `off`, with `color`, `speed` (0.2–5) and `seconds` (0 = until the next `leds`). Effects are drawn at 25 fps in the app loop, both sides mirrored; a timed effect restores the side colours when it ends |
| Light, proximity | `hal/drivers/LTR553` (LTR-553ALS in the CoreS3, 0x23), read in the app loop: proximity every 200 ms, light every 500 ms. **Auto-brightness** (on by default when the sensor answers) maps the smoothed lux (about 5 s to follow the room) on a log scale to 10–100 % backlight (≈55 % at 100 lux) and moves toward it calmly: at most 5 points every 2 s, and only when it is off by 6 or more. Readings during a touch (screen or head), while something is near the sensor or the proximity jumps (a hand over it), and for 10 s after, are left out: a hand shades the sensor and would dim the screen; `brightness {"value"}` ends it, `brightness {"auto": true}` turns it back on; paused during standby. **Approach:** `proximity_near` / `proximity_far` events against a slowly adapting baseline (+150 / +60); coming near wakes the screen like a touch, but not standby |
| Infrared | `hal/drivers/IrRemote`: RMT TX on G5 (IR LED on top of the robot; 38 kHz carrier, 50 %), RX on G10 (1 MHz ticks, 12 ms frame end, 512-symbol buffer via ping-pong). Received frames become `ir_received` with the raw mark/space timings and, when it decodes, NEC `address`/`command`; NEC repeats and short noise are skipped. `ir_send` sends NEC or raw timings, with `repeat` (NEC repeat codes every 108 ms, or the raw frame again after 40 ms: "hold") and `frames` (the whole code up to 5×, 108 ms apart, for weak links); the robot ignores its own echo for 150 ms unless `loopback` is set (self-test). The proximity sensor pauses while sending. The on-board LED is weak (~70 mA), so range is short |
| Power | Telemetry from the INA226 (`hal/drivers/INA226`, body battery: `body_battery_v`, `body_current_ma` + drain / − charge, `body_power_mw`, `body_shunt_uv`) and the AXP2101 (`core_battery_v`, `core_vbus_v`, `core_system_v`, `core_charge`, `core_charge_phase`, `pmic_temp_c`, `pmic_status1/2`). The AXP2101 IRQ status (0x49) is polled every 100 ms: `power_button {press}` (a short press wakes the screen), `usb_plugged`/`usb_unplugged`, `battery_inserted`/`battery_removed` |
| Hold | `hold {"seconds": 30..300}` keeps both servos powered at their current angle (auto torque release off), then restores release at rest; `0` ends it early, as do standby and closing the app. Events `hold_on`/`hold_off`, telemetry `hold_s` |
| Touch | the board's FT6336 driver reads both points (`hal_bridge::board_get_touch`); every 20 ms the app turns them into `touch_down` / `touch_up` `{id, x, y, ms}` events and, while `touch_stream` is on, binary `0x06` frames (sent every 100 ms) |
| Camera extras | `camera {on, size}`: `size` `"640x480"` streams at 640x480 (the sensor switches outside the LVGL lock, three frames to settle; JPEG quality 25 lowered in steps of 5 down to 10 while a frame does not fit one 64 KB message, that frame dropped), `"320x240"` (default) or off switches back; close and standby switch back too. `snapshot`: `StackChanCamera::SetSensorSize(640, 480)` (stream off, `VIDIOC_S_SENSOR_FMT`, new buffers), five frames to settle, JPEG at quality 60 stepping down until it fits 64 KB, binary `0x07`, back to 320x240. `camera_config {mirror, flip}`; `camera_reg {reg, value?}` raw GC0308 registers (event `camera_reg`) |
| Rotation, servo power | `rotate {velocity, seconds}`: yaw in wheel mode (`Servo::rotate`) for 1–30 s with auto torque release off. The first one asks on the robot's screen ("Turn the head round?", 30 s, event `rotate_asking`): Yes starts it (`rotate_confirmed`), No or no answer refuses (`rotate_refused`); the Yes holds until a USB cable is plugged in or out or the app closes. Torque is switched on first (it is off after a start). Stops when the head stops turning (the raw position moves under 2 steps per 200 ms for 0.6 s, after 0.6 s of start-up: held or blocked; the servo's load reads only the commanded drive in wheel mode, and its current reads 0) and on a cable plugged in (`rotate_stopped`); every rotation ends with `rotate_trace {samples}` (`ms:position:speed:current:load;` every 200 ms) for tuning, on any other head command, standby, close; refused with servo power off. `servo_power {on}` switches the PY32 servo supply |
| Power LED | `power_led {"mode"}`: the AXP2101 CHGLED (reg 0x69): off, blink (1 Hz), fast (4 Hz), on (the boot default), charging (the charger drives it) |
| NFC | `hal/drivers/ST25R3916`: a minimal ISO14443A reader ported from M5Stack's UiFlow2 driver (MIT). A FreeRTOS task (priority 1, core 1) polls twice a second with the RF field on only for the ~30 ms of each poll; the chip's IRQ pin isn't wired on StackChan. It reads 4- and 7-byte UIDs and, for Type 2 tags (NTAG, Ultralight), the first NDEF record (URI or text). It stops during standby. `nfc {"on"}` switches polling (on by default) |
| TPBot car (optional) | `car_ble.{h,cpp}`: a NimBLE central for an ELECFREAKS TPBot whose micro:bit runs [tpbot-ble](https://github.com/mj41/tpbot-ble). Compiled in with `CONFIG_STACKCHAN_EMBODY_CAR` (default y), but **off until `car_enable {"on": true}`** (kept in NVS, `embody/car_on`). Only then does BLE start and the robot register again with `car_drive {left, right}`, `car_stop`, `car_servo {port, angle}`, `car_headlights {color}`, `car_sonar {hz}`, `car_watchdog {ms}`, `car_board {board: v1/v2/both}` (NVS `car_board`, default v1) and the telemetry `car_connected`, `car_rssi_dbm`, `car_echo_us`, `car_line_l/r`, `car_btn_a/b`, `car_left/right`, `car_watchdog_stop`, `car_uptime_ms`, `car_i2c_errors`, `car_board`. These are the same names as `tpbot-bridge` uses (sbot readme, "Car capability"). It scans passively (60 ms in 200 ms, to leave the radio to Wi-Fi) for `TPB-*`, connects, subscribes to the state, and sends the state as telemetry at most every 50 ms. **Car commands take a fast path:** `car_*` commands (except `car_enable` and `car_board`) are handled in the WebSocket's receive task and written to BLE at once (`Client::onFastCommand`), not queued for the app loop, which can be busy for hundreds of ms with camera frames and drawing. Command to telemetry echo: 185–285 ms, with or without the camera streaming. Events `car_connected` / `car_disconnected {reason}` (with `car`, `addr`). The motors stop on every new link, when the server connection drops, and on close; the micro:bit's own watchdog stops them 500 ms after the last `car_drive` |
| Telemetry (every 2 s) | battery, charging, head yaw/pitch, Wi-Fi RSSI, free heap, uptime, brightness, volume, screensaver (0/1/2); sensors: `imu_ax_g`/`imu_ay_g`/`imu_az_g` and `imu_gyro_dps` (HAL snapshot, 10 Hz), `mag_x/y/z_ut`, `mag_raw_x/y/z`, `mag_rhall` (BMM150 through the BMI270's AUX interface), `yaw_`/`pitch_load_pct` and `_temp_c` plus `servo_voltage_v` (one servo feedback read per servo, in the app loop that owns the servo bus), `chip_temp_c` (ESP32-S3 sensor), `light_lux`, `proximity` (0–2047, only while on), `proximity_on`, `auto_brightness` (0/1) |
| Events | IMU shake, head-touch press (zones `z0`–`z2`, 0–3) / release (`ms`) and swipes, screen taps, screensaver on/off, standby, `nfc_tag {uid, type, atqa, sak, text}` and `nfc_removed {uid}` (after two missed polls) |

## Code

- `embody_client.{h,cpp}`: WebSocket client, using xiaozhi's `WebSocket` and ArduinoJson.
  - It runs from the app loop the way `hal/hal_ws_avatar.cpp` does. Incoming frames are queued by the socket task and handled in `update()`.
  - It reconnects with backoff from 1 s up to 30 s.
- `car_ble.{h,cpp}`: the optional TPBot car (NimBLE central). NimBLE callbacks run in its host task and only update mutex-protected state; the app loop polls it.
- `usb_setup.{h,cpp}`: [setup over USB](#setup-over-usb), a task on the USB serial port for the whole uptime.
- `confirm_dialog.{h,cpp}`: a Yes/No question on the robot's screen, above any app, with a timeout: for a new default server over USB and the first `rotate`.
- `boot_guard.cpp`: the crash-restart counter in RTC memory behind the boot-loop guard.
- `e2e.{h,cpp}`: per-server end-to-end encryption with the browsers enrolled through the QR code ([e2ee.md](https://github.com/mj41/home-w42-eu/blob/main/docs/e2ee.md)); keys in NVS `embody_e2e`, turned on per server with `server_e2e {server?, on}`. When it is on, plaintext commands other than stream switches (`camera`, `mic`, `imu_stream`, `touch_stream`, `light_stream`) and plaintext binary messages are refused.
- `asset_store.{h,cpp}`: files a server uploads in chunks (binary `0x11`: pictures, sounds), kept in the `userdata` FAT partition at `/user` (about 1.9 MB); they survive reboots and firmware updates.
- `sprite_layer.{h,cpp}`: stored pictures over the face (`sprite`), each with a position, scale, rotation, opacity and stacking order, moved by LVGL animations; taps on sprites marked `tap` are reported.
- `automation.h`: autostart, the one-time launch and the boot-loop guard's limit, shared by the launcher and Embody Mode.
- `app_embody_mode.{h,cpp}`: UI, commands, motion gestures, pictures, camera and microphone.
  - Commands and pictures are queued during `update()` and applied under the LVGL lock.
  - Events from HAL tasks, the NFC task and LVGL callbacks go through a mutex-protected queue. Event data can hold numbers and strings.

## Notes

- **Launcher order:** this app is launcher index 0 and AI.AGENT is 1. Other apps return via hardcoded `requestWarmReboot(<index>)` values, which are +1 compared with upstream.
- **No firmware from a server:** xiaozhi's OTA check is off (`CONFIG_STACKCHAN_XIAOZHI_OTA_CHECK`); see the trust design, [design.md](https://github.com/mj41/home-w42-eu/blob/main/docs/implementations/stackchan-trust.md).
- **Send limit:** xiaozhi's `WebSocket::Send` rejects messages over 65,535 bytes. That limits camera frames, not pictures, which the robot only receives.
- **Wi-Fi power save:** off while in use (2 minutes after the last command, or while the camera or microphone streams), on otherwise. The first command after a quiet spell can wait up to about 100 ms for the next beacon.
