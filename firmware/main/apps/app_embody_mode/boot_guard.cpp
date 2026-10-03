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
static constexpr uint32_t kMagic = 0x45424731;  // "EBG1"

void embody::note_boot()
{
    const esp_reset_reason_t reason = esp_reset_reason();
    if (s_magic != kMagic) {
        s_magic   = kMagic;
        s_crashes = 0;
    }
    const bool crash = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
                       reason == ESP_RST_WDT;
    s_crashes = crash ? s_crashes + 1 : 0;
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
