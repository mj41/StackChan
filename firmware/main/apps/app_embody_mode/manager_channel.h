/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

class WebSocket;

namespace embody {

/**
 * @brief The robot's connection to its primary manager (home-w42-eu docs/manager-channel.md),
 *        apart from the app connection: its own task and WebSocket, up first, kept across app
 *        switches, answering even when the app loop hangs.
 *
 * It sends Hello (on connecting) and State (on every change) built from what the app loop sets;
 * it adds "stuck" when the app loop stops beating. Signed frames are checked here (the primary's
 * key, this robot, a seq above the last, kept in NVS): Restart is done here at once; Apps,
 * Switch and Forget go to the app loop (pop), which applies them under the screen lock.
 */
class ManagerChannel {
public:
    struct Message {
        std::string kind;     // Apps, Switch, Forget
        std::string payload;  // the checked JSON
    };

    void start(const std::string& robotId);
    // The primary changed (USB setup, Manager screen): connect anew.
    void reconnect() { _reconnect = true; }
    // Turned off on the Manager screen (already saved): tell the manager, close.
    void turnOff() { _off = true; }
    // The app loop is alive: call every loop.
    void beat();
    // What the app loop reports: state (a JSON object: app, app_name, conn, question, answer,
    // apps_version, firmware) and its apps (a JSON array of {id, name}, for Hello).
    void setState(const std::string& state, const std::string& apps);
    bool pop(Message& out);

    bool connected() const { return _connected; }
    // The manager page's one-time sign-in address (PageCode) for the Manager screen's QR: a phone
    // that scans it is signed in at that manager ("" for none).
    std::string pageUrl() const;
    std::string lastError() const;

private:
    std::string _robot_id;
    std::atomic<bool> _connected{false};
    std::atomic<bool> _reconnect{false};
    std::atomic<bool> _off{false};  // turned off on the robot: say so, then close
    std::atomic<int64_t> _beat_us{0};
    std::atomic<int64_t> _last_rx{0};  // the socket's callbacks: members, they may run as it closes
    std::atomic<bool> _closed{false};
    bool _started = false;

    mutable std::mutex _mu;
    std::string _state, _apps, _sent, _apps_sent, _error, _page_url;
    std::deque<std::string> _inbox;  // raw frames from the socket
    std::deque<Message> _out;        // checked messages for the app loop

    static void task_entry(void* self);
    void run();
    void session();
    bool handle(const std::string& frame);  // true: the manager turned itself off (Disable)
    std::string state_json(bool hello, bool stuck);
};

}  // namespace embody
