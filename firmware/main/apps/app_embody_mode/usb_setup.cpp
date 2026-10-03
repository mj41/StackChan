/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "usb_setup.h"
#include "automation.h"
#include <hal/hal.h>
#include <settings.h>
#include <ssid_manager.h>
#include <mooncake_log.h>
#include <ArduinoJson.hpp>
#include <driver/usb_serial_jtag.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

static const char* _tag = "UsbSetup";
static constexpr const char* _prefix = "@stackchan ";

static std::mutex s_pair_mutex;
static std::string s_pair_url;

void embody::setPairUrl(const std::string& url)
{
    std::lock_guard<std::mutex> lock(s_pair_mutex);
    s_pair_url = url;
}

static void reply(const ArduinoJson::JsonDocument& doc)
{
    std::string json;
    ArduinoJson::serializeJson(doc, json);  // replaces the string's content: not appendable
    const std::string line = _prefix + json + "\n";
    usb_serial_jtag_write_bytes(line.data(), line.size(), pdMS_TO_TICKS(500));
}

static void reply_error(const char* error)
{
    ArduinoJson::JsonDocument doc;
    doc["ok"]    = false;
    doc["error"] = error;
    reply(doc);
}

// The server entry, as Embody Mode keeps its list (NVS "embody": "servers", "default").
static bool save_server(const std::string& name, const std::string& url, const std::string& token, bool makeDefault)
{
    if (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0) {
        return false;
    }
    Settings settings("embody", true);
    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, settings.GetString("servers", "[]")) || !doc.is<ArduinoJson::JsonArray>()) {
        doc.to<ArduinoJson::JsonArray>();
    }
    auto arr = doc.as<ArduinoJson::JsonArray>();
    ArduinoJson::JsonObject entry;
    for (ArduinoJson::JsonObject o : arr) {
        if (std::string(o["url"] | "") == url) {
            entry = o;
        }
    }
    if (entry.isNull()) {
        entry = arr.add<ArduinoJson::JsonObject>();
    }
    entry["name"]   = name.empty() ? url : name;
    entry["url"]    = url;
    entry["token"]  = token;
    entry["origin"] = "added";
    std::string json;
    ArduinoJson::serializeJson(doc, json);
    settings.SetString("servers", json);
    if (makeDefault) {
        settings.SetString("default", url);
    }
    return true;
}

static void handle(const std::string& json)
{
    ArduinoJson::JsonDocument req;
    if (ArduinoJson::deserializeJson(req, json)) {
        return reply_error("bad JSON");
    }
    const std::string op = req["op"] | "";
    ArduinoJson::JsonDocument res;
    if (op == "hello") {
        res["ok"]         = true;
        std::string id = "stackchan-" + GetHAL().getFactoryMacString();  // as Embody Mode: lowercase
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return std::tolower(c); });
        res["id"]         = id;
        res["model"]      = "stackchan-cores3";
        res["firmware"]   = esp_app_get_description()->version;
        res["protocol"]   = 1;
        {
            Settings settings("embody", false);
            ArduinoJson::JsonDocument original;
            if (!ArduinoJson::deserializeJson(original, settings.GetString("orig_fw", ""))) {
                res["original"] = original;  // the firmware it had before the first setup
            }
        }
#if CONFIG_STACKCHAN_EMBODY_AUTOMATION
        res["automation"] = true;
#else
        res["automation"] = false;
#endif
        return reply(res);
    }
    if (op == "provision") {
        auto applied = res["applied"].to<ArduinoJson::JsonArray>();
        // The firmware the robot had before the first setup (identity and backup hash):
        // kept once, never replaced, so it can always be restored from its backup.
        if (req["original"].is<ArduinoJson::JsonObject>()) {
            Settings settings("embody", true);
            if (settings.GetString("orig_fw", "").empty()) {
                std::string json;
                ArduinoJson::serializeJson(req["original"], json);
                if (json.size() <= 1024) {
                    settings.SetString("orig_fw", json);
                    applied.add("original");
                }
            }
        }
        if (req["server"].is<ArduinoJson::JsonObject>()) {
            if (!save_server(req["server"]["name"] | "", req["server"]["url"] | "", req["server"]["token"] | "",
                             req["default"] | false)) {
                return reply_error("server url must start with ws:// or wss://");
            }
            applied.add("server");
            mclog::tagInfo(_tag, "server {} saved{}", std::string(req["server"]["url"] | ""),
                           (req["default"] | false) ? " as the default" : "");
        }
        // More servers (apps) at once; "pin" makes one of them the default at start.
        if (req["servers"].is<ArduinoJson::JsonArray>()) {
            for (ArduinoJson::JsonObject o : req["servers"].as<ArduinoJson::JsonArray>()) {
                if (!save_server(o["name"] | "", o["url"] | "", o["token"] | "", false)) {
                    return reply_error("server url must start with ws:// or wss://");
                }
                mclog::tagInfo(_tag, "server {} saved", std::string(o["url"] | ""));
            }
            applied.add("servers");
        }
        if (const std::string pin = req["pin"] | ""; !pin.empty()) {
            Settings settings("embody", true);
            settings.SetString("default", pin);
            applied.add("pin");
            mclog::tagInfo(_tag, "pinned {}", pin);
        }
#if CONFIG_STACKCHAN_EMBODY_AUTOMATION
        if (req["autostart"].is<bool>()) {
            embody::set_autostart(req["autostart"].as<bool>());
            applied.add("autostart");
        }
#endif
        const std::string ssid = req["wifi"]["ssid"] | "";
        if (!ssid.empty()) {
            SsidManager::GetInstance().AddSsid(ssid, req["wifi"]["password"] | "");
            applied.add("wifi");
            mclog::tagInfo(_tag, "wifi {} saved", ssid);  // never the password
        }
        res["ok"] = true;
        return reply(res);
    }
    if (op == "pair") {  // the link on the screen: physical access, like reading the QR code
        std::string url;
        {
            std::lock_guard<std::mutex> lock(s_pair_mutex);
            url = s_pair_url;
        }
        if (url.empty()) {
            return reply_error("no pairing code yet: Embody Mode is not connected to its server");
        }
        res["ok"]  = true;
        res["url"] = url;
        return reply(res);
    }
    if (op == "restart") {
        res["ok"] = true;
        reply(res);
#if CONFIG_STACKCHAN_EMBODY_AUTOMATION
        embody::set_launch_once(embody::kEmbodyAppName);  // straight into Embody Mode
#endif
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    reply_error("unknown op");
}

static void setup_task(void*)
{
    std::string line;
    uint8_t buf[128];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof buf, pdMS_TO_TICKS(1000));
        for (int i = 0; i < n; i++) {
            const char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (line.rfind(_prefix, 0) == 0) {
                    handle(line.substr(strlen(_prefix)));
                }
                line.clear();
            } else if (line.size() < 4096) {
                line.push_back(c);
            }
        }
    }
}

void embody::startUsbSetup()
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size                  = 1024;
    cfg.tx_buffer_size                  = 1024;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        mclog::tagWarn(_tag, "USB serial driver not installed: no setup over USB");
        return;
    }
    xTaskCreate(setup_task, "usb_setup", 6144, nullptr, 3, nullptr);
    mclog::tagInfo(_tag, "listening on the USB serial port");
}
