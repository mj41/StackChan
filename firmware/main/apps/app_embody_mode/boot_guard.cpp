/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "automation.h"
#include <esp_attr.h>
#include <esp_system.h>
#include <mooncake_log.h>

// Kept in RTC memory: it survives a panic or watchdog reset, not a power cycle.
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_crashes;
static RTC_NOINIT_ATTR uint32_t s_last_crash;   // its esp_reset_reason_t
static RTC_NOINIT_ATTR uint32_t s_crash_total;  // since power-on
static constexpr uint32_t kMagic = 0x45424732;  // "EBG2"

void embody::note_boot()
{
    const esp_reset_reason_t reason = esp_reset_reason();
    if (s_magic != kMagic) {
        s_magic       = kMagic;
        s_crashes     = 0;
        s_last_crash  = ESP_RST_UNKNOWN;
        s_crash_total = 0;
    }
    const bool crash = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
                       reason == ESP_RST_WDT;
    s_crashes = crash ? s_crashes + 1 : 0;
    if (crash) {
        s_last_crash = reason;
        s_crash_total++;
    }
    if (s_crashes) {
        mclog::tagWarn("BootGuard", "restart after a crash ({} in a row)", s_crashes);
    }
}

int embody::crash_restarts()
{
    return s_magic == kMagic ? (int)s_crashes : 0;
}

void embody::mark_stable()
{
    s_crashes = 0;
}

static std::string reason_name(uint32_t r)
{
    switch (r) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_USB: return "usb";
        case ESP_RST_EXT: return "external";
        default: return "other (" + std::to_string(r) + ")";
    }
}

std::string embody::reset_reason()
{
    return reason_name(esp_reset_reason());
}

std::string embody::last_crash()
{
    return s_magic == kMagic && s_crash_total > 0 ? reason_name(s_last_crash) : "";
}

int embody::crash_total()
{
    return s_magic == kMagic ? (int)s_crash_total : 0;
}
