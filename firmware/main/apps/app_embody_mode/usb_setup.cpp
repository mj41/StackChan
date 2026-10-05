/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "usb_setup.h"
#include "managers.h"
#include "automation.h"
#include "confirm_dialog.h"
#include <hal/hal.h>
#include <lvgl.h>
#include <settings.h>
#include <ssid_manager.h>
#include <mooncake_log.h>
#include <ArduinoJson.hpp>
#include <driver/usb_serial_jtag.h>
#include <jpg/image_to_jpeg.h>
#include <mbedtls/base64.h>
#include <mooncake.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <strings.h>
#include <cstring>
#include <mutex>
#include <string>

static const char* _tag = "UsbSetup";
static constexpr const char* _prefix = "@stackchan ";

static std::mutex s_pair_mutex;
static std::string s_pair_url;
static std::string s_status;  // Embody Mode's status JSON (setStatus), under s_pair_mutex

void embody::setStatus(const std::string& json)
{
    std::lock_guard<std::mutex> lock(s_pair_mutex);
    s_status = json;
}

void embody::setPairUrl(const std::string& url)
{
    std::lock_guard<std::mutex> lock(s_pair_mutex);
    s_pair_url = url;
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
    size_t sent = 0;  // a screenshot is ~30 KB: write until it is all out (or the host stops reading)
    for (int idle = 0; sent < line.size() && idle < 10;) {
        // at most 512 bytes at a time: a write larger than the 1 KB TX buffer never fits
        const int n = usb_serial_jtag_write_bytes(line.data() + sent, std::min<size_t>(512, line.size() - sent),
                                                  pdMS_TO_TICKS(200));
        idle = n > 0 ? 0 : idle + 1;
        sent += n > 0 ? n : 0;
    }
    if (sent < line.size()) {
        mclog::tagWarn(_tag, "reply cut: {} of {} bytes sent", sent, line.size());
    }
}

static void reply_error(const char* error)
{
    ArduinoJson::JsonDocument doc;
    doc["ok"]    = false;
    doc["error"] = error;
    reply(doc);
}

// The server entry, as Embody Mode keeps its list (NVS "embody": "servers", "default").
static bool save_server(const std::string& name, const std::string& url, const std::string& token, bool makeDefault,
                        const char* origin = "added")
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
    entry["name"] = name.empty() ? url : name;
    entry["url"]  = url;
    if (!token.empty() || entry["token"].isNull()) {
        entry["token"] = token;  // none given: an app the robot has keeps its token
    }
    entry["origin"] = origin;
    std::string json;
    ArduinoJson::serializeJson(doc, json);
    settings.SetString("servers", json);
    if (makeDefault) {
        settings.SetString("default", url);
    }
    return true;
}

// A manager's setup over USB: the robot's apps are exactly keep (all from its manager); the
// built-in "Set up" entry stays.
static void keep_only(const std::vector<std::string>& keep)
{
    Settings settings("embody", true);
    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, settings.GetString("servers", "[]")) || !doc.is<ArduinoJson::JsonArray>()) {
        return;
    }
    ArduinoJson::JsonDocument out;
    auto arr = out.to<ArduinoJson::JsonArray>();
    for (ArduinoJson::JsonObject o : doc.as<ArduinoJson::JsonArray>()) {
        const std::string url = o["url"] | "";
        if (std::find(keep.begin(), keep.end(), url) != keep.end() || std::string(o["origin"] | "") == "built-in") {
            arr.add(o);
        }
    }
    std::string json;
    ArduinoJson::serializeJson(out, json);
    settings.SetString("servers", json);
}

#if CONFIG_STACKCHAN_EMBODY_AUTOMATION
// Automation over USB: what a person at the robot can do, for a program on the computer it is
// plugged into (start an app, tap the screen, see it). Never an answer to the robot's own
// questions (embody::questionOpen): those are for the person at the robot only.

// A tap as a virtual pointer: pressed at (x, y) until s_tap_until (LVGL ticks, ms).
static std::atomic<int> s_tap_x{0}, s_tap_y{0};
static std::atomic<uint32_t> s_tap_until{0};
static lv_indev_t* s_usb_pointer = nullptr;

