# Setting up a Stack-chan with Embody Mode

**Status:** 2026-10-02. Written while doing it on one M5Stack Stack-chan (CoreS3), from a
Fedora laptop with podman; the container build (step 4) and flashing from the container
(step 5) were tested there.

Embody Mode makes the robot a light client of a server you choose: you see through its
camera, hear through its microphone, speak through its speaker, read every sensor and
move it, from a browser. The robot can switch between servers and apps: the full
dashboard ([stackchan-server](https://github.com/mj41/stackchan-server)), a pet for kids
([stackchan-pet](https://github.com/mj41/stackchan-pet)), or a cockpit that also drives a small car
([sbot](https://github.com/mj41/sbot)). All of them are part of [home-w42-eu](https://github.com/mj41/home-w42-eu), a local first,
privacy first platform for a home.

## What you need

- An **M5Stack Stack-chan** with its CoreS3 (ESP32-S3), and a **USB-C data cable** (some
  cables only charge).
- A computer with **podman** or **docker** (or a local ESP-IDF 5.5.4, see step 4).
- **Go 1.25 or newer** for the server, on a machine in the same network as the robot.
- Optional, for the car: a **micro:bit V2** and an **ELECFREAKS TPBot** (step 8).

## 1. Get the code

```bash
git clone -b embody-mj41 https://github.com/mj41/StackChan.git
git clone https://github.com/mj41/stackchan-server.git
```

## 2. Run a server

```bash
cd stackchan-server
go run ./cmd/stackchan-server
```

- The first start creates the **robot token** in `~/.config/stackchan-server/robot-token`.
  The robot needs it (step 3); keep it private.
- It prints its address, e.g. `http://192.168.1.10:8765`. The robot and your phone must
  reach it: allow TCP port 8765 in the firewall if needed.
- More options (HTTPS for the browser's microphone, state file, offering other servers):
  the server's readme, [Run](https://github.com/mj41/stackchan-server#run).

## 3. Configure the firmware

Create `StackChan/firmware/sdkconfig.defaults.local` (it is gitignored, because it holds
the token):

```
CONFIG_STACKCHAN_EMBODY_SERVER_URL="ws://192.168.1.10:8765"
CONFIG_STACKCHAN_EMBODY_TOKEN="<the content of ~/.config/stackchan-server/robot-token>"
```

- Use the address the server printed, with `ws://` (or `wss://` for a server behind
  HTTPS).
- **The first build turns these into `sdkconfig`, and after that `sdkconfig` wins.** To
  change them later, edit the same lines in `firmware/sdkconfig` too (or delete
  `sdkconfig` to start again from the defaults).
- Optional: `CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S` (seconds without touch before the
  screen blanks, default 60, 0 = never), `CONFIG_STACKCHAN_EMBODY_ONLY` (the launcher
  offers only Embody Mode and SETUP), `CONFIG_STACKCHAN_EMBODY_CAR` (the optional car,
  default on; it stays off until you enable it, step 8).

## 4. Build

In a container (recommended: the same pinned ESP-IDF image for everyone):

```bash
cd StackChan/firmware
./container.sh build
```

- The first run pulls Espressif's `espressif/idf:v5.5.4` image (**8.6 GB** on disk) and
  fetches the dependencies (`fetch_repos.py`): about 13 minutes here, mostly the download.
  Later builds take seconds when nothing changed, and a minute or two after a change.
- The build goes to `firmware/build-container/`.
- `ENGINE=docker ./container.sh build` uses docker instead of podman.

With a local ESP-IDF 5.5.4 instead (activate it first):

```bash
cd StackChan/firmware
python3 fetch_repos.py
idf.py build
```

ESP-IDF 6.x does not work yet (a missing `mqtt` component).

## 5. Flash

Plug the cable into the USB-C port **on the robot's head** (the CoreS3), then:

```bash
./container.sh flash          # or: idf.py -p /dev/ttyACM0 flash
```

- The CoreS3's serial port is found by itself (`/dev/serial/by-id/usb-Espressif_USB_JTAG…`);
  pass another as `./container.sh flash /dev/ttyACM1`.
- On Linux your user needs access to serial ports (on Fedora and Debian: the `dialout`
  group, then log in again).
- If the robot does not enter flashing mode by itself, put it there by hand (on the
  CoreS3: hold its reset button until the green LED lights up) and flash again.
- **Opening the serial port restarts the robot.** That is normal for the CoreS3.

## 6. First start

1. After flashing, the robot shows its **launcher**. **Embody Mode** is the first app:
   open it.
2. **Wi-Fi:** if the robot knows no Wi-Fi yet, it opens a **hotspot** and shows its name
   and a configuration address on the screen. Connect your phone to that hotspot, open the
   address, and choose your Wi-Fi and its password. The robot then connects. (The SETUP
   app on the launcher has the other ways to configure Wi-Fi.)
3. **Pairing:** the robot shows a **QR code** and an 8-character code. Scan the QR code
   with your phone: the dashboard opens, paired with this robot. On a computer without a
   camera, open the server's address and type the code.
   - Open the dashboard with the same host as in the QR code (the address, not
     `localhost`): the pairing belongs to that host.
4. You see the robot's face on its screen, and its status, camera and controls in the
   browser.

On the robot:

- **Swipe up** from the bottom: **Home** closes Embody Mode (the robot restarts into the
  launcher); **QR** shows the pairing screen again (to pair another phone, or to switch
  servers).
- A **double tap** blanks the screen; a touch wakes it.
- A red **LIVE** badge shows whenever the camera or the microphone is streaming.

## 7. More servers and apps

The robot keeps a **list of servers** and can switch between them:

- **On the robot:** open the QR screen (swipe up, **QR**). **Next** shows the next server,
  **Connect** switches to it, **Pin** makes the shown one the default at start.
- **Offered by a server:** run `stackchan-server` with
  `-offer Name=ws://host:port,<token file>` and its robots add that server to their list.
- **From the dashboard:** the "Servers" section adds, removes and switches servers.

Apps that run as servers the robot can switch to:

| App | What | Repo |
|---|---|---|
| Dashboard | everything the robot has: camera, mic, speaker, every sensor, IR, NFC, files | [stackchan-server](https://github.com/mj41/stackchan-server) |
| Pet | a Tamagotchi for kids, fed with NFC cards, with games and routines | [stackchan-pet](https://github.com/mj41/stackchan-pet) |
| Cockpit | the robot's camera with a joystick for a TPBot car, head pad and lights, a safety stop | [sbot](https://github.com/mj41/sbot) |

## 8. Optional: drive a TPBot car

The robot can drive an ELECFREAKS TPBot car whose micro:bit runs [tpbot-ble](https://github.com/mj41/tpbot-ble), over
Bluetooth. Most people do not have one: the firmware keeps Bluetooth off until you
enable the car.

1. **Flash the micro:bit** with [tpbot-ble](https://github.com/mj41/tpbot-ble) (needs TinyGo 0.41 or newer): see its readme.
   Put the robot's Bluetooth address in `~/.config/tpbot-ble/allow` first, so only your
   robot may drive the car. The robot's Bluetooth address is its Wi-Fi MAC plus 2 in the
   last byte (the robot id `stackchan-0a1b2c3d4e50` gives `0A:1B:2C:3D:4E:52`), or flash
   with an empty list and read the address from the micro:bit's serial output.
2. **Switch the TPBot on with one press** of its power button (its LEDs breathe green).
   A second press starts its own line-following mode (rainbow LEDs), which drives by
   itself.
3. **Run [sbot](https://github.com/mj41/sbot#run)** and switch the robot to it (step 7). In sbot's page, open **More** and
   turn the car on. The robot registers again with the car commands and connects to the
   car within seconds.
4. Drive with the joystick or WASD while watching through the robot's camera. The car
   stops before obstacles (sonar, 10 cm by default), and on its own 500 ms after the last
   command if the connection is lost.

## Troubleshooting

| Problem | Try |
|---|---|
| The robot stays on "Connecting" | the server URL and token in `sdkconfig` (not only in `sdkconfig.defaults.local`), the firewall, the same network |
| Pairing says the code is invalid | scan again: codes are one-time and expire after 5 minutes |
| The dashboard shows no robot after pairing | open it with the same host as in the QR code |
| The browser does not offer the microphone | browsers give it only to HTTPS pages: run the server with `-tls-listen :8766` and open `https://<address>:8766` |
| After flashing, nothing connects | the robot starts in the launcher: open Embody Mode |
| Flashing from the container: "Permission denied" or "Write timeout" | `container.sh` passes the port under its own name and without SELinux labels for that run; with your own `podman run`, do the same (`--device /dev/ttyACM1:/dev/ttyACM1 --group-add keep-groups --security-opt label=disable`) |
| A changed picture or asset does not show | delete `build*/generated_assets.bin` and build again |

## Security, as it is today

- **One shared token** per server, compiled into the firmware. Anyone who reads the
  robot's flash gets it. Per-robot keys and owner-signed permissions are designed (the
  trust design, [design.md](https://github.com/mj41/stackchan-mj/blob/main/docs/design.md) in stackchan-mj) but not built yet.
- **Use your own server on your own network** for anything private. Camera and
  microphone stream only while a paired browser watches or listens, and the robot shows
  it.
- The car's micro:bit accepts only the Bluetooth addresses on its allowlist. Addresses
  can be spoofed; bonding is still to do.
