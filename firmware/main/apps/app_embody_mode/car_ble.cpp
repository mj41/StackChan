/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "car_ble.h"
#include <cstring>
#include <esp_timer.h>
#include <mooncake_log.h>
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

namespace embody {

static const std::string_view _tag = "car";

// tpbot-ble proto: UUIDs 3d44000X-87fb-42e7-9a68-7596af5168a0, little endian here.
#define TPBOT_UUID(n) \
    BLE_UUID128_INIT(0xa0, 0x68, 0x51, 0xaf, 0x96, 0x75, 0x68, 0x9a, 0xe7, 0x42, 0xfb, 0x87, n, 0x00, 0x44, 0x3d)
static const ble_uuid128_t _svc_uuid   = TPBOT_UUID(0x01);
static const ble_uuid128_t _cmd_uuid   = TPBOT_UUID(0x02);
static const ble_uuid128_t _state_uuid = TPBOT_UUID(0x03);
static const ble_uuid16_t _cccd_uuid   = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);

static constexpr const char* _name_prefix = "TPB-";
static constexpr uint8_t _op_drive = 0x01, _op_stop = 0x02, _op_servo = 0x03, _op_headlights = 0x04,
                         _op_watchdog = 0x05, _op_sonar = 0x06, _op_board = 0x07;
static constexpr size_t _state_len    = 14;
static constexpr uint8_t _state_format = 2;
static constexpr uint32_t _silence_ms = 1500;  // the micro:bit notifies at least every 200 ms

static CarBle* _car = nullptr;  // NimBLE has one host: one car

static uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------ C callbacks ------------------------------- */

static void host_task(void*)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync_cb()
{
    if (_car) {
        _car->on_sync();
    }
}

static void on_reset_cb(int reason)
{
    mclog::tagWarn(_tag, "NimBLE reset, reason {}", reason);
}

static int gap_cb(::ble_gap_event* event, void*)
{
    return _car ? _car->on_gap(event) : 0;
}

static int svc_cb(uint16_t, const struct ble_gatt_error* error, const struct ble_gatt_svc* svc, void*)
{
    if (!_car) {
        return 0;
    }
    if (error->status == 0) {
        _car->on_service(svc->start_handle, svc->end_handle);
    } else if (error->status != BLE_HS_EDONE) {
        _car->on_chrs_done(error->status);
    }
    return 0;
}

static int chr_cb(uint16_t, const struct ble_gatt_error* error, const struct ble_gatt_chr* chr, void*)
{
    if (!_car) {
        return 0;
    }
    if (error->status == 0) {
        int which = 0;
        if (ble_uuid_cmp(&chr->uuid.u, &_cmd_uuid.u) == 0) {
            which = 1;
        } else if (ble_uuid_cmp(&chr->uuid.u, &_state_uuid.u) == 0) {
            which = 2;
        }
        _car->on_chr(chr->def_handle, chr->val_handle, which);
    } else {
        _car->on_chrs_done(error->status == BLE_HS_EDONE ? 0 : error->status);
    }
    return 0;
}

static int dsc_cb(uint16_t, const struct ble_gatt_error* error, uint16_t, const struct ble_gatt_dsc* dsc, void*)
{
    if (!_car) {
        return 0;
    }
    if (error->status == 0) {
        _car->on_dsc(dsc->handle, ble_uuid_cmp(&dsc->uuid.u, &_cccd_uuid.u) == 0);
    } else {
        _car->on_dscs_done(error->status == BLE_HS_EDONE ? 0 : error->status);
    }
    return 0;
}

static int subscribe_cb(uint16_t, const struct ble_gatt_error* error, struct ble_gatt_attr*, void*)
{
    if (_car) {
        _car->on_subscribed(error->status);
    }
    return 0;
}

/* --------------------------------- public --------------------------------- */

void CarBle::start(uint8_t board)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _board   = board;
        _running = true;
        _car     = this;
    }
    if (_started) {
        start_scan();
        return;
    }
    if (nimble_port_init() != ESP_OK) {
        mclog::tagError(_tag, "nimble_port_init failed");
        return;
    }
    ble_hs_cfg.sync_cb  = on_sync_cb;
    ble_hs_cfg.reset_cb = on_reset_cb;
    _started            = true;
    nimble_port_freertos_init(host_task);
    mclog::tagInfo(_tag, "BLE started, looking for {}*", _name_prefix);
}

void CarBle::stop()
{
    uint16_t conn;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _running = false;
        conn     = _conn;
    }
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        stopMotors();
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (_started) {
        ble_gap_disc_cancel();
    }
}

bool CarBle::connected() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _ready;
}

