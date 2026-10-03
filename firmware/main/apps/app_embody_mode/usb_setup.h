/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace embody {

/**
 * @brief Setup over the USB cable: a computer (chan.w42.eu/setup in Chrome, or the
 *        stackchan-usb tool) asks for the robot id and writes the server, its token,
 *        autostart and Wi-Fi into the robot's settings, so nobody builds firmware for a
 *        token. Physical access is the proof of ownership, like the QR code.
 *
 * Lines on the USB serial port, each "@stackchan <JSON>\n"; anything else (logs) is ignored:
 *   {"op":"hello"}                       -> {"ok":true,"id","model","firmware","protocol":1,"automation"}
 *   {"op":"provision","server":{"name","url","token"},"default":true,"autostart":true,
 *    "wifi":{"ssid","password"}}         -> {"ok":true,"applied":[...]}
 *   {"op":"restart"}                     -> {"ok":true}, then a restart into Embody Mode
 * Started from main() for the whole uptime: a freshly flashed robot is in the launcher.
 */
void startUsbSetup();

}  // namespace embody
