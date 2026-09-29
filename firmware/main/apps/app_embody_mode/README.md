# Embody Mode

Launcher app (first icon) that lets you control this Stack-chan from a browser through [stackchan-server](https://github.com/mj41/stackchan-server). The server's readme defines the protocol, and the design lives in `../stackchan-mj/docs/design.md`.

## On the robot

1. **Wi-Fi:** opening the app starts Wi-Fi (loading page) and connects to `CONFIG_STACKCHAN_EMBODY_SERVER_URL`.
2. **QR code:** the screen shows the server's one-time pairing URL as a QR code, next to its 8-character code.
3. **Face:** once a browser pairs, the face appears.
   - **Tap:** sends a `screen_tap` event with x/y.
   - **Long press:** toggles the QR code, so another viewer can pair.
4. **LIVE badge:** a red **LIVE** badge shows while the camera or microphone streams.
5. **Screensaver:** after `CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S` (default 60 s, 0 = never) without touch, the screen goes blank (black). It sends `screensaver_on`, and on wake `screensaver_off`. A touch brings back the previous view (face, picture or QR). So does a command that changes the screen (emotion, say, sticker, face, a new picture) and the first pairing. The LIVE badge stays visible.
6. **Closing:** swiping up closes the app and warm-reboots to the launcher. Wi-Fi can't be stopped cleanly, which is the same reason AVATAR reboots.

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
| Commands | `ping` (answered in `embody_client` with the queue time), `nod`, `shake`, `look`, `home`, `emotion`, `say`, `sticker`, `face`, `leds`, `brightness`, `volume`, `camera`, `mic` |
| Pictures | binary `0x10` JPEG, decoded with `jpeg_dec::decode_to_lvgl` and shown over the face |
| Camera | `StreamCaptures()`, then `image_to_jpeg` (quality 25), sent as binary `0x01` every 200 ms while on |
| Microphone | a FreeRTOS task reads the audio codec at 24 kHz, channel 1 (as in the SETUP mic test). Sent as binary `0x02` in 50 ms messages |
| Telemetry (every 2 s) | battery, charging, head yaw/pitch, Wi-Fi RSSI, free heap, uptime, brightness, volume, screensaver (0/1) |
| Events | IMU shake, head-touch press and swipes, screen taps, screensaver on/off |

## Code

- `embody_client.{h,cpp}`: WebSocket client, using xiaozhi's `WebSocket` and ArduinoJson.
  - It runs from the app loop the way `hal/hal_ws_avatar.cpp` does. Incoming frames are queued by the socket task and handled in `update()`.
  - It reconnects with backoff from 1 s up to 30 s.
- `app_embody_mode.{h,cpp}`: UI, commands, motion gestures, pictures, camera and microphone.
  - Commands and pictures are queued during `update()` and applied under the LVGL lock.
  - Events from HAL tasks and LVGL callbacks go through a mutex-protected queue.

## Notes

- **Launcher order:** this app is launcher index 0 and AI.AGENT is 1. Other apps return via hardcoded `requestWarmReboot(<index>)` values, which are +1 compared with upstream.
- **Update hole closed:** `patches/xiaozhi-esp32.patch` disables xiaozhi's OTA server check (`CONFIG_STACKCHAN_XIAOZHI_OTA_CHECK`, off). Firmware is never installed from a server.
- **Send limit:** xiaozhi's `WebSocket::Send` rejects messages over 65,535 bytes. That limits camera frames, not pictures, which the robot only receives.
