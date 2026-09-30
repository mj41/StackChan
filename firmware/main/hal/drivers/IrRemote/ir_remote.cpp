/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "ir_remote.h"
#include <esp_timer.h>
#include <algorithm>
#include <cstdlib>

namespace {

constexpr uint32_t kResolutionHz = 1000000;  // 1 tick = 1 us
constexpr uint32_t kMaxTicks     = 32767;    // 15-bit symbol duration
constexpr size_t kRxSymbols      = 512;      // long enough for air conditioner frames
constexpr int64_t kEchoQuietUs   = 150000;   // ignore our own signal after sending
constexpr size_t kMaxTimings     = 1000;

// NEC (us)
constexpr uint32_t NEC_LEAD_MARK = 9000, NEC_LEAD_SPACE = 4500, NEC_REPEAT_SPACE = 2250;
constexpr uint32_t NEC_BIT_MARK = 562, NEC_ZERO_SPACE = 562, NEC_ONE_SPACE = 1687;

bool near(uint32_t value, uint32_t target)
{
    return value > target * 7 / 10 && value < target * 13 / 10;  // receivers stretch marks a little
}

}  // namespace

IrRemote::~IrRemote()
{
    end();
}

bool IrRemote::begin(gpio_num_t txPin, gpio_num_t rxPin)
{
    end();
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num                = txPin;
    tx_cfg.clk_src                 = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz           = kResolutionHz;
    tx_cfg.mem_block_symbols       = 48;  // longer frames are refilled by the driver
    tx_cfg.trans_queue_depth       = 4;
    rmt_copy_encoder_config_t copy_cfg = {};

    rmt_rx_channel_config_t rx_cfg = {};
    rx_cfg.gpio_num                = rxPin;
    rx_cfg.clk_src                 = RMT_CLK_SRC_DEFAULT;
    rx_cfg.resolution_hz           = kResolutionHz;
    rx_cfg.mem_block_symbols       = 96;  // ping-pong copies longer frames into _rx_buf

    rmt_rx_event_callbacks_t callbacks = {};
    callbacks.on_recv_done             = on_rx_done;

    _rx_done = xQueueCreate(4, sizeof(size_t));
    _rx_buf.resize(kRxSymbols);
    bool ok = _rx_done && rmt_new_tx_channel(&tx_cfg, &_tx) == ESP_OK && rmt_new_copy_encoder(&copy_cfg, &_copy) == ESP_OK &&
              rmt_enable(_tx) == ESP_OK && rmt_new_rx_channel(&rx_cfg, &_rx) == ESP_OK &&
              rmt_rx_register_event_callbacks(_rx, &callbacks, this) == ESP_OK && rmt_enable(_rx) == ESP_OK &&
              arm_receive();
    if (!ok) {
        end();
    }
    return ok;
}

void IrRemote::end()
{
    if (_rx) {
        rmt_disable(_rx);
        rmt_del_channel(_rx);
        _rx = nullptr;
    }
    if (_tx) {
        rmt_disable(_tx);
        rmt_del_channel(_tx);
        _tx = nullptr;
    }
    if (_copy) {
        rmt_del_encoder(_copy);
        _copy = nullptr;
    }
    if (_rx_done) {
        vQueueDelete(_rx_done);
        _rx_done = nullptr;
    }
    _carrier_hz = 0;
}

/* ---------------------------------- Send ---------------------------------- */

