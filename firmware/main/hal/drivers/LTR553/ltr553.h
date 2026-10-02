/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 *
 * Minimal LTR-553ALS-WA ambient light + proximity sensor driver (CoreS3, I2C 0x23).
 * Register values follow the Lite-On datasheet and M5Stack's M5CoreS3 LTR5XX
 * driver (MIT): https://github.com/m5stack/M5CoreS3/blob/main/src/utility/LTR5XX.cpp
 * Polled; the interrupt pin is not used.
 */
#pragma once
#include <driver/i2c_master.h>
#include <cstdint>

class LTR553 {
public:
    ~LTR553();

    // Checks the part ID, then starts both sensors: light at gain 4x (about
    // 0.25-16k lux), 100 ms integration every 500 ms; proximity every 100 ms.
    bool begin(i2c_master_bus_handle_t bus, uint8_t address = 0x23);
    void end();

    // Ambient light in lux (Lite-On's two-channel formula). False if no valid sample.
    // ch0/ch1 (optional) get the raw counts: CH0 visible + IR, CH1 IR.
    bool readLux(float& lux, uint16_t* ch0 = nullptr, uint16_t* ch1 = nullptr);
    // Proximity, 0..2047: higher is closer. Relative only; depends on the target.
    bool readProximity(uint16_t& value);
    // Proximity on/off. Off puts it in standby: its IR LED stops pulsing. Light keeps working.
    bool setProximityEnabled(bool on);
    // Fast: proximity every 50 ms and light every 100 ms (for a stream); else the
    // defaults above. The light integration time stays 100 ms, so lux is unchanged.
    bool setFastRate(bool fast);

private:
    i2c_master_dev_handle_t _dev = nullptr;

    bool wr(uint8_t reg, uint8_t value);
    bool rd(uint8_t reg, uint8_t* out, size_t len);
};
