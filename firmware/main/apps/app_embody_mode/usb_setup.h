/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <string>

namespace embody {

/**
 * @brief Setup over the USB cable: a computer (chan.w42.eu/setup in Chrome, or the
 *        s-w42-eu-usb tool) asks for the robot id and writes the server, its token,
 *        autostart and Wi-Fi into the robot's settings, so nobody builds firmware for a
 *        token. Physical access is the proof of ownership, like the QR code.
 *
 * Lines on the USB serial port, each "@stackchan <JSON>\n"; anything else (logs) is ignored:
 *   {"op":"hello"}                       -> {"ok":true,"id","model","firmware","protocol":1,"automation",
 *                                            "original"?}
 *   {"op":"provision","server":{"name","url","token"},"default":true,"autostart":true,
 *    "wifi":{"ssid","password"},"original":{...}}  -> {"ok":true,"applied":[...]}
 *   "servers":[{"name","url","token"},...] adds more at once; "pin":"<url>" makes it the default
 *   {"op":"restart"}                     -> {"ok":true}, then a restart into Embody Mode
 *   {"op":"pair"}                        -> {"ok":true,"url"}: the pairing link on the robot's screen
 * "original": the firmware the robot had before its first setup (the setup page reads its
 *   identity and saves a backup); stored once (NVS embody/orig_fw), never replaced.
 * "previous": the firmware it had before the latest setup with a backup (NVS embody/prev_fw),
 *   replaced each time; hello returns both.
 * A provision that changes the default server asks on the screen first ("Connect to …? Yes/No",
 *   60 s); without a Yes nothing is changed and the answer is an error.
 * Started from main() for the whole uptime: a freshly flashed robot is in the launcher.
 */
void startUsbSetup();

/**
 * @brief The pairing link Embody Mode shows now ("" while it has none), for {"op":"pair"}:
 *        the setup page opens it, so the computer that set the robot up is paired at once.
 *        Thread-safe: Embody Mode sets it from its loop, the USB task reads it.
 */
void setPairUrl(const std::string& url);

}  // namespace embody
