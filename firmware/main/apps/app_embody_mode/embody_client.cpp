/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "embody_client.h"
#include <hal/hal.h>
#include <board.h>
#include <web_socket.h>
#include <mooncake_log.h>
#include <ArduinoJson.hpp>
#include <algorithm>

using namespace embody;

static const char* _tag = "Embody";

static constexpr uint32_t _heartbeat_interval = 30000;
static constexpr uint32_t _telemetry_interval = 2000;
static constexpr uint32_t _max_backoff        = 30000;

Client::Client(Config config) : _config(std::move(config))
{
    set_state(State::Connecting, "Connecting to server...");
}

Client::~Client()
{
    // Destroy the socket first: its callbacks capture this.
    _ws.reset();
}

void Client::set_state(State state, std::string statusText)
{
    if (_state == state && _status_text == statusText) {
        return;
    }
    _state       = state;
    _status_text = std::move(statusText);
    if (_state != State::Registered) {
        _pair_url.clear();
        _pair_code.clear();
        _viewers = 0;
    }
    _revision++;
}

void Client::schedule_retry(State state, std::string statusText, uint32_t delayMs)
{
    set_state(state, std::move(statusText));
    _next_try  = GetHAL().millis() + delayMs;
    _announced = false;
}

void Client::update()
{
    auto now = GetHAL().millis();

    if (!_ws || !_ws->IsConnected()) {
        if (_state == State::Registered) {
            mclog::tagWarn(_tag, "connection lost");
            schedule_retry(State::Offline, "Connection lost. Reconnecting...", _backoff);
        }
        if ((int32_t)(now - _next_try) < 0) {
            return;
        }
        // Show "Connecting..." for one frame before blocking in connect().
        if (!_announced) {
            _announced = true;
            set_state(State::Connecting, "Connecting to server...");
            return;
        }
        connect();
        return;
    }

    std::queue<std::string> inbox;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::swap(inbox, _inbox);
    }
    while (!inbox.empty()) {
        handle_frame(inbox.front());
        inbox.pop();
    }

    if (_state != State::Registered) {
        return;
    }
    if (now - _last_beat >= _heartbeat_interval) {
        _last_beat = now;
        ArduinoJson::JsonDocument doc;
        doc["kind"] = "Heartbeat";
        doc["meta"].to<ArduinoJson::JsonObject>();
        doc["body"].to<ArduinoJson::JsonObject>();
        std::string frame;
        ArduinoJson::serializeJson(doc, frame);
        send(frame);
    }
    if (now - _last_tele >= _telemetry_interval) {
        _last_tele = now;
        send_telemetry();
    }
}

void Client::connect()
{
    _ws.reset();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _inbox = {};
    }

    auto url = _config.serverUrl + "/api/workers/connect";
    mclog::tagInfo(_tag, "connecting to {} as {}", url, _config.robotId);

    auto network = Board::GetInstance().GetNetwork();
    _ws          = network->CreateWebSocket(2);
    if (!_ws) {
        schedule_retry(State::Offline, "Network error. Retrying...", _max_backoff);
        return;
    }
    _ws->SetHeader("Authorization", ("Bearer " + _config.token).c_str());
    _ws->SetHeader("X-Yolovm-Worker-Id", _config.robotId.c_str());
    _ws->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            return;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _inbox.emplace(data, len);
    });

    if (!_ws->Connect(url.c_str())) {
        mclog::tagWarn(_tag, "connect failed, retry in {} ms", _backoff);
        schedule_retry(State::Offline, "Server unreachable. Retrying...", _backoff);
        _backoff = std::min(_backoff * 2, _max_backoff);
        return;
    }

    // Register must be the first frame.
    ArduinoJson::JsonDocument doc;
    doc["kind"]              = "Register";
    doc["meta"]["worker_id"] = _config.robotId;
    auto body                = doc["body"].to<ArduinoJson::JsonObject>();
    body["class"]            = "robot";
    auto caps                = body["capabilities"].to<ArduinoJson::JsonObject>();
    caps["model"]            = _config.model;
    caps["firmware"]         = _config.firmware;
    auto commands            = caps["commands"].to<ArduinoJson::JsonArray>();
    for (const auto& c : _config.commands) {
        commands.add(c);
    }
    auto measurements = caps["measurements"].to<ArduinoJson::JsonArray>();
    for (const auto& m : _config.measurements) {
        measurements.add(m);
    }
    std::string frame;
    ArduinoJson::serializeJson(doc, frame);
    send(frame);
}

void Client::handle_frame(const std::string& text)
{
    ArduinoJson::JsonDocument doc;
    if (auto err = ArduinoJson::deserializeJson(doc, text); err) {
        mclog::tagWarn(_tag, "bad frame: {}", err.c_str());
        return;
    }
    std::string kind = doc["kind"] | "";
    auto body        = doc["body"];

    if (kind == "Accepted") {
        mclog::tagInfo(_tag, "registered");
        _backoff   = 1000;
        _last_beat = GetHAL().millis();
        set_state(State::Registered, "Scan the QR code to pair");
    } else if (kind == "Rejected") {
        std::string reason = body["reason"] | "unknown reason";
        mclog::tagError(_tag, "rejected: {}", reason);
        _ws.reset();
        schedule_retry(State::Rejected, "Server rejected: " + reason, _max_backoff);
    } else if (kind == "PairCode") {
        _pair_url  = body["url"] | "";
        _pair_code = body["code"] | "";
        mclog::tagInfo(_tag, "pair code {} -> {}", _pair_code, _pair_url);
        _revision++;
    } else if (kind == "Paired") {
        _viewers     = body["viewers"] | 0;
        _status_text = _viewers == 1 ? "Paired with 1 browser" : fmt::format("Paired with {} browsers", _viewers);
        _revision++;
    } else if (kind == "RobotCommand") {
        std::string command = body["command"] | "";
        mclog::tagInfo(_tag, "command: {}", command);
        if (onCommand && !command.empty()) {
            onCommand(command);
        }
    }
}

bool Client::send(const std::string& frame)
{
    if (!_ws || !_ws->IsConnected()) {
        return false;
    }
    return _ws->Send(frame);
}

void Client::send_telemetry()
{
    if (!collectTelemetry) {
        return;
    }
    ArduinoJson::JsonDocument doc;
    doc["kind"] = "RobotTelemetry";
    doc["meta"].to<ArduinoJson::JsonObject>();
    auto measurements = doc["body"]["measurements"].to<ArduinoJson::JsonObject>();
    for (const auto& [key, value] : collectTelemetry()) {
        measurements[key] = value;
    }
    std::string frame;
    ArduinoJson::serializeJson(doc, frame);
    send(frame);
}