int CarBle::rssi()
{
    uint16_t conn;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        conn = _ready ? _conn : BLE_HS_CONN_HANDLE_NONE;
    }
    int8_t v = 0;
    if (conn == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_rssi(conn, &v) != 0) {
        return 0;
    }
    return v;
}

std::vector<CarBle::Event> CarBle::poll()
{
    uint16_t quiet = BLE_HS_CONN_HANDLE_NONE;
    std::vector<Event> out;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_ready && now_ms() - _last_note_ms > _silence_ms) {
            quiet = _conn;
        }
        out.swap(_events);
    }
    if (quiet != BLE_HS_CONN_HANDLE_NONE) {
        mclog::tagWarn(_tag, "no state for {} ms: dropping the link", _silence_ms);
        ble_gap_terminate(quiet, BLE_ERR_REM_USER_CONN_TERM);
    }
    return out;
}

CarBle::State CarBle::state(bool* fresh)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (fresh) {
        *fresh = _fresh;
    }
    _fresh = false;
    return _state;
}

static uint8_t clamp(int v, int lo, int hi)
{
    return (uint8_t)(v < lo ? lo : v > hi ? hi : v);
}

static uint8_t speed(int v)
{
    return (uint8_t)(int8_t)(v < -100 ? -100 : v > 100 ? 100 : v);
}

bool CarBle::drive(int left, int right)
{
    const uint8_t b[] = {_op_drive, speed(left), speed(right)};
    return send(b, sizeof b);
}

bool CarBle::stopMotors()
{
    const uint8_t b[] = {_op_stop};
    return send(b, sizeof b);
}

bool CarBle::servo(int port, int angle)
{
    const uint8_t b[] = {_op_servo, clamp(port, 1, 4), clamp(angle, 0, 180)};
    return send(b, sizeof b);
}

bool CarBle::headlights(uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t m[] = {_op_headlights, r, g, b};
    return send(m, sizeof m);
}

bool CarBle::sonar(int hz)
{
    const uint8_t b[] = {_op_sonar, clamp(hz, 0, 20)};
    return send(b, sizeof b);
}

bool CarBle::watchdog(int ms)
{
    ms                = ms < 100 ? 100 : ms > 10000 ? 10000 : ms;
    const uint8_t b[] = {_op_watchdog, (uint8_t)(ms & 0xFF), (uint8_t)(ms >> 8)};
    return send(b, sizeof b);
}

bool CarBle::board(uint8_t mode)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _board = mode;
    }
    const uint8_t b[] = {_op_board, mode};
    return send(b, sizeof b);
}

bool CarBle::send(const uint8_t* data, size_t len)
{
    uint16_t conn, handle;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_ready) {
            return false;
        }
        conn   = _conn;
        handle = _cmd_val;
    }
    return ble_gattc_write_no_rsp_flat(conn, handle, data, len) == 0;
}

/* --------------------------------- NimBLE --------------------------------- */

void CarBle::on_sync()
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &_own_addr);
    start_scan();
}

void CarBle::start_scan()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_running || _conn != BLE_HS_CONN_HANDLE_NONE || !ble_hs_synced()) {
            return;
        }
    }
    if (ble_gap_disc_active()) {
        return;
    }
    struct ble_gap_disc_params params = {};
    params.passive           = 1;  // the name is in the advertisement itself
    params.filter_duplicates = 1;
    params.itvl              = BLE_GAP_SCAN_ITVL_MS(200);
    params.window            = BLE_GAP_SCAN_WIN_MS(60);  // leave the radio mostly to Wi-Fi
    const int rc = ble_gap_disc(_own_addr, BLE_HS_FOREVER, &params, gap_cb, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        mclog::tagWarn(_tag, "scan failed: {}", rc);
    }
}

void CarBle::fail(const char* what, int rc)
{
    mclog::tagWarn(_tag, "{} failed: {}", what, rc);
    uint16_t conn;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        conn = _conn;
    }
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void CarBle::push_event(Event e)
{
    if (_events.size() < 8) {
        _events.push_back(std::move(e));
    }
}

