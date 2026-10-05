# StackChan Open-Source

> **This fork, branch `embody-mj41`,** adds **Embody Mode**: the first launcher app, which
> makes the robot a light client of a home server (camera, microphone, speaker, every
> sensor as raw data, and commands), switchable between servers and apps (a remote
> dashboard, a pet, a cockpit that drives a TPBot car over BLE). See
> [firmware/main/apps/app_embody_mode/README.md](firmware/main/apps/app_embody_mode/README.md),
> and to set a robot up: one click in Chrome at [sm.w42.eu](https://sm.w42.eu), or your own
> server or build: [SETUP.md](firmware/main/apps/app_embody_mode/SETUP.md).
> The robots are set up by [s-w42-eu-manager](https://github.com/mj41/s-w42-eu-manager) for the apps [s-w42-eu-raw](https://github.com/mj41/s-w42-eu-raw), [s-w42-eu-pet](https://github.com/mj41/s-w42-eu-pet)
> and [s-w42-eu-sbot](https://github.com/mj41/s-w42-eu-sbot) ([s.w42.eu](https://s.w42.eu)); the car's micro:bit runs [tpbot-ble](https://github.com/mj41/tpbot-ble). All of them are part of
> [home-w42-eu](https://github.com/mj41/home-w42-eu), a local first, privacy first platform for a home.
>
> **A proof of concept, vibe coded:** written with AI agents and tested on a real robot at home,
> but neither the code nor its security has been reviewed by humans. Use it on your own network.
>
> **Early stage: no backward compatibility.** Protocols, APIs, file formats and stored settings
> change when something better comes along, without migrations: update the robot's firmware
> and the servers together.
>
> **Want more?** Ask in [home-w42-eu's issues](https://github.com/mj41/home-w42-eu/issues), and ideally [sponsor mj41](https://github.com/sponsors/mj41) on GitHub:
> mj41 codes for attention food.
>
> <img src="firmware/main/apps/app_embody_mode/screenshots/face.jpg" width="320" alt="Embody Mode: the robot's face with a speech bubble"> <img src="firmware/main/apps/app_embody_mode/screenshots/qr-screen.jpg" width="320" alt="Embody Mode: the QR screen with the pairing code">
>
> The rest of this page is upstream's.

<img src="https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1205/K151_stack_chan_main_pictures_01.webp" width="60%">

Here are StackChan related open-source resources, including source code of the StackChan firmware, remote controller firmware, mobile app (iOS and Android), and server. 

Update of this repo could be a little late than the released firmware and mobile app. 

----

<img src="https://cdn.shopify.com/s/files/1/0056/7689/2250/files/5a589623895f65487717894d9240f6b8.png" width="60%">

**StackChan is a super kawaii AI desktop robot co-created by M5Stack and the user community.** It uses the M5Stack **flagship IoT development kit [CoreS3](https://docs.m5stack.com/en/core/CoreS3)** as its main controller, powered by an ESP32-S3 SoC featuring a 240 MHz dual-core processor, with 16MB Flash and 8MB PSRAM onboard, and supporting Wi-Fi and BLE. The main unit also integrates a 2.0-inch capacitive touch display with a high-strength glass cover, a 0.3 MP camera, a proximity & ambient light sensor, a 9-axis IMU (accelerometer + gyroscope + magnetometer), a microSD card slot, a 1W speaker, dual microphones, and power/reset buttons. 

The **robot body**, connected to the main unit, includes a USB-C interface for power and data, a 550 mAh battery, two feedback servos (360-degree continuous rotation on the horizontal axis and 90-degree movement on the vertical axis), two rows totaling 12 RGB LEDs, infrared transmitter and receiver, a three-zone touch panel, and a full-featured NFC module. 

The **factory firmware** is feature-rich, including an AI Agent, lively and expressive animations, ESP-NOW wireless remote control, and online app downloads. It can connect to a mobile app for video viewing, remote avatar control, and more, and also supports online updates (OTA). The product also supports programming via Arduino, UiFlow2, and other methods, and can connect to various expansion units in the M5Stack ecosystem, making it easy to implement a wide range of custom functions. 

> ⚠️ Do not forcibly rotate any movable parts connected to the motors by hand when you are unsure whether the motors are powered and under control, as this may cause hardware damage. 

- Purchase link: [M5Stack Official Store](https://shop.m5stack.com/products/stackchan-kawaii-co-created-open-source-ai-desktop-robot) | [淘宝 Taobao](https://item.taobao.com/item.htm?id=1042238294510)

- Product document page: [English](https://docs.m5stack.com/en/StackChan) | [日本語](https://docs.m5stack.com/ja/StackChan) | [中文](https://docs.m5stack.com/zh_CN/StackChan)

- Board support package: https://github.com/m5stack/StackChan-BSP

Thank you to the contributors of the StackChan community, especially: 

| ![](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1205/avatar_stack_chan.jpg) | ![](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1205/avatar_takao.jpg) |
| -------------------------------------------------------------------------------- | --------------------------------------------------------------------------- |
| [@stack_chan](https://x.com/stack_chan)                                          | [@mongonta555](https://x.com/mongonta555)                                   |
| Shinya Ishikawa                                                                  | Takao Akaki                                                                 |

Stack-chan (スタックチャン) is a registered trademark of Shinya Ishikawa; this project is independent and only made to work with [Stack-chan](https://github.com/stack-chan/stack-chan) robots.
