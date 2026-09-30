/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * Infrared remote send/receive with the ESP32-S3 RMT peripheral. On StackChan
 * the IR LED is on G5 and the IRM56384 receiver (38 kHz, active low) on G10.
 * Frames are kept as raw mark/space timings so any remote can be replayed;
 * NEC frames are also decoded.
 */
#pragma once
#include <driver/gpio.h>
#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstdint>
#include <string>
#include <vector>

class IrRemote {
public:
    struct Frame {
        std::vector<uint32_t> timings;  // microseconds: mark, space, mark, ... (starts and ends with a mark)
        bool nec     = false;           // a valid NEC frame (address/command below)
        bool repeat  = false;           // an NEC repeat code (button held)
        uint16_t address = 0;           // 8-bit, or 16-bit for extended NEC
        uint8_t command  = 0;
    };

    ~IrRemote();

    bool begin(gpio_num_t txPin, gpio_num_t rxPin);
    void end();

    // Sends marks/spaces in microseconds, modulated with the carrier. Blocks until done.
    // With hearSelf, the receiver keeps listening, so a reflection of our own
    // signal comes back through receive() (a self-test).
    bool send(const std::vector<uint32_t>& timings, uint32_t carrierHz = 38000, bool hearSelf = false);
    // frames: the whole frame this many times (a receiver that misses one gets another; a
    // toggle button may then toggle twice). repeats: NEC repeat codes after that, like a held
    // button. All 108 ms apart, as a real remote does.
    bool sendNec(uint16_t address, uint8_t command, bool hearSelf = false, int repeats = 0, int frames = 1);
    static std::vector<uint32_t> necTimings(uint16_t address, uint8_t command, int repeats = 0, int frames = 1);
    // The frame sent 1 + repeats times, gapMs apart (for raw codes).
    static std::vector<uint32_t> repeated(const std::vector<uint32_t>& frame, int repeats, uint32_t gapMs = 40);

    // A received frame, if one arrived (non-blocking). Call it regularly: it also
    // re-arms the receiver. Frames seen while (or just after) sending are our own
    // echo and dropped.
    bool receive(Frame& out);

    static std::string timingsToString(const std::vector<uint32_t>& timings);
    static std::vector<uint32_t> timingsFromString(const std::string& text);

private:
    rmt_channel_handle_t _tx    = nullptr;
    rmt_channel_handle_t _rx    = nullptr;
    rmt_encoder_handle_t _copy  = nullptr;
    QueueHandle_t _rx_done      = nullptr;
    std::vector<rmt_symbol_word_t> _rx_buf;
    uint32_t _carrier_hz        = 0;
    int64_t _quiet_until_us     = 0;  // ignore receptions until then (own echo)

    bool arm_receive();
    static bool on_rx_done(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t* data, void* ctx);
    static bool decode_nec(Frame& frame);
};