static void usb_pointer_read(lv_indev_t*, lv_indev_data_t* data)
{
    data->point.x = s_tap_x;
    data->point.y = s_tap_y;
    const bool pressed = (int32_t)(s_tap_until.load() - lv_tick_get()) > 0 && !embody::questionOpen();
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// Taps need the person's Yes once ("Let the computer on USB use the screen?"): with taps a program
// could choose and pin any server in the list. The Yes is kept in NVS (a launch restarts the
// robot) and ends when the USB host goes away (the cable is unplugged).
static bool usb_control_allowed()
{
    Settings settings("embody", false);
    return settings.GetBool("usb_ctrl", false);
}

static void set_usb_control(bool on)
{
    Settings settings("embody", true);
    settings.SetBool("usb_ctrl", on);
    mclog::tagInfo(_tag, "control over USB {}", on ? "allowed (until the cable is unplugged)" : "ended");
}

static void tap(ArduinoJson::JsonDocument& res, int x, int y, int ms)
{
    {
        LvglLockGuard lock;
        if (!s_usb_pointer) {
            s_usb_pointer = lv_indev_create();
            lv_indev_set_type(s_usb_pointer, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(s_usb_pointer, usb_pointer_read);
            lv_indev_set_display(s_usb_pointer, lv_display_get_default());
        }
        s_tap_x     = x;
        s_tap_y     = y;
        s_tap_until = lv_tick_get() + (uint32_t)ms;
    }
    vTaskDelay(pdMS_TO_TICKS(ms + 100));  // released again when the reply goes out
    res["ok"] = true;
}

// The active screen as a JPEG, base64 (questions on the top layer are not in it: "question").
static bool screenshot(ArduinoJson::JsonDocument& res)
{
    lv_draw_buf_t* snap = nullptr;
    {
        LvglLockGuard lock;
        snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    }
    if (!snap) {
        mclog::tagWarn(_tag, "screenshot: no snapshot");
        return false;
    }
    mclog::tagInfo(_tag, "screenshot: {}x{}, encoding", (int)snap->header.w, (int)snap->header.h);
    uint8_t* jpeg = nullptr;
    size_t len    = 0;
    const bool ok = image_to_jpeg((uint8_t*)snap->data, snap->data_size, snap->header.w, snap->header.h,
                                  V4L2_PIX_FMT_RGB565, 70, &jpeg, &len) &&
                    jpeg;
    res["width"]  = snap->header.w;
    res["height"] = snap->header.h;
    {
        LvglLockGuard lock;
        lv_draw_buf_destroy(snap);
    }
    if (!ok) {
        return false;
    }
    size_t b64len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64len, jpeg, len);
    std::string b64(b64len, '\0');
    mbedtls_base64_encode((unsigned char*)b64.data(), b64.size(), &b64len, jpeg, len);
    free(jpeg);
    b64.resize(b64len);
    res["ok"]       = true;
    res["question"] = embody::questionOpen();
    res["jpeg"]     = b64;
    mclog::tagInfo(_tag, "screenshot: {} bytes JPEG", len);
    return true;
}
#endif

static std::function<void()> s_manager_changed;
static std::atomic<int> s_stall{0};

int embody::takeStall()
{
    return s_stall.exchange(0);
}

void embody::onManagerChanged(std::function<void()> fn)
{
    s_manager_changed = std::move(fn);
}

void embody::managerChanged()
{
    if (s_manager_changed) {
        s_manager_changed();
    }
}

// The robot's managers (managers.h), for hello and status (no keys, no tokens).
static void add_managers(ArduinoJson::JsonDocument& res)
{
    for (const char* slot : {"manager", "manager2"}) {
        const auto m = embody::loadManager(slot);
        if (!m.valid()) {
            continue;
        }
        auto o           = res[slot].to<ArduinoJson::JsonObject>();
        o["id"]          = m.id;
        o["name"]        = m.name;
        o["url"]         = m.url;
        o["page"]        = m.page;
        o["version"]     = m.version;
        o["seq"]         = m.seq;
        o["remote_apps"] = m.remote;
        o["ask_pin"]     = m.askPin;
        if (slot[7] == '2') {
            o["may_primary"] = m.mayPrimary;
        }
    }
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
        add_managers(res);
        std::string id = "stackchan-" + GetHAL().getFactoryMacString();  // as Embody Mode: lowercase
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return std::tolower(c); });
        res["id"]         = id;
        res["model"]      = "stackchan-cores3";
        res["firmware"]   = embody::firmwareVersion();
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
                if (!embody::askOnScreenAndWait("Connect to " + (new_name.empty() ? host : new_name) + "?",
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
        // The robot's manager (managers.h): its key (it signs what the robot gets later), its
        // channel (URL and token), its page, the owner's two choices. Only over USB. An optional
        // second manager may become the primary on the robot's screen (may_primary).
        const bool managed = req["manager"].is<ArduinoJson::JsonObject>();
        std::string origin = "added";
        if (managed) {
            embody::Manager m;
            if (!embody::managerFromJson(req["manager"], m)) {
                return reply_error("manager: {key (base64 DER), url (ws:// or wss://), token, …}");
            }
            embody::Manager second;
            const bool has_second = req["manager2"].is<ArduinoJson::JsonObject>();
            if (has_second && !embody::managerFromJson(req["manager2"], second)) {
                return reply_error("manager2: {key, url, token, may_primary, …}");
            }
            embody::saveManager("manager", m);
            if (has_second) {
                embody::saveManager("manager2", second);
            } else {
                embody::clearManager("manager2");
            }
            Settings old("embody", true);
            old.EraseKey("managers");  // the earlier list of several
            origin = "manager";
            applied.add("manager");
            embody::managerChanged();  // the channel connects to it now (usb_setup.h)
            mclog::tagInfo(_tag, "manager {} ({}) saved; remote changes {}{}", m.id, m.name, m.remote ? "allowed" : "off",
                           has_second ? "; a second manager" : "");
        }
        // More servers (apps) at once; "pin" makes one of them the default at start. From a
        // manager they are all the robot's apps: the others go.
        if (req["servers"].is<ArduinoJson::JsonArray>()) {
            std::vector<std::string> urls;
            for (ArduinoJson::JsonObject o : req["servers"].as<ArduinoJson::JsonArray>()) {
                if (!save_server(o["name"] | "", o["url"] | "", o["token"] | "", false, origin.c_str())) {
                    return reply_error("server url must start with ws:// or wss://");
                }
                urls.push_back(o["url"] | "");
                mclog::tagInfo(_tag, "server {} saved", std::string(o["url"] | ""));
            }
            if (managed) {
                keep_only(urls);
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
    if (op == "status") {  // read-only: what Embody Mode does now, and the manager settings
        std::string st;
        {
            std::lock_guard<std::mutex> lock(s_pair_mutex);
            st = s_status;
        }
        res["ok"] = true;
        ArduinoJson::JsonDocument embodyDoc;
        if (!st.empty() && !ArduinoJson::deserializeJson(embodyDoc, st)) {
            res["embody"] = embodyDoc;
        } else {
            res["embody"] = nullptr;  // Embody Mode is not running
        }
        add_managers(res);
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
#if CONFIG_STACKCHAN_EMBODY_AUTOMATION
    if (op == "tap") {
        // {"x": 0..319, "y": 0..239, "ms": 30..5000 (default 100; long press ~800)}
        const int x  = req["x"] | -1;
        const int y  = req["y"] | -1;
        const int ms = std::clamp(req["ms"] | 100, 30, 5000);
        if (x < 0 || y < 0 || x >= (int)lv_display_get_horizontal_resolution(nullptr) ||
            y >= (int)lv_display_get_vertical_resolution(nullptr)) {
            return reply_error("x or y outside the screen");
        }
        if (embody::questionOpen()) {
            return reply_error("a question is on the robot's screen: only a person at the robot may answer it");
        }
        if (!usb_control_allowed()) {
            mclog::tagInfo(_tag, "asking on the screen: allow control over USB?");
            if (!embody::askOnScreenAndWait("Let the computer on USB use the screen?",
                                            "It can then tap anything until the cable is unplugged, but never answer "
                                            "questions like this one.",
                                            60)) {
                return reply_error("not allowed on the robot: tap Yes when it asks");
            }
            set_usb_control(true);
        }
        tap(res, x, y, ms);
        mclog::tagInfo(_tag, "tap over USB at {},{} for {} ms", x, y, ms);
        return reply(res);
    }
    if (op == "stall") {  // {"seconds": 1..120}: the app loop stops (tests of the manager channel)
        const int seconds = std::clamp(req["seconds"] | 30, 1, 120);
        s_stall           = seconds;
        mclog::tagWarn(_tag, "stalling the app loop for {} s (over USB)", seconds);
        res["ok"] = true;
        return reply(res);
    }
    if (op == "screenshot") {
        mclog::tagInfo(_tag, "screenshot over USB");
        if (!screenshot(res)) {
            return reply_error("screenshot failed");
        }
        return reply(res);
    }
    if (op == "launch") {
        // {"app": a launcher app's name ("AVATAR", "Embody Mode"…) or "launcher"}: restart into it
        const std::string want = req["app"] | "";
        std::string app        = embody::kLauncherName;
        if (!want.empty() && strcasecmp(want.c_str(), embody::kLauncherName) != 0) {
            const auto apps = mooncake::GetMooncake().getAllAppProps();
            const auto it   = std::find_if(apps.begin(), apps.end(),
                                           [&](const auto& p) { return strcasecmp(p.info.name.c_str(), want.c_str()) == 0; });
            if (it == apps.end()) {
                res["ok"]    = false;
                res["error"] = "no app named " + want;
                auto names   = res["apps"].to<ArduinoJson::JsonArray>();
                for (const auto& p : apps) {
                    names.add(p.info.name);
                }
                return reply(res);
            }
            app = it->info.name;
        }
        res["ok"]  = true;
        res["app"] = app;
        reply(res);
        mclog::tagInfo(_tag, "launch over USB: restarting into {}", app);
        embody::set_launch_once(app);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
#endif
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
#if CONFIG_STACKCHAN_EMBODY_AUTOMATION
    // The Yes for control over USB ends when no USB host is there: at start (after a grace time
    // for the host to enumerate) and whenever the host goes away for a few seconds.
    const TickType_t started = xTaskGetTickCount();
    TickType_t gone_since    = 0;
#endif
    for (;;) {
#if CONFIG_STACKCHAN_EMBODY_AUTOMATION
        if (usb_serial_jtag_is_connected()) {
            gone_since = 0;
        } else if (xTaskGetTickCount() - started > pdMS_TO_TICKS(5000)) {
            if (!gone_since) {
                gone_since = xTaskGetTickCount();
            } else if (xTaskGetTickCount() - gone_since > pdMS_TO_TICKS(3000) && usb_control_allowed()) {
                set_usb_control(false);
            }
        }
#endif
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
