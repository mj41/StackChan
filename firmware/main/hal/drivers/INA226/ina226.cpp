/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "ina226.h"

namespace {
constexpr int kI2cTimeoutMs       = 50;
constexpr uint8_t REG_CONFIG      = 0x00;
constexpr uint8_t REG_SHUNT       = 0x01;  // 2.5 uV/LSB, signed
constexpr uint8_t REG_BUS         = 0x02;  // 1.25 mV/LSB
constexpr uint8_t REG_MANUFACTURER = 0xFE;  // 0x5449 "TI"
// 0100 (fixed) | AVG 16 (010) | VBUS 1.1 ms (100) | VSHUNT 1.1 ms (100) | continuous shunt + bus (111)
constexpr uint16_t CONFIG = (0x4 << 12) | (0x2 << 9) | (0x4 << 6) | (0x4 << 3) | 0x7;
}  // namespace

INA226::~INA226()
{
    end();
}

bool INA226::begin(i2c_master_bus_handle_t bus, uint8_t address, float shuntOhm)
{
    end();
    _shunt_ohm = shuntOhm;
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
    uint16_t id = 0;
    if (!rd16(REG_MANUFACTURER, id) || id != 0x5449 || !wr16(REG_CONFIG, CONFIG)) {
        end();
        return false;
    }
    return true;
}

void INA226::end()
{
    if (_dev) {
        i2c_master_bus_rm_device(_dev);
        _dev = nullptr;
    }
}

bool INA226::rd16(uint8_t reg, uint16_t& value)
{
    uint8_t d[2];
    if (i2c_master_transmit_receive(_dev, &reg, 1, d, 2, kI2cTimeoutMs) != ESP_OK) {
        return false;
    }
    value = (d[0] << 8) | d[1];  // big-endian
    return true;
}

bool INA226::wr16(uint8_t reg, uint16_t value)
{
    uint8_t d[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_master_transmit(_dev, d, 3, kI2cTimeoutMs) == ESP_OK;
}

bool INA226::read(Reading& out)
{
    uint16_t shunt = 0, bus = 0;
    if (!_dev || !rd16(REG_SHUNT, shunt) || !rd16(REG_BUS, bus)) {
        return false;
    }
    out.shunt_uv   = (int16_t)shunt * 2.5f;
    out.bus_v      = bus * 0.00125f;
    out.current_ma = out.shunt_uv / 1000.0f / _shunt_ohm;
    out.power_mw   = out.bus_v * out.current_ma;
    return true;
}
