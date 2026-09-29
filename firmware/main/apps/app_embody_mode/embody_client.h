/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
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

class WebSocket;

namespace embody {

/**
 * @brief WebSocket client for stackchan-server (../stackchan-server).
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
    };

    using Telemetry = std::vector<std::pair<std::string, float>>;

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
    // Increments whenever anything shown on screen changes.
    uint32_t revision() const
    {
        return _revision;
    }

    std::function<void(const std::string& command)> onCommand;
    std::function<Telemetry()> collectTelemetry;

private:
    Config _config;
    std::unique_ptr<WebSocket> _ws;

    std::mutex _mutex;
    std::queue<std::string> _inbox;  // text frames from the WebSocket task

    State _state = State::Connecting;
    std::string _status_text;
    std::string _pair_url;
    std::string _pair_code;
    int _viewers        = 0;
    uint32_t _revision  = 0;
    bool _announced     = false;  // "Connecting…" shown before the blocking connect
    uint32_t _backoff   = 1000;
    uint32_t _next_try  = 0;
    uint32_t _last_beat = 0;
    uint32_t _last_tele = 0;

    void set_state(State state, std::string statusText);
    void connect();
    void schedule_retry(State state, std::string statusText, uint32_t delayMs);
    void handle_frame(const std::string& text);
    bool send(const std::string& frame);
    void send_telemetry();
};

}  // namespace embody
