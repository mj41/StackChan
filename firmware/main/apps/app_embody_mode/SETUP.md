# Setting up a Stackchan with Embody Mode

**Status:** 2026-10-03. Written while doing it on one M5Stack Stackchan robot (CoreS3), from a
Fedora laptop with Chrome and podman.

Embody Mode makes the robot a light client of a server you choose: you see through its
camera, hear through its microphone, speak through its speaker, read every sensor and
move it, from a browser. The robot can switch between servers and apps: the full
dashboard ([stackchan-server](https://github.com/mj41/stackchan-server)), a pet for kids
([stackchan-pet](https://github.com/mj41/stackchan-pet)), or a cockpit that also drives a small car
([sbot](https://github.com/mj41/sbot)). All of them are part of [home-w42-eu](https://github.com/mj41/home-w42-eu), a local first,
privacy first platform for a home.

There are three ways, from one click to your own firmware:

| Way | For | What it takes |
|---|---|---|
| [A. One click on chan.w42.eu](#a-one-click-on-chanw42eu) | everyone | Chrome or Edge, a GitHub or Google account |
| [B. Your own server](#b-your-own-server) | your home network, private things | Go 1.26 on a computer at home, Chrome or Edge |
| [C. Build the firmware yourself](#c-build-the-firmware-yourself) | developers, changes to the firmware | podman or docker (or ESP-IDF 5.5.4) |

All of them need an **M5Stack Stackchan** robot with its CoreS3 (ESP32-S3) and a **USB-C data
cable** (some cables only charge). Optional, for the car: a **micro:bit V2** and an
**ELECFREAKS TPBot** ([Drive a TPBot car](#optional-drive-a-tpbot-car)).

## A. One click on chan.w42.eu

1. Plug the robot into your computer: the USB-C port **on the robot's head** (the CoreS3).
2. Open [chan.w42.eu/setup](https://chan.w42.eu/setup) in **Chrome or Edge** (they have Web
   Serial; Firefox and Safari do not) and sign in with GitHub or Google.
3. Optional: tick **Start Embody Mode when the robot turns on** (off by default; otherwise
   open it from the launcher), and pick the **App** it starts with when the server offers more
   than its dashboard. In **Options**, your **Wi-Fi** name and password: they go to the robot
   over the cable, never to the server.
4. Press **Set up my robot** and pick the **USB JTAG/serial debug unit**.

The first time, the page saves a **backup of the robot's current firmware** to your computer
(about a minute; keep the file private, it holds the robot's old settings too). With it, Options →
Firmware → **Restore the original firmware** puts the robot back as it was. Then it installs the
latest Embody Mode firmware, adds the robot to your account with its own token, writes the server, the token and the Wi-Fi into the robot, and restarts it into
Embody Mode. It takes about two minutes. Then:

- The robot connects to chan.w42.eu. It is **private**: only you, signed in, see it. Open
  [chan.w42.eu](https://chan.w42.eu) on any device where you sign in, and it is there.
- Without Wi-Fi in step 3, the robot opens a hotspot first ([First start](#first-start)).
- [chan.w42.eu/robots](https://chan.w42.eu/robots) lists your robots: make one public, give
  it a new token, or remove it.

Everything streams through that server (a proof of concept, not reviewed), so keep private
things on your own server (B).

## B. Your own server

1. **Run a server** on a computer in the same network as the robot:

   ```bash
   git clone https://github.com/mj41/stackchan-server.git
   cd stackchan-server
   go run ./cmd/stackchan-server
   ```

   - The first start creates the **robot token** in `~/.config/stackchan-server/robot-token`.
     Keep it private.
   - It prints its address, e.g. `http://192.168.1.10:8765`. The robot and your phone must
     reach it: allow TCP port 8765 in the firewall if needed.
   - More options (HTTPS for the browser's microphone, state file, offering other servers):
     the server's readme, [Run](https://github.com/mj41/stackchan-server#run).

2. **Set the robot up** from the same computer: plug the robot in, open
   `http://localhost:8765/setup` in Chrome or Edge and press **Set up my robot**. The server
   fills in its address, its robot token and this computer's Wi-Fi, and installs the official
   firmware (it fetches the release from GitHub). Nothing to type.

   - **From another computer:** on the server's computer, press **Copy setup for another
     computer** on that page, and paste it on the other computer's setup page (Options → Server →
     My own server), e.g. [chan.w42.eu/setup](https://chan.w42.eu/setup) (signed in, to install
     the firmware there). It holds the robot
     token and the Wi-Fi password: keep it private.
   - The robot must reach the server's address: by default `ws://<LAN IP>:8765`; set another
     with `-public-url`.
   - Or, on a robot that already has Embody Mode, from a terminal:
     `go run ./cmd/stackchan-usb provision -url ws://192.168.1.10:8765 -token-file ~/.config/stackchan-server/robot-token -default -autostart`,
     then `go run ./cmd/stackchan-usb restart` (the server's readme,
     [Set a robot up over USB](https://github.com/mj41/stackchan-server#set-a-robot-up-over-usb)).

3. Pair: the robot shows a QR code; scan it with your phone ([First start](#first-start)).

## C. Build the firmware yourself

For changes to the firmware. There is one firmware source; the official release is that
source built with its release configuration, and everything else a robot needs (server,
token, Wi-Fi, autostart) is its settings, written over USB. Your own builds are for your own
robots: flash them from a terminal, as below. The setup page installs only the official
release.

### 1. Get the code

```bash
git clone -b embody-mj41 https://github.com/mj41/StackChan.git
git clone https://github.com/mj41/stackchan-server.git   # the server and stackchan-usb
```

### 2. Choose how the robot gets its server

- **Over USB after flashing** (simplest): nothing to configure. Set the robot up afterwards as
  in B, step 2 (the page with "Keep the robot's firmware", or `stackchan-usb`).
- **Built in:** create `StackChan/firmware/sdkconfig.defaults.local` (it is gitignored,
  because it holds the token):

  ```
  CONFIG_STACKCHAN_EMBODY_SERVER_URL="ws://192.168.1.10:8765"
  CONFIG_STACKCHAN_EMBODY_TOKEN="<the content of ~/.config/stackchan-server/robot-token>"
  ```

  For chan.w42.eu: `wss://chan.w42.eu` and the token that
  [chan.w42.eu/robots](https://chan.w42.eu/robots) shows once when you add your robot by its
  id (`stackchan-` and the robot's Wi-Fi MAC address in lowercase without colons, e.g.
  `stackchan-0a1b2c3d4e50`; `stackchan-usb hello` prints it, and the firmware logs it at start).

  **The first build turns these into `sdkconfig`, and after that `sdkconfig` wins.** To
  change them later, edit the same lines in `firmware/sdkconfig` too (or delete `sdkconfig`
  to start again from the defaults).

Optional, in the same file: `CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S` (seconds without touch
before the screen blanks, default 60, 0 = never), `CONFIG_STACKCHAN_EMBODY_ONLY` (the launcher
offers only Embody Mode and SETUP), `CONFIG_STACKCHAN_EMBODY_CAR` (the optional car, default on;
it stays off until you enable it), `CONFIG_STACKCHAN_EMBODY_AUTOMATION`
([Automation](#optional-let-a-server-or-an-ai-agent-run-the-robot)).

### 3. Build

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
- After adding a source file: `./container.sh idf reconfigure` first.

With a local ESP-IDF 5.5.4 instead (activate it first):

```bash
cd StackChan/firmware
python3 fetch_repos.py
idf.py build
```

ESP-IDF 6.x does not work yet (a missing `mqtt` component).

### 4. Flash

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

### 5. Set it up over USB

Without a built-in server (step 2), set the robot up as in B, step 2: the setup page with
**Keep the robot's firmware**, or `stackchan-usb`.

### The official release

Pushing an `embody-v*` tag makes GitHub build the release: `sdkconfig.defaults` plus
`sdkconfig.defaults.release` (no server, no token, automation on), never
`sdkconfig.defaults.local`. It attaches the parts, `manifest.json` with their SHA-256 and one
merged image to the release; servers give those to `/setup` with `-firmware-dir`.
`./container.sh release` runs the same script, `firmware/release.sh`, locally (into `firmware/build-release/dist`), to
check it before tagging.

## First start

1. After flashing by hand (C), the robot shows its **launcher**. **Embody Mode** is the first
   app: open it. (Set up over USB, the robot starts in Embody Mode by itself.)
2. **Wi-Fi:** if the robot knows no Wi-Fi yet, it opens a **hotspot** and shows its name
   and a configuration address on the screen. Connect your phone to that hotspot, open the
   address, and choose your Wi-Fi and its password. The robot then connects. (The SETUP
   app on the launcher has the other ways to configure Wi-Fi.)
3. **Pairing:** the robot shows a **QR code** and an 8-character code. Scan the QR code
   with your phone: the dashboard opens, paired with this robot. On a computer without a
   camera, open the server's address and type the code.
   - Open the dashboard with the same host as in the QR code (the address, not
     `localhost`): the pairing belongs to that host.
   - On chan.w42.eu your private robot needs no pairing: sign in and it is there.
4. You see the robot's face on its screen, and its status, camera and controls in the
   browser.

On the robot:

- **Swipe up** from the bottom: **Home** closes Embody Mode (the robot restarts into the
  launcher); **QR** shows the pairing screen again (to pair another phone, or to switch
  servers).
- A **double tap** blanks the screen; a touch wakes it.
- A red **LIVE** badge shows whenever the camera or the microphone is streaming.

## More servers and apps

The robot keeps a **list of servers** and can switch between them:

- **On the robot:** open the QR screen (swipe up, **QR**). **Next** shows the next server,
  **Connect** switches to it, **Pin** makes the shown one the default at start.
- **Over USB:** set the robot up again with another server (A or B); it is added to the list
  and made the default.
- **Offered by a server:** run `stackchan-server` with
  `-offer Name=ws://host:port,<token file>` and its robots add that server to their list.
- **From the dashboard:** the "Servers" section adds, removes and switches servers.

Apps that run as servers the robot can switch to:

| App | What | Repo |
|---|---|---|
| Dashboard | everything the robot has: camera, mic, speaker, every sensor, IR, NFC, files | [stackchan-server](https://github.com/mj41/stackchan-server) |
| Pet | a Tamagotchi for kids, fed with NFC cards, with games and routines | [stackchan-pet](https://github.com/mj41/stackchan-pet) |
| Cockpit | the robot's camera with a joystick for a TPBot car, head pad and lights, a safety stop | [sbot](https://github.com/mj41/sbot) |

## Optional: drive a TPBot car

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
3. **Run [sbot](https://github.com/mj41/sbot#run)** and switch the robot to it ([More servers and apps](#more-servers-and-apps)). In sbot's page, open **More** and
   turn the car on. The robot registers again with the car commands and connects to the
   car within seconds.
4. Drive with the joystick or WASD while watching through the robot's camera. The car
   stops before obstacles (sonar, 10 cm by default), and on its own 500 ms after the last
   command if the connection is lost.

## Optional: let a server or an AI agent run the robot

The robot then accepts three more commands from its server: `automation {"autostart": true}`
(open Embody Mode after every power-on or restart), `restart`, and `launch {"app": "AVATAR"}`
(restart into another app once). Home in Embody Mode still leaves the robot in the launcher.
Details: the [Embody Mode README](README.md#configuration).

- **Release firmware (A, B):** built in. The setup over USB turns autostart on, so the robot
  comes back into Embody Mode after a power cycle.
- **Your own build (C):** off by default. Add `CONFIG_STACKCHAN_EMBODY_AUTOMATION=y` to
  `sdkconfig` (and to `sdkconfig.defaults.local`), build and flash.

Anything that can send commands through the server can then restart the robot and keep it in
Embody Mode, so use it only with a server you trust.

## Troubleshooting

| Problem | Try |
|---|---|
| The setup page offers no device | a data cable (not a charging-only one), in the head's USB-C port; close other programs on the port (`idf.py monitor`, `stackchan-usb`); on Linux, the `dialout` group |
| The setup page: "The robot did not answer" | press the robot's reset button and try again; with "Keep the robot's firmware", the robot needs firmware with setup over USB (2026-10 or newer) |
| Flashing stops halfway | put the robot into flashing mode by hand (hold the reset button until the green LED lights up) and press the button again |
| The robot stays on "Connecting" | the server URL and token (for built-in ones: in `sdkconfig`, not only in `sdkconfig.defaults.local`), the firewall, the same network |
| The robot keeps restarting | after 3 crashes in a row it stops opening Embody Mode by itself and stays in the launcher; report the crash (`idf.py monitor` shows it) and restore the original firmware or install again |
| Embody Mode says "Set up: chan.w42.eu/setup" | release firmware with no server yet: set it up over USB (A or B) |
| Pairing says the code is invalid | scan again: codes are one-time and expire after 5 minutes |
| The dashboard shows no robot after pairing | open it with the same host as in the QR code |
| chan.w42.eu says "this robot is private" | sign in with the account that set it up, or make it public on [Your robots](https://chan.w42.eu/robots) |
| The browser does not offer the microphone | browsers give it only to HTTPS pages: run the server with `-tls-listen :8766` and open `https://<address>:8766` |
| After flashing by hand, nothing connects | the robot starts in the launcher: open Embody Mode |
| Flashing from the container: "Permission denied" or "Write timeout" | `container.sh` passes the port under its own name and without SELinux labels for that run; with your own `podman run`, do the same (`--device /dev/ttyACM1:/dev/ttyACM1 --group-add keep-groups --security-opt label=disable`) |
| A changed picture or asset does not show | delete `build*/generated_assets.bin` and build again |

## Security, as it is today

- **Tokens.** Set up over USB, the token lives in the robot's settings, not in the firmware
  image. On chan.w42.eu every robot has its own token, which works only for that robot id. On
  your own server, robots share the server's one robot token. Anyone who reads the robot's
  flash gets its token. Per-robot keys and owner-signed permissions are designed (the trust
  design, [design.md](https://github.com/mj41/stackchan-mj/blob/main/docs/design.md) in
  stackchan-mj) but not built yet.
- **Setup over USB trusts the cable:** anything on the robot's USB port can change its
  server. Having the robot in hand is the proof of ownership, as with its QR code.
- **Use your own server on your own network** for anything private. Camera and
  microphone stream only while a paired browser watches or listens, and the robot shows
  it.
- The car's micro:bit accepts only the Bluetooth addresses on its allowlist. Addresses
  can be spoofed; bonding is still to do.
