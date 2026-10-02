/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct ble_gap_event;

namespace embody {

/**
 * @brief Optional TPBot car: a micro:bit V2 running ../tpbot-ble firmware,
 * reached over BLE (NimBLE central).
 *
 * The protocol (service, command opcodes, the 14-byte state) is tpbot-ble's
 * proto package; its readme has the tables. start() brings up NimBLE and keeps
 * scanning for "TPB-*" and reconnecting. NimBLE callbacks run in its host task;
 * the app loop reads the results with take().
 */
class CarBle {
public:
    struct State {
        uint8_t seq      = 0;
        uint32_t ms      = 0;  // micro:bit uptime
        uint16_t echo_us = 0;  // sonar echo pulse, 0 = none
        uint8_t inputs   = 0;  // bit 0/1 line L/R (raw), bit 2/3 button A/B pressed
        int8_t left      = 0;  // motor speed in effect
        int8_t right     = 0;
        uint8_t flags    = 0;  // bit 0 the watchdog stopped the motors
        uint8_t i2c_err  = 0;
        uint8_t board    = 0;
    };
    struct Event {
        std::string name;  // "car_connected" or "car_disconnected"
        std::string car;   // advertised name, e.g. TPB-1a2b
        std::string addr;
        int reason = 0;
    };

    // board: 0 both, 1 V1, 2 V2 (sent after every connect).
    void start(uint8_t board);
    void stop();

    bool connected() const;
    int rssi();  // dBm of the link, 0 when unknown
    // Called from the app loop: drops a link that went quiet. Returns events since the last call.
    std::vector<Event> poll();
    // The newest state; fresh is set once per new notification.
    State state(bool* fresh = nullptr);

    // Commands (tpbot-ble proto). false when not connected.
    bool drive(int left, int right);
    bool stopMotors();
    bool servo(int port, int angle);
    bool headlights(uint8_t r, uint8_t g, uint8_t b);
    bool sonar(int hz);
    bool watchdog(int ms);
    bool board(uint8_t mode);

    // NimBLE glue, public for the C callbacks.
    void on_sync();
    int on_gap(::ble_gap_event* event);
    void on_service(uint16_t start, uint16_t end);
    void on_chr(uint16_t def_handle, uint16_t val_handle, int which);
    void on_chrs_done(int status);
    void on_dsc(uint16_t handle, bool cccd);
    void on_dscs_done(int status);
    void on_subscribed(int status);

private:
    bool send(const uint8_t* data, size_t len);
    void start_scan();
    void fail(const char* what, int rc);
    void push_event(Event e);

    mutable std::mutex _mutex;
    bool _started      = false;
    bool _running      = false;  // start() called and not stopped
    uint8_t _own_addr  = 0;
    uint8_t _board     = 1;
    uint16_t _conn     = 0xFFFF;  // BLE_HS_CONN_HANDLE_NONE
    bool _ready        = false;   // subscribed: commands may go out
    uint16_t _svc_end  = 0;
    uint16_t _cmd_val  = 0;
    uint16_t _state_val = 0;
    uint16_t _state_def_next = 0;  // the declaration handle after the state characteristic
    uint16_t _cccd     = 0;
    std::string _name, _addr;
    State _state;
    bool _fresh        = false;
    uint32_t _last_note_ms = 0;
    std::vector<Event> _events;
};

}  // namespace embody