int CarBle::on_gap(::ble_gap_event* event)
{
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0 || !fields.name ||
                fields.name_len < strlen(_name_prefix) ||
                memcmp(fields.name, _name_prefix, strlen(_name_prefix)) != 0) {
                return 0;
            }
            char addr[18];
            const uint8_t* a = event->disc.addr.val;
            snprintf(addr, sizeof addr, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (!_running || _conn != BLE_HS_CONN_HANDLE_NONE) {
                    return 0;
                }
                _name.assign((const char*)fields.name, fields.name_len);
                _addr = addr;
            }
            mclog::tagInfo(_tag, "found {} {} ({} dBm), connecting", _name, _addr, event->disc.rssi);
            ble_gap_disc_cancel();
            const int rc = ble_gap_connect(_own_addr, &event->disc.addr, 5000, nullptr, gap_cb, nullptr);
            if (rc != 0) {
                mclog::tagWarn(_tag, "connect failed: {}", rc);
                start_scan();
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            start_scan();
            return 0;
        case BLE_GAP_EVENT_CONNECT: {
            if (event->connect.status != 0) {
                mclog::tagWarn(_tag, "connect status {}", event->connect.status);
                start_scan();
                return 0;
            }
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _conn      = event->connect.conn_handle;
                _ready     = false;
                _cmd_val   = _state_val = _cccd = _svc_end = _state_def_next = 0;
                _last_note_ms = now_ms();
            }
            const int rc = ble_gattc_disc_svc_by_uuid(event->connect.conn_handle, &_svc_uuid.u, svc_cb, nullptr);
            if (rc != 0) {
                fail("service discovery", rc);
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISCONNECT: {
            Event e{"car_disconnected", "", "", event->disconnect.reason};
            {
                std::lock_guard<std::mutex> lock(_mutex);
                const bool was_ready = _ready;
                _conn                = BLE_HS_CONN_HANDLE_NONE;
                _ready               = false;
                _state               = State{};
                e.car                = _name;
                e.addr               = _addr;
                if (was_ready) {
                    push_event(e);
                }
            }
            mclog::tagInfo(_tag, "disconnected, reason {}", event->disconnect.reason);
            start_scan();
            return 0;
        }
        case BLE_GAP_EVENT_NOTIFY_RX: {
            uint8_t b[_state_len];
            const uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            std::lock_guard<std::mutex> lock(_mutex);
            if (event->notify_rx.attr_handle != _state_val || len < _state_len ||
                os_mbuf_copydata(event->notify_rx.om, 0, _state_len, b) != 0 || b[0] != _state_format) {
                return 0;
            }
            _state.seq     = b[1];
            _state.ms      = b[2] | (b[3] << 8) | (b[4] << 16) | ((uint32_t)b[5] << 24);
            _state.echo_us = b[6] | (b[7] << 8);
            _state.inputs  = b[8];
            _state.left    = (int8_t)b[9];
            _state.right   = (int8_t)b[10];
            _state.flags   = b[11];
            _state.i2c_err = b[12];
            _state.board   = b[13];
            _fresh         = true;
            _last_note_ms  = now_ms();
            return 0;
        }
        default:
            return 0;
    }
}

void CarBle::on_service(uint16_t start, uint16_t end)
{
    uint16_t conn;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _svc_end = end;
        conn     = _conn;
    }
    const int rc = ble_gattc_disc_all_chrs(conn, start, end, chr_cb, nullptr);
    if (rc != 0) {
        fail("characteristic discovery", rc);
    }
}

void CarBle::on_chr(uint16_t def_handle, uint16_t val_handle, int which)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_state_val && !_state_def_next && def_handle > _state_val) {
        _state_def_next = def_handle;
    }
    if (which == 1) {
        _cmd_val = val_handle;
    } else if (which == 2) {
        _state_val = val_handle;
    }
}

void CarBle::on_chrs_done(int status)
{
    uint16_t conn, start, end;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        conn  = _conn;
        start = _state_val;
        end   = _state_def_next ? _state_def_next - 1 : _svc_end;
        if (status != 0 || !_cmd_val || !_state_val) {
            status = status ? status : BLE_HS_ENOENT;
        }
    }
    if (status != 0) {
        fail("TPBot characteristics", status);
        return;
    }
    const int rc = ble_gattc_disc_all_dscs(conn, start, end, dsc_cb, nullptr);
    if (rc != 0) {
        fail("descriptor discovery", rc);
    }
}

void CarBle::on_dsc(uint16_t handle, bool cccd)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (cccd && !_cccd) {
        _cccd = handle;
    }
}

void CarBle::on_dscs_done(int status)
{
    uint16_t conn, cccd;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        conn = _conn;
        cccd = _cccd;
    }
    if (status != 0 || !cccd) {
        fail("state notifications", status ? status : BLE_HS_ENOENT);
        return;
    }
    static const uint8_t on[] = {1, 0};
    const int rc              = ble_gattc_write_flat(conn, cccd, on, sizeof on, subscribe_cb, nullptr);
    if (rc != 0) {
        fail("subscribe", rc);
    }
}

void CarBle::on_subscribed(int status)
{
    if (status != 0) {
        fail("subscribe", status);
        return;
    }
    uint8_t board;
    Event e{"car_connected"};
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _ready        = true;
        _last_note_ms = now_ms();
        board         = _board;
        e.car         = _name;
        e.addr        = _addr;
        push_event(e);
    }
    mclog::tagInfo(_tag, "{} ready", e.car);
    this->board(board);
    stopMotors();  // a new link always starts from a stopped car
}

}  // namespace embody
