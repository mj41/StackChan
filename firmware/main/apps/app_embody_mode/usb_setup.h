/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <functional>
#include <string>

namespace embody {

/**
 * @brief Setup over the USB cable: a computer (sm.w42.eu in Chrome, or the
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
 *   "manager":{"key","name","version","remote_apps","ask_pin"}: set up by a Stackchan manager: its
 *   public key (base64 DER, P-256; its id: the first 12 hex of the key's SHA-256), its name, the app
 *   list's version, whether it may change its apps later (ManagedApps, signed with that key; default
 *   true), and whether a new start app from it is asked on the screen (default true). A robot may
 *   have several managers (managers.h): its "servers" replace that manager's apps only, and servers
 *   no manager owns go. Only over USB; hello and status return "managers" (without keys).
 *   {"op":"restart"}                     -> {"ok":true}, then a restart into Embody Mode
 *   {"op":"pair"}                        -> {"ok":true,"url"}: the pairing link on the robot's screen
 *   {"op":"status"}                      -> {"ok":true,"embody":{server, name, state, status, qr, shown,
 *                                            default, question, servers:[{name, url, origin}]}|null,
 *                                            "managers":[{id, name, version, remote_apps, ask_pin}]?}:
 *                                            read-only, no tokens
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

/**
 * @brief What Embody Mode is doing, as JSON, for {"op":"status"} (tests, tools): set from its loop.
 */
void setStatus(const std::string& json);

/**
 * @brief The manager changed (a USB setup): the channel connects to the new one
 *        (manager_channel.h). Embody Mode sets the hook; the USB task calls it.
 */
void onManagerChanged(std::function<void()> fn);
void managerChanged();

/**
 * @brief A USB setup gives the robot another manager: the old one hears it first (Leaving, with
 *        the new one's name), so its page can say where the robot went (manager_channel.h).
 *        Returns when that is sent, or after a moment. Embody Mode sets the hook.
 */
void onManagerLeaving(std::function<void(const std::string& to)> fn);
void managerLeaving(const std::string& to);

/**
 * @brief Automation (tests): the app loop stops for this many seconds, as if it hung; the manager
 *        channel then reports it stuck. 0: none pending. Embody Mode takes it in its loop.
 */
int takeStall();

/**
 * @brief The firmware's version as the robot reports it (USB hello, Register): M5Stack's version
 *        it is based on, "mj41" (this fork), and the Embody Mode release from release.sh
 *        ("1.5.1-mj41-v0.3.1"), or "-dev" in an own build. The release's manifest.json says the same.
 */
inline const char* firmwareVersion()
{
#ifdef EMBODY_VERSION
    return FIRMWARE_VERSION "-mj41-" EMBODY_VERSION;
#else
    return FIRMWARE_VERSION "-mj41-dev";
#endif
}

}  // namespace embody
