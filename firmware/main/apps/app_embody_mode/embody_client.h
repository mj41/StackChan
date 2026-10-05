/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>
#include <ArduinoJson.hpp>

class WebSocket;

namespace embody {

class E2E;

/**
 * @brief WebSocket client for an Embody Mode server, e.g. s-w42-eu-raw
 *        (https://github.com/mj41/s-w42-eu-raw).
 *
 * Frames are JSON {kind, meta, body}; see the protocol table in the server's
 * readme.md. Driven from the app loop: update() may block while connecting.
 */
class Client {
public:
    enum class State { Connecting, Registered, Offline, Rejected };

    struct Config {
        std::string serverUrl;  // e.g. ws://192.168.1.10:8765
        std::string token;
        std::string robotId;
        std::string model;
        std::string firmware;
        std::vector<std::string> commands;
        std::vector<std::string> measurements;
        // End-to-end encryption for this server (e2e.h; nullptr: plaintext). Owned by the app.
        E2E* e2e = nullptr;
    };

    using Telemetry = std::vector<std::pair<std::string, double>>;  // double: Unix times need it
    using Texts     = std::vector<std::pair<std::string, std::string>>;

    explicit Client(Config config);
    ~Client();

    void update();

    State state() const
    {
        return _state;
    }
    const std::string& statusText() const
    {
        return _status_text;
    }
    const std::string& pairUrl() const
    {
        return _pair_url;
    }
    const std::string& pairCode() const
    {
        return _pair_code;
    }
    int viewers() const
    {
        return _viewers;
    }
    // The last Paired only said "browsers were paired before" (on reconnect), not a new scan.
    bool pairedOnReconnect() const
    {
        return _paired_on_reconnect;
    }
    // Increments whenever anything shown on screen changes.
    uint32_t revision() const
    {
        return _revision;
    }
    // Increments for every RobotCommand received, including ping (counts as use).
    uint32_t commandCount() const
    {
        return _command_count;
    }

    // args is the command's JSON "args" object (or "{}"). "ping" is answered here.
    std::function<void(const std::string& command, const std::string& args)> onCommand;
    // Commands that must not wait for the app loop (driving the car). Called in the
    // socket task for "car_*" commands, before onCommand: return true if handled.
    // It must be thread-safe; anything else goes through onCommand as usual.
    std::function<bool(const std::string& command, const std::string& args)> onFastCommand;
    std::function<Telemetry()> collectTelemetry;

    // Binary messages from the server (e.g. 0x10 picture): type byte, then payload.
    std::function<void(uint8_t type, const std::string& payload)> onBinary;

    // Report something that happened on the robot, e.g. "shake", optionally with
    // numeric data such as {x, y} and text data such as {uid}. Dropped while offline.
    void sendEvent(const std::string& name, const Telemetry& data = {}, const Texts& text = {});

    // Send telemetry now, besides the periodic collectTelemetry (e.g. a car's sensors). Main loop only.
    void sendTelemetry(const Telemetry& t);
    // Send a binary message (type byte + payload), e.g. a camera frame. Main loop only.
    bool sendBinary(uint8_t type, const uint8_t* data, size_t len);

    bool isRegistered() const
    {
        return _state == State::Registered;
    }
    // The command now in onCommand came sealed by an enrolled browser (not from the relay).
    bool lastCommandSealed() const
    {
        return _last_sealed;
    }
    bool encrypted() const
    {
        return _config.e2e != nullptr;
    }

    // Standby: disconnect now and stay offline for delayMs (then reconnect as usual).
    void standby(uint32_t delayMs);
    // End standby early: reconnect on the next update().
    void wakeNow();

private:
    Config _config;
    std::unique_ptr<WebSocket> _ws;

    std::mutex _mutex;
    struct Inbound {
        std::string text;  // JSON, or type byte + payload when binary
        int64_t rxUs;      // esp_timer time the frame arrived, for ping queue time
        bool binary;
    };
    std::queue<Inbound> _inbox;  // text frames from the WebSocket task

    State _state = State::Connecting;
    std::string _status_text;
    std::string _pair_url;
    std::string _pair_base;  // the server's pairing URL, before the robot adds the e2e fragment
    bool _last_sealed = false;
    bool _plain_binary_logged = false;
    std::string _pair_code;
    int _viewers        = 0;
    bool _paired_on_reconnect = false;
    uint32_t _revision  = 0;
    std::atomic<uint32_t> _command_count{0};
    bool _announced     = false;  // "Connecting…" shown before the blocking connect
    uint32_t _backoff   = 1000;
    uint32_t _next_try  = 0;
    uint32_t _last_beat = 0;
    uint32_t _last_tele = 0;

    void set_state(State state, std::string statusText);
    void connect();
    void schedule_retry(State state, std::string statusText, uint32_t delayMs);
    bool fast_command(const char* data, size_t len);
    void handle_frame(const Inbound& in);
    bool send(const std::string& frame);
    // Telemetry, events and pongs: sealed as E2EData when encrypted.
    bool send_report(ArduinoJson::JsonDocument& doc);
    void handle_command(const std::string& command, ArduinoJson::JsonVariantConst args, int64_t rxUs);
    void send_telemetry();
};

}  // namespace embody
