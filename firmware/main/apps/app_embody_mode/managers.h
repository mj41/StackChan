/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace embody {

/**
 * @brief A Stackchan manager this robot was set up by over USB (s-w42-eu-manager: a home's own, or
 *        sm.w42.eu). A robot may have several; each manages only its own apps (their origin is
 *        "manager:<id>"), and the robot shows them all. NVS "embody"/"managers", a JSON list.
 */
struct Manager {
    std::string id;    // the first 12 hex characters of SHA-256 of its key (DER)
    std::string key;   // its public key, base64 DER (SubjectPublicKeyInfo, P-256)
    std::string name;  // e.g. "sm.w42.eu", for people
    int32_t version = 0;  // of the last app list from it
    bool remote     = true;  // it may change its apps later (ManagedApps)
    bool askPin     = true;  // a new start app from it is asked on the screen
};

std::vector<Manager> loadManagers();
void saveManagers(const std::vector<Manager>& managers);

// The id of a manager's key ("" for a key that is not base64).
std::string managerId(const std::string& keyB64);

// The origin of the apps a manager put on the robot.
inline std::string managerOrigin(const std::string& id)
{
    return "manager:" + id;
}

// Whether sig (base64, ECDSA P-256 over SHA-256) signs payload with keyB64.
bool managerSigned(const std::string& keyB64, const std::string& payload, const std::string& sigB64);

// "id:version,id:version": the app list versions the robot has, one per manager (Register label
// apps_ver, AppsVersion): each manager finds its own.
std::string appsVersions(const std::vector<Manager>& managers);

}  // namespace embody
