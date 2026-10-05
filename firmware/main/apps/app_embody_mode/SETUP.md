# Setting up a Stackchan with Embody Mode

Embody Mode makes the robot a light client of a server you choose: you see through its
camera, hear through its microphone, speak through its speaker, read every sensor and
move it, from a browser. The robot can switch between servers and apps: the full
dashboard ([s-w42-eu-raw](https://github.com/mj41/s-w42-eu-raw)), a pet for kids
([s-w42-eu-pet](https://github.com/mj41/s-w42-eu-pet)), or a cockpit that also drives a small car
([s-w42-eu-sbot](https://github.com/mj41/s-w42-eu-sbot)). All of them are part of [home-w42-eu](https://github.com/mj41/home-w42-eu), a local first,
privacy first platform for a home.

There are three ways, from one click to your own firmware:

| Way | For | What it takes |
|---|---|---|
| [A. One click on sm.w42.eu](#a-one-click-on-smw42eu) | everyone | Chrome or Edge, a GitHub or Google account |
| [B. Your own server](#b-your-own-server) | your home network, private things | Go 1.26 on a computer at home, Chrome or Edge |
| [C. Build the firmware yourself](#c-build-the-firmware-yourself) | developers, changes to the firmware | podman or docker (or ESP-IDF 5.5.4) |

All of them need an **M5Stack Stackchan** robot with its CoreS3 (ESP32-S3) and a **USB-C data
cable** (some cables only charge). Optional, for the car: a **micro:bit V2** and an
**ELECFREAKS TPBot** ([Drive a TPBot car](#optional-drive-a-tpbot-car)).

## A. One click on sm.w42.eu

[sm.w42.eu](https://sm.w42.eu) is the Stackchan manager: it sets your robot up for the apps you
approve, each on a host of its own ([s.w42.eu](https://s.w42.eu) lists them): the raw dashboard
[raw.sa.w42.eu](https://raw.sa.w42.eu) and the pet [pet.sa.w42.eu](https://pet.sa.w42.eu).

1. Plug the robot into your computer: the USB-C port **on the robot's head** (the CoreS3).
2. Open [sm.w42.eu](https://sm.w42.eu) in **Chrome or Edge** (they have Web Serial; Firefox
   and Safari do not) and sign in with GitHub or Google.
3. **Set up your robot:** press **Connect** and pick the **USB JTAG/serial debug unit**. The page
   shows what it found on the robot.
4. The choices are filled in; change what you want:
   - **Apps:** all of them; ★ marks the one it starts with, × removes one.
   - **Let sm.w42.eu change this robot's apps** (on): later you change its apps on the page, from
     anywhere; the robot accepts only app lists signed by sm.w42.eu. Off: only over USB.
   - **The robot asks on its screen before its start app changes** (on).
   - **Wi-Fi and more:** your Wi-Fi name and password (they go to the robot over the cable,
     never to the server), and **Start Embody Mode when the robot turns on** (off by default;
     otherwise open it from the launcher).
5. Press **Set up**.

With **Back up the current firmware first** (on by default), the page first saves the robot's
current firmware to your computer, where the browser keeps downloads (e.g. `~/Downloads`; about
two minutes; keep the file private, it holds the robot's old settings too). The first backup is
the robot's original. The robot's **⋯** menu → **Restore an earlier firmware** puts back the original
or the previous backup (the page checks the file against the robot's record of both); older
backups only from a terminal (`esptool.py write_flash 0x0 <file>`). Then it installs the
latest Embody Mode firmware, adds the robot to your account, writes every app you ticked with a
token of its own, and the Wi-Fi, into the robot, and restarts it into Embody Mode; the robot
asks on its screen before the app it starts with is set (tap **Yes**). It takes about two
minutes. Then:

- The robot connects to its start app; Next and Connect on its QR screen switch between your
  apps. It is **private**: only you, signed in, see it. Open the app (e.g.
  [raw.sa.w42.eu](https://raw.sa.w42.eu)) on any device where you sign in, and it is there.
- Without Wi-Fi in step 4, the robot opens a hotspot first ([First start](#first-start)).
- [sm.w42.eu](https://sm.w42.eu) shows your robot: **Open** its start app, its status (online,
  on which app), its apps (★ start, × remove, + add: they reach the robot within a few minutes,
  "✓ On the robot"), a name for it, and in **⋯**: public or private (public: its QR code pairs
  anyone), history, remove (its tokens stop working).
- Plugged into the computer again, the robot gets a USB strip on its card: update the firmware,
  write its apps, change the two permissions.

Everything streams through those servers (a proof of concept, not reviewed), so keep private
things on your own servers (B).

## B. Your own servers

At home, a manager sets your robots up for your own app servers, with no sign-in: it works only
from the computer it runs on.

1. **Run the apps** on a computer in the same network as the robot, e.g. the raw dashboard:

   ```bash
   git clone https://github.com/mj41/s-w42-eu-raw.git
   cd s-w42-eu-raw
   go run ./cmd/s-w42-eu-raw
   ```

   - The first start creates the **robot token** in `~/.config/stackchan-server/robot-token`.
     Keep it private.
   - It prints its address, e.g. `http://192.168.1.10:8765`. The robot and your phone must
     reach it: allow TCP port 8765 in the firewall if needed.
   - More options (HTTPS for the browser's microphone, state file, a manager's tokens): the
     server's readme, [Run](https://github.com/mj41/s-w42-eu-raw#run). The pet
     ([s-w42-eu-pet](https://github.com/mj41/s-w42-eu-pet)) runs the same way, on port 8770.

2. **Run the manager** on the same computer, with your apps in its catalog
   `~/.config/s-w42-eu-manager/apps.json` (its readme,
   [The app catalog](https://github.com/mj41/s-w42-eu-manager#the-app-catalog)), e.g. with the
   shared robot token:

   ```json
   [{"id": "raw", "name": "Raw data", "url": "ws://192.168.1.10:8765", "token_file": "/home/me/.config/stackchan-server/robot-token"}]
   ```

   ```bash
   git clone https://github.com/mj41/s-w42-eu-manager.git
   cd s-w42-eu-manager
   go run ./cmd/s-w42-eu-manager
   ```

3. **Set the robot up:** plug it in, open `http://localhost:8790` in Chrome or Edge, press
   **Connect** and **Set up** (the apps are ticked). The manager fills in this computer's Wi-Fi
   and installs the official firmware (it fetches the release from GitHub). Nothing to type.

   - The robot must reach each app's address (`url` in the catalog).
   - Or, on a robot that already has Embody Mode, from a terminal (in s-w42-eu-manager):
     `go run ./cmd/s-w42-eu-usb provision -url ws://192.168.1.10:8765 -token-file ~/.config/stackchan-server/robot-token -default -autostart`,
     then `go run ./cmd/s-w42-eu-usb restart` (the manager's readme,
     [Over USB from a terminal](https://github.com/mj41/s-w42-eu-manager#over-usb-from-a-terminal-s-w42-eu-usb)).

4. Pair: the robot shows a QR code; scan it with your phone ([First start](#first-start)).

## C. Build the firmware yourself

For changes to the firmware. There is one firmware source; the official release is that
source built with its release configuration, and everything else a robot needs (server,
token, Wi-Fi, autostart) is its settings, written over USB. Your own builds are for your own
robots: flash them from a terminal, as below. The manager's page installs only the official
release.

### 1. Get the code

```bash
git clone -b embody-mj41 https://github.com/mj41/StackChan.git
git clone https://github.com/mj41/s-w42-eu-manager.git   # the manager and s-w42-eu-usb
```

### 2. Choose how the robot gets its server

- **Over USB after flashing** (simplest): nothing to configure. Set the robot up afterwards as
  in A or B (the page with **Install the latest official Embody Mode** unticked, or `s-w42-eu-usb`).
- **Built in:** create `StackChan/firmware/sdkconfig.defaults.local` (it is gitignored,
  because it holds the token):

  ```
  CONFIG_STACKCHAN_EMBODY_SERVER_URL="ws://192.168.1.10:8765"
  CONFIG_STACKCHAN_EMBODY_TOKEN="<the content of ~/.config/stackchan-server/robot-token>"
  ```

  The robot's id is `stackchan-` and its Wi-Fi MAC address in lowercase without colons, e.g.
  `stackchan-0a1b2c3d4e50` (`s-w42-eu-usb hello` prints it, and the firmware logs it at start).
  The w42.eu apps take only tokens from sm.w42.eu: set such a robot up there (A).

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

Without a built-in server (step 2), set the robot up as in A or B: the page with **Install the
latest official Embody Mode** unticked, or `s-w42-eu-usb`.

### The official release

Pushing an `embody-v*` tag makes GitHub build the release: `sdkconfig.defaults` plus
`sdkconfig.defaults.release` (no server, no token, automation on), never
`sdkconfig.defaults.local`. It attaches the parts, `manifest.json` with their SHA-256 and one
merged image to the release. A manager's page installs the latest GitHub release by default
(`-firmware-release latest`); `-firmware-dir` serves those files from a directory instead.
`./container.sh release` runs the same script, `firmware/release.sh`, locally (into `firmware/build-release/dist`), to
check it before tagging.

The release is reproducible: CI, a laptop and a cloud rebuild give the same bytes. The signed
hashes are in [mj41cz-approved](https://gitlab.com/mj41cz/mj41cz-approved), the rebuild logs in
[mj41cz-rebuilds](https://gitlab.com/mj41cz/mj41cz-rebuilds); details in the
[device setup design](https://github.com/mj41/home-w42-eu/blob/main/docs/device-setup.md), section 6.

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
   - On the w42.eu apps your private robot needs no pairing: sign in and it is there.
4. You see the robot's face on its screen, and its status, camera and controls in the
   browser.

On the robot:

- **Swipe up** from the bottom: **Home** closes Embody Mode (the robot restarts into the
  launcher); **QR** shows the pairing screen again (to pair another phone, or to switch
  servers).
- A **double tap** blanks the screen; a touch wakes it.
- A red **LIVE** badge shows whenever the camera or the microphone is streaming.

## More servers and apps

The robot keeps a **list of servers** (its apps), set by its managers, and can switch between
them:

- **On the robot:** open the QR screen (swipe up, **QR**). **Next** shows the next server,
  **Connect** switches to it, **Pin** makes the shown one the default at start.
- **More apps** come from the manager's catalog: add an app to your manager's
  `~/.config/s-w42-eu-manager/apps.json` and set the robot up again (B), or change its apps
  online on the manager's page (if you allowed that at setup).
- **Several managers:** each manages only its own apps. A robot set up on sm.w42.eu (A) and on
  your own manager (B) keeps both, and the QR screen shows them all.
- **From an app:** an app may suggest switching to another of the robot's apps (in the
  dashboard: **Apps**, **Switch**); the robot asks on its screen.

Apps that run as servers the robot can switch to:

| App | What | Repo |
|---|---|---|
| Dashboard | everything the robot has: camera, mic, speaker, every sensor, IR, NFC, files | [s-w42-eu-raw](https://github.com/mj41/s-w42-eu-raw) |
| Pet | a Tamagotchi for kids, fed with NFC cards, with games and routines | [s-w42-eu-pet](https://github.com/mj41/s-w42-eu-pet) |
| Cockpit | the robot's camera with a joystick for a TPBot car, head pad and lights, a safety stop | [s-w42-eu-sbot](https://github.com/mj41/s-w42-eu-sbot) |

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
3. **Run [s-w42-eu-sbot](https://github.com/mj41/s-w42-eu-sbot#run)** and switch the robot to it ([More servers and apps](#more-servers-and-apps)). In sbot's page, open **More** and
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

- **Release firmware (A, B):** built in. The manager's page can turn autostart on (**Start Embody
  Mode when the robot turns on**), so the robot comes back into Embody Mode after a power cycle.
- **Your own build (C):** off by default. Add `CONFIG_STACKCHAN_EMBODY_AUTOMATION=y` to
  `sdkconfig` (and to `sdkconfig.defaults.local`), build and flash.

Anything that can send commands through the server can then restart the robot and keep it in
Embody Mode, so use it only with a server you trust.

## Troubleshooting

| Problem | Try |
|---|---|
| The page offers no device | a data cable (not a charging-only one), in the head's USB-C port; close other programs on the port (`idf.py monitor`, `s-w42-eu-usb`); on Linux, the `dialout` group |
| The page: "The robot did not answer" | press the robot's reset button and try again; without installing the firmware, the robot needs firmware with setup over USB (embody-v0.1.0 or newer) |
| Flashing stops halfway | put the robot into flashing mode by hand (hold the reset button until the green LED lights up) and press the button again |
| The robot stays on "Connecting" | the server URL and token (for built-in ones: in `sdkconfig`, not only in `sdkconfig.defaults.local`), the firewall, the same network |
| The robot keeps restarting | after 3 crashes in a row it stops opening Embody Mode by itself and stays in the launcher; report the crash (`idf.py monitor` shows it) and restore the original firmware or install again |
| Embody Mode says "Set up: sm.w42.eu" | release firmware with no server yet: set it up over USB (A or B) |
| Pairing says the code is invalid | scan again: codes are one-time and expire after 5 minutes |
| The dashboard shows no robot after pairing | open it with the same host as in the QR code |
| An app says "this robot is private" | sign in with the account that set it up, or make it public on [sm.w42.eu](https://sm.w42.eu) |
| The browser does not offer the microphone | browsers give it only to HTTPS pages: run the server with `-tls-listen :8766` and open `https://<address>:8766` |
| After flashing by hand, nothing connects | the robot starts in the launcher: open Embody Mode |
| Flashing from the container: "Permission denied" or "Write timeout" | `container.sh` passes the port under its own name and without SELinux labels for that run; with your own `podman run`, do the same (`--device /dev/ttyACM1:/dev/ttyACM1 --group-add keep-groups --security-opt label=disable`) |
| A changed picture or asset does not show | delete `build*/generated_assets.bin` and build again |

## Security, as it is today

- **Tokens.** Set up over USB, the token lives in the robot's settings, not in the firmware
  image. Set up by a manager (sm.w42.eu), a robot has a token of its own for each app, which
  works only for that robot id. With a shared token in a home manager's catalog, robots share
  that app's one robot token. Anyone who reads the robot's flash gets its token. Per-robot keys and owner-signed permissions are designed (the trust
  design, [stackchan-trust.md](https://github.com/mj41/home-w42-eu/blob/main/docs/implementations/stackchan-trust.md) in
  home-w42-eu) but not built yet.
- **Setup over USB trusts the cable,** with one tap: a new default server needs a **Yes** on
  the robot's screen ("Connect to …?"); Wi-Fi, autostart and servers that are not the default
  need no tap. Having the robot in hand is the proof of ownership, as with its QR code.
- **A paired browser** may switch the robot to another server or make one the default only
  after a **Yes** on the robot's screen.
- **Control over USB** (taps from a program, release firmware): the first tap asks on the
  screen "Let the computer on USB use the screen?"; the Yes lasts until the cable is unplugged.
  USB taps can never answer the robot's own questions.
- **End-to-end encryption:** when it is on for a server, the relay carries only ciphertext
  between the robot and the browsers enrolled through its QR code
  ([e2ee.md](https://github.com/mj41/home-w42-eu/blob/main/docs/e2ee.md)).
- **Use your own server on your own network** for anything private. Camera and
  microphone stream only while a paired browser watches or listens, and the robot shows
  it.
- The car's micro:bit accepts only the Bluetooth addresses on its allowlist. Addresses
  can be spoofed; bonding is still to do.
