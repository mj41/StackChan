/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "usb_setup.h"
#include "automation.h"
#include <hal/hal.h>
#include <lvgl.h>
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
#include <atomic>
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

// A question on the robot's screen, above any app (LVGL's top layer): true when the person at
// the robot taps Yes within the time. The LVGL lock is held only to build and remove the dialog.
static std::atomic<int> s_answer{0};

static void on_answer(lv_event_t* e)
{
    s_answer = (int)(intptr_t)lv_event_get_user_data(e);
}

static bool confirm_on_screen(const std::string& question, const std::string& detail, int seconds)
{
    s_answer = 0;
    lv_obj_t* box = nullptr;
    {
        LvglLockGuard lock;
        box = lv_obj_create(lv_layer_top());
        lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(box, lv_color_hex(0x1E2355), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_radius(box, 0, 0);
        lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);  // nothing below reacts meanwhile
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* q = lv_label_create(box);
        lv_label_set_text(q, question.c_str());
        lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(q, LV_PCT(100));
        lv_obj_set_style_text_font(q, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(q, lv_color_white(), 0);
        lv_obj_align(q, LV_ALIGN_TOP_LEFT, 0, 4);

        lv_obj_t* d = lv_label_create(box);
        lv_label_set_text(d, detail.c_str());
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, LV_PCT(100));
        lv_obj_set_style_text_font(d, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(d, lv_color_hex(0xA3A8CC), 0);
        lv_obj_align_to(d, q, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

        struct {
            const char* text;
            uint32_t color;
            int answer;
            lv_align_t align;
        } buttons[] = {{"Yes", 0x2E9E5B, 1, LV_ALIGN_BOTTOM_LEFT}, {"No", 0xC0392B, -1, LV_ALIGN_BOTTOM_RIGHT}};
        for (const auto& bt : buttons) {
            lv_obj_t* b = lv_button_create(box);
            lv_obj_set_size(b, 130, 60);
            lv_obj_set_style_bg_color(b, lv_color_hex(bt.color), 0);
            lv_obj_align(b, bt.align, 0, 0);
            lv_obj_add_event_cb(b, on_answer, LV_EVENT_CLICKED, (void*)(intptr_t)bt.answer);
            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, bt.text);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
            lv_obj_center(l);
        }
    }
    for (int i = 0; i < seconds * 10 && s_answer == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    {
        LvglLockGuard lock;
        lv_obj_delete(box);
    }
    return s_answer == 1;
}

static std::string host_of(const std::string& url)
{
    auto start = url.find("://");
    start      = start == std::string::npos ? 0 : start + 3;
    return url.substr(start, url.find('/', start) - start);
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
            ArduinoJson::JsonDocument previous;
            if (!ArduinoJson::deserializeJson(previous, settings.GetString("prev_fw", ""))) {
                res["previous"] = previous;  // the firmware it had before the latest setup
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
        // A new default server is confirmed by the person at the robot (a page that holds the
        // USB port must not move the robot to another server unnoticed). Other servers, Wi-Fi
        // and autostart need no tap: they change nothing until chosen.
        std::string new_default = req["pin"] | "";
        std::string new_name;
        if (new_default.empty() && (req["default"] | false)) {
            new_default = req["server"]["url"] | "";
            new_name    = req["server"]["name"] | "";
        }
        if (!new_default.empty()) {
            for (ArduinoJson::JsonObject o : req["servers"].as<ArduinoJson::JsonArray>()) {
                if (std::string(o["url"] | "") == new_default) {
                    new_name = o["name"] | "";
                }
            }
            if (std::string(req["server"]["url"] | "") == new_default && new_name.empty()) {
                new_name = req["server"]["name"] | "";
            }
            Settings current("embody", false);
            if (current.GetString("default", "") != new_default) {
                const std::string host = host_of(new_default);
                mclog::tagInfo(_tag, "asking on the screen: connect to {}?", host);
                if (!confirm_on_screen("Connect to " + (new_name.empty() ? host : new_name) + "?",
                                       host + "\nasked over USB. Tap Yes to make it this robot's server.", 60)) {
                    mclog::tagInfo(_tag, "not confirmed on the robot: nothing changed");
                    return reply_error("not confirmed on the robot: nothing changed");
                }
            }
        }
        auto applied = res["applied"].to<ArduinoJson::JsonArray>();
        // The firmware the robot had before the first setup (identity and backup hash):
        // kept once, never replaced, so it can always be restored from its backup.
        // The firmware it had before this setup (the latest backup): replaced every time.
        if (req["previous"].is<ArduinoJson::JsonObject>()) {
            std::string json;
            ArduinoJson::serializeJson(req["previous"], json);
            if (json.size() <= 1024) {
                Settings settings("embody", true);
                settings.SetString("prev_fw", json);
                applied.add("previous");
            }
        }
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
