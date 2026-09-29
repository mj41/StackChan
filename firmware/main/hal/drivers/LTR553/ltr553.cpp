/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "ltr553.h"

namespace {

constexpr int kI2cTimeoutMs = 50;

constexpr uint8_t REG_ALS_CONTR     = 0x80;  // bit 0 active, bits 4:2 gain
constexpr uint8_t REG_PS_CONTR      = 0x81;  // bits 1:0 = 10 active
constexpr uint8_t REG_PS_LED        = 0x82;  // pulse frequency, duty cycle, peak current
constexpr uint8_t REG_PS_N_PULSES   = 0x83;
constexpr uint8_t REG_PS_MEAS_RATE  = 0x84;
constexpr uint8_t REG_ALS_MEAS_RATE = 0x85;  // bits 5:3 integration time, bits 2:0 rate
constexpr uint8_t REG_PART_ID       = 0x86;  // 0x92: part 0x9, revision 0x2
constexpr uint8_t REG_ALS_DATA_CH1  = 0x88;  // CH1 low, CH1 high, CH0 low, CH0 high
constexpr uint8_t REG_ALS_PS_STATUS = 0x8C;  // bit 7: ALS data invalid
constexpr uint8_t REG_PS_DATA       = 0x8D;  // low, then high (bits 2:0)

constexpr uint8_t ALS_GAIN_4X      = 0x02;
constexpr float ALS_GAIN           = 4.0f;
constexpr float ALS_INT_100MS      = 1.0f;  // integration time in units of 100 ms
constexpr uint8_t ALS_INT_TIME_100 = 0x00;
constexpr uint8_t ALS_RATE_500MS   = 0x03;
constexpr uint8_t PS_RATE_100MS    = 0x02;
// 60 kHz pulses, 100 % duty, 100 mA peak (the datasheet default), 4 pulses per reading
constexpr uint8_t PS_LED_DEFAULT = (0x03 << 5) | (0x03 << 3) | 0x04;
constexpr uint8_t PS_PULSES      = 4;

}  // namespace

LTR553::~LTR553()
{
    end();
}

bool LTR553::begin(i2c_master_bus_handle_t bus, uint8_t address)
{
    end();
    if (!bus || i2c_master_probe(bus, address, kI2cTimeoutMs) != ESP_OK) {
        return false;
    }
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length     = I2C_ADDR_BIT_LEN_7;
    cfg.device_address      = address;
    cfg.scl_speed_hz        = 100000;
    if (i2c_master_bus_add_device(bus, &cfg, &_dev) != ESP_OK) {
        _dev = nullptr;
        return false;
    }
    uint8_t part = 0;
    if (!rd(REG_PART_ID, &part, 1) || (part >> 4) != 0x9) {
        end();
        return false;
    }
    bool ok = wr(REG_PS_LED, PS_LED_DEFAULT) && wr(REG_PS_N_PULSES, PS_PULSES) &&
              wr(REG_PS_MEAS_RATE, PS_RATE_100MS) &&
              wr(REG_ALS_MEAS_RATE, (ALS_INT_TIME_100 << 3) | ALS_RATE_500MS) &&
              wr(REG_ALS_CONTR, (ALS_GAIN_4X << 2) | 0x01) && wr(REG_PS_CONTR, 0x02);
    if (!ok) {
        end();
    }
    return ok;
}

void LTR553::end()
{
    if (_dev) {
        wr(REG_PS_CONTR, 0x00);  // standby: the IR LED stops pulsing
        wr(REG_ALS_CONTR, 0x00);
        i2c_master_bus_rm_device(_dev);
        _dev = nullptr;
    }
}

bool LTR553::wr(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(_dev, buf, 2, kI2cTimeoutMs) == ESP_OK;
}

bool LTR553::rd(uint8_t reg, uint8_t* out, size_t len)
{
    return i2c_master_transmit_receive(_dev, &reg, 1, out, len, kI2cTimeoutMs) == ESP_OK;
}

bool LTR553::readLux(float& lux)
{
    uint8_t status = 0, d[4];
    if (!_dev || !rd(REG_ALS_PS_STATUS, &status, 1) || (status & 0x80) || !rd(REG_ALS_DATA_CH1, d, 4)) {
        return false;
    }
    const float ch1 = (float)(d[0] | (d[1] << 8));
    const float ch0 = (float)(d[2] | (d[3] << 8));
    if (ch0 + ch1 == 0) {
        lux = 0;
        return true;
    }
    // Lite-On LTR-553ALS appendix A: the formula depends on the IR ratio.
    const float ratio = ch1 / (ch0 + ch1);
    float raw         = 0;
    if (ratio < 0.45f) {
        raw = 1.7743f * ch0 + 1.1059f * ch1;
    } else if (ratio < 0.64f) {
        raw = 4.2785f * ch0 - 1.9548f * ch1;
    } else if (ratio < 0.85f) {
        raw = 0.5926f * ch0 + 0.1185f * ch1;
    }
    lux = raw > 0 ? raw / ALS_GAIN / ALS_INT_100MS : 0;
    return true;
}

bool LTR553::readProximity(uint16_t& value)
{
    uint8_t d[2];
    if (!_dev || !rd(REG_PS_DATA, d, 2)) {
        return false;
    }
    value = d[0] | ((d[1] & 0x07) << 8);
    return true;
}
