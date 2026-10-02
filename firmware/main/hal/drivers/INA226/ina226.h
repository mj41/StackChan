/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 *
 * Minimal TI INA226 current/voltage monitor. On StackChan it sits on the body
 * battery (Power board: I2C 0x41, 10 mOhm shunt R18 between BAT+ and BAT_IN).
 */
#pragma once
#include <driver/i2c_master.h>
#include <cstdint>

class INA226 {
public:
    struct Reading {
        float bus_v      = 0;  // battery voltage (1.25 mV steps)
        float shunt_uv   = 0;  // raw shunt voltage (2.5 uV steps), signed
        float current_ma = 0;  // shunt_uv / shunt resistance, signed
        float power_mw   = 0;  // bus_v * current_ma
    };

    ~INA226();
    // Checks the manufacturer ID and starts continuous shunt + bus conversion,
    // averaged over 16 samples (a fresh value about every 35 ms).
    bool begin(i2c_master_bus_handle_t bus, uint8_t address = 0x41, float shuntOhm = 0.010f);
    void end();
    bool read(Reading& out);

private:
    i2c_master_dev_handle_t _dev = nullptr;
    float _shunt_ohm             = 0.010f;

    bool rd16(uint8_t reg, uint16_t& value);
    bool wr16(uint8_t reg, uint16_t value);
};
