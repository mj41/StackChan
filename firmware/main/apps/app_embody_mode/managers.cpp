/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "managers.h"
#include <settings.h>
#include <ArduinoJson.hpp>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

namespace embody {

static bool b64decode(const std::string& in, std::string& out)
{
    size_t olen = 0;
    out.resize(in.size());
    if (mbedtls_base64_decode((unsigned char*)out.data(), out.size(), &olen, (const unsigned char*)in.data(), in.size()) != 0) {
        return false;
    }
    out.resize(olen);
    return true;
}

std::vector<Manager> loadManagers()
{
    std::vector<Manager> out;
    Settings settings("embody", false);
    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, settings.GetString("managers", "[]"))) {
        return out;
    }
    for (ArduinoJson::JsonObject o : doc.as<ArduinoJson::JsonArray>()) {
        Manager m;
        m.id      = o["id"] | "";
        m.key     = o["key"] | "";
        m.name    = o["name"] | "";
        m.version = o["version"] | 0;
        m.remote  = o["remote"] | true;
        m.askPin  = o["ask_pin"] | true;
        if (!m.id.empty() && !m.key.empty()) {
            out.push_back(m);
        }
    }
    return out;
}

void saveManagers(const std::vector<Manager>& managers)
{
    ArduinoJson::JsonDocument doc;
    auto arr = doc.to<ArduinoJson::JsonArray>();
    for (const auto& m : managers) {
        auto o       = arr.add<ArduinoJson::JsonObject>();
        o["id"]      = m.id;
        o["key"]     = m.key;
        o["name"]    = m.name;
        o["version"] = m.version;
        o["remote"]  = m.remote;
        o["ask_pin"] = m.askPin;
    }
    std::string json;
    ArduinoJson::serializeJson(doc, json);
    Settings settings("embody", true);
    settings.SetString("managers", json);
}

std::string managerId(const std::string& keyB64)
{
    std::string der;
    if (!b64decode(keyB64, der) || der.empty()) {
        return "";
    }
    unsigned char hash[32];
    mbedtls_sha256((const unsigned char*)der.data(), der.size(), hash, 0);
    static const char* hex = "0123456789abcdef";
    std::string id;
    for (int i = 0; i < 6; i++) {
        id += hex[hash[i] >> 4];
        id += hex[hash[i] & 15];
    }
    return id;
}

bool managerSigned(const std::string& keyB64, const std::string& payload, const std::string& sigB64)
{
    std::string key, sig;
    if (!b64decode(keyB64, key) || !b64decode(sigB64, sig) || sig.empty()) {
        return false;
    }
    unsigned char hash[32];
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    const bool ok = mbedtls_pk_parse_public_key(&pk, (const unsigned char*)key.data(), key.size()) == 0 &&
                    mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA) &&
                    mbedtls_sha256((const unsigned char*)payload.data(), payload.size(), hash, 0) == 0 &&
                    mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, (const unsigned char*)sig.data(),
                                      sig.size()) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

std::string appsVersions(const std::vector<Manager>& managers)
{
    std::string out;
    for (const auto& m : managers) {
        if (!out.empty()) {
            out += ",";
        }
        out += m.id + ":" + std::to_string(m.version);
    }
    return out;
}

}  // namespace embody
