/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "manager_channel.h"
#include "managers.h"
#include "automation.h"
#include <board.h>
#include <web_socket.h>
#include <mooncake_log.h>
#include <ArduinoJson.hpp>
#include <mbedtls/base64.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>

namespace embody {

static const char* _tag = "Manager";

static constexpr int64_t _stuck_after_us = 10'000'000;  // no beat from the app loop this long
static constexpr int64_t _silent_us      = 70'000'000;  // nothing from the manager: reconnect
static constexpr int64_t _ping_every_us  = 25'000'000;
static constexpr int _max_backoff_ms     = 60'000;

void ManagerChannel::start(const std::string& robotId)
{
    if (_started) {
        return;
    }
    _started  = true;
    _robot_id = robotId;
    _beat_us  = esp_timer_get_time();
    // Its own task: TLS for a manager on the internet needs a deep stack.
    xTaskCreate(task_entry, "embody_mgr", 10240, this, 4, nullptr);
}

void ManagerChannel::beat()
{
    _beat_us = esp_timer_get_time();
}

void ManagerChannel::setState(const std::string& state, const std::string& apps)
{
    std::lock_guard<std::mutex> lock(_mu);
    _state = state;
    _apps  = apps;
}

bool ManagerChannel::pop(Message& out)
{
    std::lock_guard<std::mutex> lock(_mu);
    if (_out.empty()) {
        return false;
    }
    out = std::move(_out.front());
    _out.pop_front();
    return true;
}

void ManagerChannel::leave(const std::string& to)
{
    if (!_connected) {
        return;  // nobody to tell
    }
    {
        std::lock_guard<std::mutex> lock(_mu);
        _leave_to = to;
    }
    _left  = false;
    _leave = true;
    for (int waited = 0; waited < 2000 && !_left && _connected; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

std::string ManagerChannel::pageUrl() const
{
    std::lock_guard<std::mutex> lock(_mu);
    return _connected ? _page_url : "";
}

std::string ManagerChannel::lastError() const
{
    std::lock_guard<std::mutex> lock(_mu);
    return _error;
}

void ManagerChannel::task_entry(void* self)
{
    static_cast<ManagerChannel*>(self)->run();
}

void ManagerChannel::run()
{
    int backoff = 1000;
    for (;;) {
        const int64_t t0 = esp_timer_get_time();
        session();
        _connected = false;
        if (esp_timer_get_time() - t0 > 60'000'000) {
            backoff = 1000;
        }
        for (int waited = 0; waited < backoff && !_reconnect; waited += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (_reconnect) {
            backoff = 1000;
        } else {
            backoff = std::min(backoff * 2, _max_backoff_ms);
        }
    }
}

// The state as a Hello or State body: the app's state, its apps (Hello), the last seq, stuck.
std::string ManagerChannel::state_json(bool hello, bool stuck)
{
    ArduinoJson::JsonDocument doc;
    {
        std::lock_guard<std::mutex> lock(_mu);
        ArduinoJson::deserializeJson(doc, _state.empty() ? "{}" : _state);
        if (hello || _apps != _apps_sent) {  // on Hello, and whenever the list changed
            ArduinoJson::JsonDocument apps;
            if (!ArduinoJson::deserializeJson(apps, _apps.empty() ? "[]" : _apps)) {
                doc["apps"] = apps;
            }
            _apps_sent = _apps;
        }
    }
    doc["seq"]   = loadManager("manager").seq;
    doc["stuck"] = stuck;
    std::string out;
    ArduinoJson::serializeJson(doc, out);
    return out;
}

void ManagerChannel::session()
{
    _reconnect = false;
    _off       = false;
    const Manager m = loadManager("manager");
    if (!m.valid() || m.token.empty() || !m.enabled) {
        vTaskDelay(pdMS_TO_TICKS(2000));  // none yet (a USB setup gives one), or turned off
        return;
    }
    auto network = Board::GetInstance().GetNetwork();
    std::unique_ptr<WebSocket> ws = network ? network->CreateWebSocket(3) : nullptr;
    if (!ws) {
        return;
    }
    ws->SetHeader("Authorization", ("Bearer " + m.token).c_str());
    ws->SetHeader("X-Device-Id", _robot_id.c_str());
    _last_rx = esp_timer_get_time();
    _closed  = false;
    ws->OnData([this](const char* data, size_t len, bool binary) {
        _last_rx = esp_timer_get_time();
        if (!binary && len > 0 && len < 64 * 1024) {
            std::lock_guard<std::mutex> lock(_mu);
            _inbox.emplace_back(data, len);
        }
    });
    ws->OnDisconnected([this]() { _closed = true; });
    if (!ws->Connect(m.url.c_str())) {
        std::lock_guard<std::mutex> lock(_mu);
        _error = "cannot reach " + m.url;
        mclog::tagWarn(_tag, "cannot reach {}", m.url);
        return;
    }
    _connected = true;
    {
        std::lock_guard<std::mutex> lock(_mu);
        _error.clear();
        _sent.clear();
    }
    mclog::tagInfo(_tag, "channel to {} open", m.name);
    bool stuck_sent = false;
    std::string hello = "{\"kind\":\"Hello\",\"body\":" + state_json(true, false) + "}";
    ws->Send(hello);
    int64_t last_tx = esp_timer_get_time();
    while (!_closed && !_reconnect && ws->IsConnected()) {
        vTaskDelay(pdMS_TO_TICKS(200));
        const int64_t now = esp_timer_get_time();
        for (;;) {
            std::string frame;
            {
                std::lock_guard<std::mutex> lock(_mu);
                if (_inbox.empty()) {
                    break;
                }
                frame = std::move(_inbox.front());
                _inbox.pop_front();
            }
            if (handle(frame)) {
                ws->Send("{\"kind\":\"Off\",\"body\":{\"by\":\"manager\"}}");
                vTaskDelay(pdMS_TO_TICKS(300));
                _connected = false;
                mclog::tagInfo(_tag, "turned off by {}: apps change only over USB now", m.name);
                return;
            }
        }
        if (_leave.exchange(false)) {  // a USB setup gives it another manager: tell this one
            ArduinoJson::JsonDocument doc;
            doc["kind"] = "Leaving";
            {
                std::lock_guard<std::mutex> lock(_mu);
                doc["body"]["to"] = _leave_to;
            }
            std::string frame;
            ArduinoJson::serializeJson(doc, frame);
            ws->Send(frame);
            vTaskDelay(pdMS_TO_TICKS(300));
            _left = true;
            mclog::tagInfo(_tag, "told {}: set up with another manager", m.name);
        }
        if (_off) {  // turned off on the robot's Manager screen
            ws->Send("{\"kind\":\"Off\",\"body\":{\"by\":\"robot\"}}");
            vTaskDelay(pdMS_TO_TICKS(300));
            _connected = false;
            mclog::tagInfo(_tag, "turned off on the robot");
            return;
        }
        const bool stuck = now - _beat_us > _stuck_after_us;
        std::string st;
        {
            std::lock_guard<std::mutex> lock(_mu);
            st = _state;
        }
        bool apps_changed = false;
        {
            std::lock_guard<std::mutex> lock(_mu);
            apps_changed = _apps != _apps_sent;
        }
        if (st != _sent || stuck != stuck_sent || apps_changed) {  // something changed: tell the manager
            {
                std::lock_guard<std::mutex> lock(_mu);
                _sent = st;
            }
            stuck_sent = stuck;
            ws->Send("{\"kind\":\"State\",\"body\":" + state_json(false, stuck) + "}");
            last_tx = now;
            if (stuck) {
                mclog::tagWarn(_tag, "the app loop is not responding");
            }
        } else if (now - last_tx > _ping_every_us) {
            ws->Send("{\"kind\":\"Ping\",\"body\":{}}");
            last_tx = now;
        }
        if (now - _last_rx > _silent_us) {
            mclog::tagWarn(_tag, "nothing from the manager: reconnecting");
            break;
        }
    }
    _connected = false;
    mclog::tagInfo(_tag, "channel closed");
}

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

// A frame from the manager: Signed frames are checked (key, robot, seq) and taken. True: the
// manager turned itself off for this robot (Disable).
bool ManagerChannel::handle(const std::string& frame)
{
    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, frame)) {
        return false;
    }
    if (std::string(doc["kind"] | "") == "PageCode") {  // only an address on its manager's own page
        const std::string url  = doc["body"]["url"] | "";
        const std::string page = loadManager("manager").page;
        if (!page.empty() && url.rfind(page + "/phone?code=", 0) == 0 && url.size() < 200) {
            std::lock_guard<std::mutex> lock(_mu);
            _page_url = url;
        }
        return false;
    }
    if (std::string(doc["kind"] | "") != "Signed") {
        return false;
    }
    const std::string payloadB64 = doc["body"]["payload"] | "";
    const std::string sig        = doc["body"]["sig"] | "";
    std::string payload;
    if (!b64decode(payloadB64, payload)) {
        return false;
    }
    Manager m = loadManager("manager");
    if (!managerSigned(m.key, payload, sig)) {
        mclog::tagWarn(_tag, "refused: not signed by {}", m.name);
        return false;
    }
    ArduinoJson::JsonDocument p;
    if (ArduinoJson::deserializeJson(p, payload) || std::string(p["robot"] | "") != _robot_id ||
        std::string(p["manager"] | "") != m.id) {
        mclog::tagWarn(_tag, "refused: not for this robot");
        return false;
    }
    const int32_t seq      = p["seq"] | 0;
    const std::string kind = p["kind"] | "";
    bool fresh             = false;
    updateManager("manager", [&](Manager& cur) {
        if (cur.id == m.id && seq > cur.seq) {
            cur.seq = seq;
            fresh   = true;
        }
    });
    if (!fresh) {
        mclog::tagWarn(_tag, "refused: seq {} not newer", seq);
        return false;
    }
    if (kind == "Disable") {  // it may turn itself off (never on: that is the robot's, or USB's)
        updateManager("manager", [](Manager& cur) { cur.enabled = false; });
        return true;
    }
    if (kind == "Restart") {  // here, not in the app loop: it may be the one that hangs
        mclog::tagWarn(_tag, "restart asked by {}", m.name);
        embody::set_launch_once(embody::kEmbodyAppName);  // back into Embody Mode, not the launcher
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    if (!m.remote) {
        mclog::tagWarn(_tag, "refused: {} changes go over USB only", kind);
        return false;
    }
    std::lock_guard<std::mutex> lock(_mu);
    _out.push_back({kind, payload});
    return false;
}

}  // namespace embody