bool IrRemote::send(const std::vector<uint32_t>& timings, uint32_t carrierHz, bool hearSelf)
{
    if (!_tx || timings.empty()) {
        return false;
    }
    if (carrierHz != _carrier_hz) {
        rmt_carrier_config_t carrier = {};
        carrier.frequency_hz         = std::clamp<uint32_t>(carrierHz, 30000, 60000);
        carrier.duty_cycle           = 0.5f;  // the LED only gets ~70 mA (51 R from 5 V): use more of each cycle
        if (rmt_apply_carrier(_tx, &carrier) != ESP_OK) {
            return false;
        }
        _carrier_hz = carrierHz;
    }

    // Marks (carrier on, level 1) and spaces alternate; split what doesn't fit in 15 bits.
    std::vector<std::pair<uint16_t, uint16_t>> parts;  // (level, ticks)
    for (size_t i = 0; i < timings.size() && i < kMaxTimings; i++) {
        uint32_t left = std::clamp<uint32_t>(timings[i], 1, 1000000);
        while (left > 0) {
            const uint32_t d = std::min(left, kMaxTicks);
            parts.emplace_back(i % 2 == 0 ? 1 : 0, d);
            left -= d;
        }
    }
    if (parts.size() % 2) {
        parts.emplace_back(0, 1);  // pad the last symbol with a 1 us space
    }
    std::vector<rmt_symbol_word_t> symbols(parts.size() / 2);
    for (size_t i = 0; i < symbols.size(); i++) {
        symbols[i].level0    = parts[2 * i].first;
        symbols[i].duration0 = parts[2 * i].second;
        symbols[i].level1    = parts[2 * i + 1].first;
        symbols[i].duration1 = parts[2 * i + 1].second;
    }

    uint64_t total_us = 0;
    for (const auto& part : parts) {
        total_us += part.second;
    }
    rmt_transmit_config_t tx_cfg = {};
    _quiet_until_us              = INT64_MAX;  // while sending
    bool ok = rmt_transmit(_tx, _copy, symbols.data(), symbols.size() * sizeof(rmt_symbol_word_t), &tx_cfg) == ESP_OK &&
              rmt_tx_wait_all_done(_tx, (int)(total_us / 1000) + 500) == ESP_OK;
    _quiet_until_us = hearSelf ? 0 : esp_timer_get_time() + kEchoQuietUs;
    return ok;
}

std::vector<uint32_t> IrRemote::necTimings(uint16_t address, uint8_t command, int repeats)
{
    const uint8_t lo    = address & 0xFF;
    const uint8_t hi    = address > 0xFF ? (address >> 8) : (uint8_t)~lo;  // extended NEC has a 16-bit address
    const uint8_t b[4]  = {lo, hi, command, (uint8_t)~command};
    std::vector<uint32_t> t = {NEC_LEAD_MARK, NEC_LEAD_SPACE};
    for (int i = 0; i < 32; i++) {
        t.push_back(NEC_BIT_MARK);
        t.push_back((b[i / 8] >> (i % 8)) & 1 ? NEC_ONE_SPACE : NEC_ZERO_SPACE);  // LSB first
    }
    t.push_back(NEC_BIT_MARK);
    // Repeat codes (9 ms mark, 2.25 ms space, stop mark), each starting 108 ms after the previous start.
    constexpr uint32_t period = 108000, repeat_len = NEC_LEAD_MARK + NEC_REPEAT_SPACE + NEC_BIT_MARK;
    uint32_t frame_len = 0;
    for (uint32_t v : t) {
        frame_len += v;
    }
    for (int r = 0; r < repeats; r++) {
        t.push_back(period - (r == 0 ? frame_len : repeat_len));
        t.push_back(NEC_LEAD_MARK);
        t.push_back(NEC_REPEAT_SPACE);
        t.push_back(NEC_BIT_MARK);
    }
    return t;
}

bool IrRemote::sendNec(uint16_t address, uint8_t command, bool hearSelf, int repeats)
{
    return send(necTimings(address, command, repeats), 38000, hearSelf);
}

std::vector<uint32_t> IrRemote::repeated(const std::vector<uint32_t>& frame, int repeats, uint32_t gapMs)
{
    std::vector<uint32_t> t = frame;
    for (int r = 0; r < repeats && !frame.empty() && t.size() + frame.size() + 1 <= kMaxTimings; r++) {
        if (frame.size() % 2 == 0) {
            t.back() += gapMs * 1000;  // the frame already ends with a space
        } else {
            t.push_back(gapMs * 1000);
        }
        t.insert(t.end(), frame.begin(), frame.end());
    }
    return t;
}

/* --------------------------------- Receive -------------------------------- */

bool IrRemote::on_rx_done(rmt_channel_handle_t, const rmt_rx_done_event_data_t* data, void* ctx)
{
    auto* self      = static_cast<IrRemote*>(ctx);
    BaseType_t woke = pdFALSE;
    size_t n        = data->num_symbols;
    xQueueSendFromISR(self->_rx_done, &n, &woke);
    return woke == pdTRUE;
}

