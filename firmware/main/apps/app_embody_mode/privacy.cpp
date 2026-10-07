/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "privacy.h"
#include <settings.h>
#include <ArduinoJson.hpp>
#include <cstdio>
#include <ctime>

namespace embody {

Privacy loadPrivacy()
{
    Privacy p;
    Settings settings("embody", false);
    ArduinoJson::JsonDocument doc;
    if (!ArduinoJson::deserializeJson(doc, settings.GetString("privacy", "{}"))) {
        const std::string mode = doc["mode"] | "on";
        if (mode == "on" || mode == "off" || mode == "night") {
            p.mode = mode;
        }
        parseNight(doc["night"] | "", p);
    }
    return p;
}

void savePrivacy(const Privacy& p)
{
    ArduinoJson::JsonDocument doc;
    doc["mode"]  = p.mode;
    doc["night"] = nightText(p);
    std::string json;
    ArduinoJson::serializeJson(doc, json);
    Settings settings("embody", true);
    settings.SetString("privacy", json);
}

bool parseNight(const std::string& s, Privacy& p)
{
    unsigned fh = 0, fm = 0, th = 0, tm = 0;
    if (std::sscanf(s.c_str(), "%u:%u-%u:%u", &fh, &fm, &th, &tm) != 4 || fh > 23 || th > 23 || fm > 59 || tm > 59 ||
        (fh == th && fm == tm)) {
        return false;
    }
    p.from = (int)(fh * 60 + fm);
    p.to   = (int)(th * 60 + tm);
    return true;
}

std::string nightText(const Privacy& p)
{
    char buf[48];  // the compiler cannot see that the values are minutes of a day
    std::snprintf(buf, sizeof buf, "%02d:%02d-%02d:%02d", p.from / 60 % 24, p.from % 60, p.to / 60 % 24, p.to % 60);
    return buf;
}

std::string privacyBlocked(const Privacy& p)
{
    if (p.mode == "off") {
        return "off on the robot";
    }
    if (p.mode != "night") {
        return "";
    }
    const time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    if (local.tm_year + 1900 < 2025) {
        return "night " + nightText(p) + " (the robot's clock is not set yet)";
    }
    const int m     = local.tm_hour * 60 + local.tm_min;
    const bool dark = p.from < p.to ? (m >= p.from && m < p.to) : (m >= p.from || m < p.to);
    return dark ? "night " + nightText(p) : "";
}

}  // namespace embody
