/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <ArduinoJson.hpp>

namespace embody {

/**
 * @brief The robot's Stackchan manager (s-w42-eu-manager: a home's own, or sm.w42.eu), set over
 *        USB (home-w42-eu docs/manager-channel.md). The **primary** (NVS "embody"/"manager") is
 *        the one the robot keeps its channel to (manager_channel.h) and the only one whose signed
 *        messages it applies; every app on the robot comes from it. An optional **second** manager
 *        (NVS "manager2") may become the primary on the robot's Manager screen, if the setup said
 *        so (may_primary).
 */
struct Manager {
    std::string id;     // the first 12 hex characters of SHA-256 of its key (DER)
    std::string key;    // its public key, base64 DER (SubjectPublicKeyInfo, P-256)
    std::string name;   // e.g. "sm.w42.eu", "home on laptop", for people
    std::string url;    // its channel: ws(s)://…/robot
    std::string token;  // the robot's channel token there
    std::string page;   // its page for people (the Manager screen shows it)
    int32_t seq     = 0;  // of the last signed message applied
    int32_t version = 0;  // of the last app list applied
    bool remote     = true;   // it may change the apps and switch them (else only USB)
    bool askPin     = true;   // a switch or a new start app from it is asked on the screen
    bool mayPrimary = false;  // the second: may become the primary on the robot's screen

    bool valid() const { return !id.empty() && !key.empty() && !url.empty(); }
};

// "manager" (the primary) or "manager2".
Manager loadManager(const char* slot);
void saveManager(const char* slot, const Manager& m);
void clearManager(const char* slot);
// Load, change and save under one lock (the channel task and the app loop both update it).
void updateManager(const char* slot, const std::function<void(Manager&)>& change);

// From the setup's JSON {key, name, url, token, page, seq, version, remote_apps, ask_pin,
// may_primary}; false when the key is not one.
bool managerFromJson(ArduinoJson::JsonVariantConst o, Manager& m);

// The id of a manager's key ("" for a key that is not base64).
std::string managerId(const std::string& keyB64);

// Whether sig (base64, ECDSA P-256 over SHA-256) signs payload with keyB64.
bool managerSigned(const std::string& keyB64, const std::string& payload, const std::string& sigB64);

// How the robot and its manager name an app without its URL: the first 8 bytes of SHA-256 of the
// URL, hex.
std::string appId(const std::string& url);

}  // namespace embody
