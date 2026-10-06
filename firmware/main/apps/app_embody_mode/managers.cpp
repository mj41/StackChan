/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "managers.h"
#include <settings.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mutex>

namespace embody {

static std::recursive_mutex s_mu;  // the NVS records, read-modify-write

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

static std::string hex_of(const unsigned char* b, size_t n)
{
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += hex[b[i] >> 4];
        s += hex[b[i] & 15];
    }
    return s;
}

Manager loadManager(const char* slot)
{
    std::lock_guard<std::recursive_mutex> lock(s_mu);
    Manager m;
    Settings settings("embody", false);
    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, settings.GetString(slot, "{}"))) {
        return m;
    }
    m.id         = doc["id"] | "";
    m.key        = doc["key"] | "";
    m.name       = doc["name"] | "";
    m.url        = doc["url"] | "";
    m.token      = doc["token"] | "";
    m.page       = doc["page"] | "";
    m.seq        = doc["seq"] | 0;
    m.version    = doc["version"] | 0;
    m.remote     = doc["remote"] | true;
    m.askPin     = doc["ask_pin"] | true;
    m.mayPrimary = doc["may_primary"] | false;
    m.enabled    = doc["enabled"] | true;
    return m;
}

void saveManager(const char* slot, const Manager& m)
{
    std::lock_guard<std::recursive_mutex> lock(s_mu);
    ArduinoJson::JsonDocument doc;
    doc["id"]          = m.id;
    doc["key"]         = m.key;
    doc["name"]        = m.name;
    doc["url"]         = m.url;
    doc["token"]       = m.token;
    doc["page"]        = m.page;
    doc["seq"]         = m.seq;
    doc["version"]     = m.version;
    doc["remote"]      = m.remote;
    doc["ask_pin"]     = m.askPin;
    doc["may_primary"] = m.mayPrimary;
    doc["enabled"]     = m.enabled;
    std::string json;
    ArduinoJson::serializeJson(doc, json);
    Settings settings("embody", true);
    settings.SetString(slot, json);
}

void clearManager(const char* slot)
{
    std::lock_guard<std::recursive_mutex> lock(s_mu);
    Settings settings("embody", true);
    settings.EraseKey(slot);
}

void updateManager(const char* slot, const std::function<void(Manager&)>& change)
{
    std::lock_guard<std::recursive_mutex> lock(s_mu);
    Manager m = loadManager(slot);
    change(m);
    saveManager(slot, m);
}

bool managerFromJson(ArduinoJson::JsonVariantConst o, Manager& m)
{
    m.key = o["key"] | "";
    m.id  = managerId(m.key);
    if (m.id.empty() || m.key.size() > 256) {
        return false;
    }
    m.name       = o["name"] | "";
    m.url        = o["url"] | "";
    m.token      = o["token"] | "";
    m.page       = o["page"] | "";
    m.seq        = o["seq"] | 0;
    m.version    = o["version"] | 0;
    m.remote     = o["remote_apps"] | true;
    m.askPin     = o["ask_pin"] | true;
    m.mayPrimary = o["may_primary"] | false;
    m.enabled    = o["enabled"] | true;  // a USB setup turns it on, unless it says not to
    return m.url.rfind("ws://", 0) == 0 || m.url.rfind("wss://", 0) == 0;
}

std::string managerId(const std::string& keyB64)
{
    std::string der;
    if (!b64decode(keyB64, der) || der.empty()) {
        return "";
    }
    unsigned char hash[32];
    mbedtls_sha256((const unsigned char*)der.data(), der.size(), hash, 0);
    return hex_of(hash, 6);
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

std::string appId(const std::string& url)
{
    unsigned char hash[32];
    mbedtls_sha256((const unsigned char*)url.data(), url.size(), hash, 0);
    return hex_of(hash, 8);
}

}  // namespace embody
