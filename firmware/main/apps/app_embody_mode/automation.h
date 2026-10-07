/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

// Optional automation (CONFIG_STACKCHAN_EMBODY_AUTOMATION, off by default): which app the
// launcher opens by itself after a boot. Shared by the launcher and Embody Mode.
//
// - "autostart" (NVS embody/autostart, off until a server sets it): open Embody Mode after
//   every power-on or restart. Not when an app's Home brought the robot to the launcher
//   (a warm-reboot target is set then), so Home always leaves the robot in the launcher.
// - "launch" (NVS embody/launch, used once): the app a `launch` or `restart` command asked
//   for, by its launcher name ("AVATAR", "Embody Mode"…), or "launcher" for none.

#include <sdkconfig.h>
#include <settings.h>
#include <string>

namespace embody {

// Boot-loop guard (boot_guard.cpp): crash restarts in a row (panic, watchdog), counted in
// RTC memory. note_boot() once at start; an app that ran a while calls mark_stable().
inline constexpr int kMaxCrashRestarts = 3;
void note_boot();
int crash_restarts();
void mark_stable();
// For hello over USB: why this boot happened, and the last crash since power-on (RTC memory:
// it survives the restarts after it) and how many there were.
std::string reset_reason();
std::string last_crash();
int crash_total();

inline constexpr const char* kEmbodyAppName = "Embody Mode";
inline constexpr const char* kLauncherName  = "launcher";

inline bool autostart()
{
    Settings settings("embody", false);
    return settings.GetBool("autostart", false);
}

inline void set_autostart(bool on)
{
    Settings settings("embody", true);
    settings.SetBool("autostart", on);
}

// The next boot opens this app once (kLauncherName: stay in the launcher).
inline void set_launch_once(const std::string& app)
{
    Settings settings("embody", true);
    settings.SetString("launch", app);
}

// Called once by the launcher after a boot: the app to open, or "" for none. Clears the
// one-time request. returning_from_app: an app's Home asked for the launcher. After
// kMaxCrashRestarts crashes in a row it opens nothing, so a crashing app cannot loop.
inline std::string take_boot_app(bool returning_from_app)
{
#if CONFIG_STACKCHAN_EMBODY_AUTOMATION
    std::string once;
    {
        Settings settings("embody", true);
        once = settings.GetString("launch", "");
        if (!once.empty()) {
            settings.EraseKey("launch");
        }
    }
    if (crash_restarts() >= kMaxCrashRestarts) {
        return "";  // a boot loop: stay in the launcher
    }
    if (!once.empty()) {
        return once == kLauncherName ? "" : once;
    }
    if (!returning_from_app && autostart()) {
        return kEmbodyAppName;
    }
#else
    (void)returning_from_app;
#endif
    return "";
}

}  // namespace embody
