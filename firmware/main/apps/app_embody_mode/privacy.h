/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <string>

namespace embody {

/**
 * @brief The camera and the microphone, as the person at the robot set them (s5 "secure"): the
 *        firmware itself refuses them, whatever an app, a manager or a browser asks.
 *
 * mode "on": allowed; "off": never; "night": not between from and to (local time, the robot's
 * time zone; a window may pass midnight, 22:00-07:00). Kept in NVS (embody/privacy). Changed only
 * on the robot's Manager screen (asked with a Yes) or over USB (physical access), never online.
 */
struct Privacy {
    std::string mode = "on";
    int from         = 22 * 60;  // minutes after midnight
    int to           = 7 * 60;
};

Privacy loadPrivacy();
void savePrivacy(const Privacy& p);

// "22:00-07:00" into p.from and p.to; false when it is not that.
bool parseNight(const std::string& s, Privacy& p);
std::string nightText(const Privacy& p);  // "22:00-07:00"

// Blocked now: why ("off on the robot", "night 22:00-07:00"), else "". With "night" and the clock
// not set yet (no network time), blocked: off until the robot knows it is day.
std::string privacyBlocked(const Privacy& p);

}  // namespace embody