bool IrRemote::arm_receive()
{
    rmt_receive_config_t cfg = {};
    cfg.signal_range_min_ns  = 1000;      // shorter pulses are glitches
    cfg.signal_range_max_ns  = 12000000;  // 12 ms without an edge ends the frame (the NEC lead mark is 9 ms)
    return rmt_receive(_rx, _rx_buf.data(), _rx_buf.size() * sizeof(rmt_symbol_word_t), &cfg) == ESP_OK;
}

bool IrRemote::receive(Frame& out)
{
    size_t n = 0;
    if (!_rx || xQueueReceive(_rx_done, &n, 0) != pdTRUE) {
        return false;
    }
    // The receiver's output is low while it sees the carrier: low = mark.
    Frame frame;
    int last_level = -1;
    for (size_t i = 0; i < std::min(n, _rx_buf.size()); i++) {
        const rmt_symbol_word_t sym            = _rx_buf[i];
        const std::pair<int, uint32_t> halves[2] = {{sym.level0 ? 0 : 1, (uint32_t)sym.duration0},
                                                    {sym.level1 ? 0 : 1, (uint32_t)sym.duration1}};
        for (const auto& [mark, ticks] : halves) {
            if (ticks == 0 || (frame.timings.empty() && !mark)) {
                continue;  // end marker, or a space before the first mark
            }
            if (mark == last_level && !frame.timings.empty()) {
                frame.timings.back() += ticks;  // merge split symbols of the same level
            } else if (frame.timings.size() < kMaxTimings) {
                frame.timings.push_back(ticks);
            }
            last_level = mark;
        }
    }
    if (last_level == 0 && !frame.timings.empty()) {
        frame.timings.pop_back();  // a trailing space is just the idle line
    }
    arm_receive();

    if (esp_timer_get_time() < _quiet_until_us || frame.timings.size() < 3) {
        return false;  // our own echo, or noise
    }
    decode_nec(frame);
    out = std::move(frame);
    return true;
}

bool IrRemote::decode_nec(Frame& f)
{
    const auto& t = f.timings;
    if (t.size() >= 3 && t.size() <= 4 && near(t[0], NEC_LEAD_MARK) && near(t[1], NEC_REPEAT_SPACE) &&
        near(t[2], NEC_BIT_MARK)) {
        f.nec = f.repeat = true;
        return true;
    }
    if (t.size() < 67 || !near(t[0], NEC_LEAD_MARK) || !near(t[1], NEC_LEAD_SPACE)) {
        return false;
    }
    uint8_t b[4] = {};
    for (int i = 0; i < 32; i++) {
        const uint32_t mark = t[2 + 2 * i], space = t[3 + 2 * i];
        const bool one      = near(space, NEC_ONE_SPACE);
        if (!near(mark, NEC_BIT_MARK) || (!one && !near(space, NEC_ZERO_SPACE))) {
            return false;
        }
        if (one) {
            b[i / 8] |= 1 << (i % 8);
        }
    }
    if ((uint8_t)(b[2] ^ b[3]) != 0xFF) {
        return false;
    }
    f.nec     = true;
    f.address = (uint8_t)(b[0] ^ b[1]) == 0xFF ? b[0] : (uint16_t)(b[0] | (b[1] << 8));
    f.command = b[2];
    return true;
}

/* --------------------------------- Strings -------------------------------- */

std::string IrRemote::timingsToString(const std::vector<uint32_t>& timings)
{
    std::string s;
    for (size_t i = 0; i < timings.size(); i++) {
        if (i) {
            s += ',';
        }
        s += std::to_string(timings[i]);
    }
    return s;
}

std::vector<uint32_t> IrRemote::timingsFromString(const std::string& text)
{
    std::vector<uint32_t> out;
    const char* p = text.c_str();
    while (*p && out.size() < kMaxTimings) {
        char* end     = nullptr;
        const long v  = std::strtol(p, &end, 10);
        if (end == p) {
            ++p;  // skip separators
            continue;
        }
        if (v > 0) {
            out.push_back((uint32_t)std::min(v, 1000000L));
        }
        p = end;
    }
    return out;
}
