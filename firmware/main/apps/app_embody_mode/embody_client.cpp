/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "embody_client.h"
#include "e2e.h"
#include <cstring>
#include <hal/hal.h>
#include <board.h>
#include <web_socket.h>
#include <mooncake_log.h>
#include <ArduinoJson.hpp>
#include <esp_timer.h>
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

    std::queue<Inbound> inbox;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::swap(inbox, _inbox);
    }
    while (!inbox.empty()) {
        const auto& in = inbox.front();
        if (in.binary && _config.e2e) {
            // Encrypted: only sealed messages from an enrolled browser (0x31); the relay could
            // inject plaintext pictures, files or audio.
            std::string plain;
            if ((uint8_t)in.text[0] == 0x31 && _config.e2e->openBrowserBinary(in.text, plain) && !plain.empty()) {
                if (onBinary) {
                    onBinary((uint8_t)plain[0], plain.substr(1));
                }
            } else if (!_plain_binary_logged) {
                _plain_binary_logged = true;
                mclog::tagWarn(_tag, "e2e: plaintext binary from the relay refused (type 0x{:02x})", (uint8_t)in.text[0]);
            }
        } else if (in.binary) {
            if (onBinary) {
                onBinary((uint8_t)in.text[0], in.text.substr(1));
            }
        } else {
            handle_frame(in);
        }
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

    auto url = _config.serverUrl + "/api/devices/connect";
    mclog::tagInfo(_tag, "connecting to {} as {}", url, _config.robotId);

    auto network = Board::GetInstance().GetNetwork();
    _ws          = network->CreateWebSocket(2);
    if (!_ws) {
        schedule_retry(State::Offline, "Network error. Retrying...", _max_backoff);
        return;
    }
    _ws->SetHeader("Authorization", ("Bearer " + _config.token).c_str());
    _ws->SetHeader("X-Device-Id", _config.robotId.c_str());
    _ws->OnData([this](const char* data, size_t len, bool binary) {
        if (len == 0 || len > 256 * 1024) {
            return;
        }
        if (!binary && onFastCommand && fast_command(data, len)) {
            return;
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _inbox.push({std::string(data, len), esp_timer_get_time(), binary});
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
    if (_config.e2e) {
        body["labels"]["e2e"] = "1";
    }
    if (!_config.appsVersions.empty()) {
        body["labels"]["apps_ver"] = _config.appsVersions;
    }
    std::string frame;
    ArduinoJson::serializeJson(doc, frame);
    send(frame);
}

// Socket task: handles a "car_*" RobotCommand at once (the app loop can be busy for
// hundreds of ms with camera frames and drawing). Other frames return false.
bool Client::fast_command(const char* data, size_t len)
{
    if (_config.e2e) {
        return false;  // encrypted: car commands come sealed, through the app loop
    }
    static constexpr char key[] = "\"car_";
    if (!memmem(data, len, key, sizeof key - 1)) {
        return false;
    }
    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, data, len) || !doc.is<ArduinoJson::JsonObject>() ||
        std::string(doc["kind"] | "") != "RobotCommand") {
        return false;
    }
    const std::string command = doc["body"]["command"] | "";
    if (command.rfind("car_", 0) != 0) {
        return false;
    }
    std::string args = "{}";
    if (doc["body"]["args"].is<ArduinoJson::JsonObject>()) {
        args.clear();
        ArduinoJson::serializeJson(doc["body"]["args"], args);
    }
    if (!onFastCommand(command, args)) {
        return false;
    }
    _command_count++;
    return true;
}

void Client::handle_frame(const Inbound& in)
{
    ArduinoJson::JsonDocument doc;
    if (auto err = ArduinoJson::deserializeJson(doc, in.text); err) {
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
        _pair_base = body["url"] | "";
        _pair_url  = _config.e2e ? _pair_base + "#" + _config.e2e->fragment() : _pair_base;
        _pair_code = body["code"] | "";
        mclog::tagInfo(_tag, "pair code {} -> {}", _pair_code, _pair_base);  // never the e2e fragment: it holds the pairing secret
        _revision++;
    } else if (kind == "Paired") {
        _viewers             = body["viewers"] | 0;
        _paired_on_reconnect = body["reconnect"] | false;
        // What the person sees: who has the app open now (not every browser that ever paired).
        const int watching = body["watching"] | 0;
        _status_text       = watching > 0 ? fmt::format("{} watching now", watching) : "Paired: nobody watching now";
        _revision++;
    } else if (kind == "ManagedApps") {
        if (onManagedApps) {
            onManagedApps(body["payload"] | "", body["sig"] | "");
        }
    } else if (kind == "RobotCommand") {
        std::string command = body["command"] | "";
        // Encrypted: the relay may only switch streams; what streams stays sealed.
        static const char* switches[] = {"camera", "mic", "imu_stream", "touch_stream", "light_stream"};
        if (_config.e2e && std::none_of(std::begin(switches), std::end(switches), [&](const char* s) { return command == s; })) {
            mclog::tagWarn(_tag, "e2e: plaintext command refused: {}", command);
            return;
        }
        _last_sealed = false;
        handle_command(command, body["args"], in.rxUs);
    } else if (_config.e2e && (kind == "E2EEnroll" || kind == "E2EHello")) {
        std::string gk;
        bool ok = kind == "E2EEnroll" ? _config.e2e->enroll(body["b"] | "", body["mac"] | "", gk)
                                      : _config.e2e->hello(body["b"] | "", gk);
        if (!ok) {
            return;
        }
        send("{\"kind\":\"E2EGroupKey\",\"meta\":{},\"body\":" + gk + "}");
        if (kind == "E2EEnroll" && !_pair_base.empty()) {  // a fresh secret in the QR code
            _pair_url = _pair_base + "#" + _config.e2e->fragment();
            _revision++;
        }
    } else if (_config.e2e && kind == "E2ECommand") {
        std::string plain;
        if (!_config.e2e->openCommand(body["b"] | "", body["n"] | "", body["c"] | "", plain)) {
            mclog::tagWarn(_tag, "e2e: sealed command refused");
            return;
        }
        ArduinoJson::JsonDocument cmd;
        if (ArduinoJson::deserializeJson(cmd, plain)) {
            return;
        }
        _last_sealed = true;
        handle_command(cmd["command"] | "", cmd["args"], in.rxUs);
        _last_sealed = false;
    }
}

void Client::handle_command(const std::string& command, ArduinoJson::JsonVariantConst args, int64_t rxUs)
{
    _command_count++;
    if (command == "ping") {
        // Answer right here: queue_ms is how long the ping waited for the app loop.
        ArduinoJson::JsonDocument pong;
        pong["kind"] = "RobotPong";
        pong["meta"].to<ArduinoJson::JsonObject>();
        pong["body"]["id"]       = args["id"] | "";
        pong["body"]["queue_ms"] = (esp_timer_get_time() - rxUs) / 1000.0;
        send_report(pong);
        return;
    }
    mclog::tagInfo(_tag, "command: {}{}", command, _last_sealed ? " (sealed)" : "");
    if (onCommand && !command.empty()) {
        std::string a = "{}";
        if (args.is<ArduinoJson::JsonObjectConst>()) {
            a.clear();
            ArduinoJson::serializeJson(args, a);
        }
        onCommand(command, a);
    }
}

bool Client::send_report(ArduinoJson::JsonDocument& doc)
{
    if (!_config.e2e) {
        std::string frame;
        ArduinoJson::serializeJson(doc, frame);
        return send(frame);
    }
    ArduinoJson::JsonDocument plain;
    plain["kind"] = doc["kind"];
    plain["body"] = doc["body"];
    std::string p, sealed;
    ArduinoJson::serializeJson(plain, p);
    if (!_config.e2e->sealData(p, sealed)) {
        return false;
    }
    return send("{\"kind\":\"E2EData\",\"meta\":{},\"body\":" + sealed + "}");
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
    if (collectTelemetry) {
        sendTelemetry(collectTelemetry());
    }
}

void Client::sendTelemetry(const Telemetry& t)
{
    if (_state != State::Registered) {
        return;
    }
    ArduinoJson::JsonDocument doc;
    doc["kind"] = "RobotTelemetry";
    doc["meta"].to<ArduinoJson::JsonObject>();
    auto measurements = doc["body"]["measurements"].to<ArduinoJson::JsonObject>();
    for (const auto& [key, value] : t) {
        measurements[key] = value;
    }
    send_report(doc);
}

void Client::sendAppsVersion(const std::string& versions)
{
    if (_state != State::Registered) {
        return;
    }
    ArduinoJson::JsonDocument doc;
    doc["kind"] = "AppsVersion";
    doc["meta"].to<ArduinoJson::JsonObject>();
    doc["body"]["versions"] = versions;
    std::string frame;
    ArduinoJson::serializeJson(doc, frame);
    send(frame);
}

void Client::sendEvent(const std::string& name, const Telemetry& data, const Texts& text)
{
    if (_state != State::Registered) {
        return;
    }
    ArduinoJson::JsonDocument doc;
    doc["kind"] = "RobotEvent";
    doc["meta"].to<ArduinoJson::JsonObject>();
    doc["body"]["name"] = name;
    if (!data.empty() || !text.empty()) {
        auto obj = doc["body"]["data"].to<ArduinoJson::JsonObject>();
        for (const auto& [key, value] : data) {
            obj[key] = value;
        }
        for (const auto& [key, value] : text) {
            obj[key] = value;
        }
    }
    send_report(doc);
}

bool Client::sendBinary(uint8_t type, const uint8_t* data, size_t len)
{
    if (_state != State::Registered || !_ws || !_ws->IsConnected()) {
        return false;
    }
    if (_config.e2e) {
        std::string sealed;
        if (!_config.e2e->sealBinary(type, data, len, sealed)) {
            return false;
        }
        if (sealed.size() > 65535) {  // xiaozhi WebSocket::Send limit
            mclog::tagWarn(_tag, "binary message too large: {} bytes", sealed.size());
            return false;
        }
        return _ws->Send(sealed.data(), sealed.size(), true);
    }
    if (len + 1 > 65535) {  // xiaozhi WebSocket::Send limit
        mclog::tagWarn(_tag, "binary message too large: {} bytes", len + 1);
        return false;
    }
    std::string msg;
    msg.reserve(len + 1);
    msg.push_back((char)type);
    msg.append((const char*)data, len);
    return _ws->Send(msg.data(), msg.size(), true);
}

void Client::standby(uint32_t delayMs)
{
    mclog::tagInfo(_tag, "standby for {} s", delayMs / 1000);
    _ws.reset();
    _backoff = 1000;
    schedule_retry(State::Offline, "Standby", delayMs);
}

void Client::wakeNow()
{
    if (_state != State::Registered) {
        _next_try  = GetHAL().millis();
        _announced = false;
    }
}
