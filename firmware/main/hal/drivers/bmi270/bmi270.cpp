/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "bmi270.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstring>

static const char* TAG = "BMI270";

BMI270::BMI270(i2c_master_bus_handle_t i2c_bus_handle, uint8_t addr) : _addr(addr), _initialized(false)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = _addr,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &_i2c_dev));

    _bmi.intf            = BMI2_I2C_INTF;
    _bmi.read            = bmi2_i2c_read;
    _bmi.write           = bmi2_i2c_write;
    _bmi.delay_us        = bmi2_delay_us;
    _bmi.read_write_len  = 32;         // Max read/write length
    _bmi.config_file_ptr = NULL;       // Use default config
    _bmi.intf_ptr        = &_i2c_dev;  // Pass the device handle as interface pointer
}

BMI270::~BMI270()
{
    if (_i2c_dev) {
        i2c_master_bus_rm_device(_i2c_dev);
    }
}

BMI2_INTF_RETURN_TYPE BMI270::bmi2_i2c_read(uint8_t reg_addr, uint8_t* reg_data, uint32_t len, void* intf_ptr)
{
    i2c_master_dev_handle_t dev = *(i2c_master_dev_handle_t*)intf_ptr;
    esp_err_t err               = i2c_master_transmit_receive(dev, &reg_addr, 1, reg_data, len, 1000);
    return (err == ESP_OK) ? BMI2_OK : BMI2_E_COM_FAIL;
}

BMI2_INTF_RETURN_TYPE BMI270::bmi2_i2c_write(uint8_t reg_addr, const uint8_t* reg_data, uint32_t len, void* intf_ptr)
{
    i2c_master_dev_handle_t dev = *(i2c_master_dev_handle_t*)intf_ptr;

    uint8_t* buf = (uint8_t*)malloc(len + 1);
    if (!buf) return BMI2_E_COM_FAIL;

    buf[0] = reg_addr;
    memcpy(buf + 1, reg_data, len);

    esp_err_t err = i2c_master_transmit(dev, buf, len + 1, 1000);
    free(buf);

    return (err == ESP_OK) ? BMI2_OK : BMI2_E_COM_FAIL;
}

#define NOP() asm volatile("nop")

void BMI270::bmi2_delay_us(uint32_t period, void* intf_ptr)
{
    uint64_t m = (uint64_t)esp_timer_get_time();
    if (period) {
        uint64_t e = (m + period);
        if (m > e) {  // overflow
            while ((uint64_t)esp_timer_get_time() > e) {
                NOP();
            }
        }
        while ((uint64_t)esp_timer_get_time() < e) {
            NOP();
        }
    }
}

bool BMI270::begin()
{
    int8_t rslt;

    // Initialize bmi270
    rslt = bmi270_init(&_bmi);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "bmi270_init failed: %d", rslt);
        return false;
    }

    // Configure Accel
    struct bmi2_sens_config config;
    config.type = BMI2_ACCEL;
    rslt        = bmi2_get_sensor_config(&config, 1, &_bmi);
    if (rslt == BMI2_OK) {
        config.cfg.acc.odr         = BMI2_ACC_ODR_100HZ;
        config.cfg.acc.range       = BMI2_ACC_RANGE_2G;
        config.cfg.acc.bwp         = BMI2_ACC_NORMAL_AVG4;
        config.cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
        rslt                       = bmi2_set_sensor_config(&config, 1, &_bmi);
    }
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Accel config failed: %d", rslt);
        return false;
    }

    // Configure Gyro
    config.type = BMI2_GYRO;
    rslt        = bmi2_get_sensor_config(&config, 1, &_bmi);
    if (rslt == BMI2_OK) {
        config.cfg.gyr.odr         = BMI2_GYR_ODR_100HZ;
        config.cfg.gyr.range       = BMI2_GYR_RANGE_2000;
        config.cfg.gyr.bwp         = BMI2_GYR_NORMAL_MODE;
        config.cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
        config.cfg.gyr.noise_perf  = BMI2_PERF_OPT_MODE;
        rslt                       = bmi2_set_sensor_config(&config, 1, &_bmi);
    }
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Gyro config failed: %d", rslt);
        return false;
    }

    // Enable sensors
    uint8_t sensor_list[2] = {BMI2_ACCEL, BMI2_GYRO};
    rslt                   = bmi2_sensor_enable(sensor_list, 2, &_bmi);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Sensor enable failed: %d", rslt);
        return false;
    }

    _initialized = true;
    return true;
}

bool BMI270::update()
{
    if (!_initialized) return false;

    struct bmi2_sens_data sens_data = {{0}};
    int8_t rslt                     = bmi2_get_sensor_data(&sens_data, &_bmi);

    if (rslt == BMI2_OK) {
        // Convert Accel
        // Assuming 2G range and 16-bit resolution as configured
        _data.accel_x = lsb_to_mps2(sens_data.acc.x, 2.0f, 16);
        _data.accel_y = lsb_to_mps2(sens_data.acc.y, 2.0f, 16);
        _data.accel_z = lsb_to_mps2(sens_data.acc.z, 2.0f, 16);

        // Convert Gyro
        // Assuming 2000dps range and 16-bit resolution
        _data.gyro_x = lsb_to_dps(sens_data.gyr.x, 2000.0f, 16);
        _data.gyro_y = lsb_to_dps(sens_data.gyr.y, 2000.0f, 16);
        _data.gyro_z = lsb_to_dps(sens_data.gyr.z, 2000.0f, 16);

        if (_mag_ok) {
            decode_mag(sens_data.aux_data);
        }

        return true;
    }
    return false;
}

void BMI270::getAccelerometer(float& x, float& y, float& z)
{
    x = _data.accel_x;
    y = _data.accel_y;
    z = _data.accel_z;
}

void BMI270::getGyroscope(float& x, float& y, float& z)
{
    x = _data.gyro_x;
    y = _data.gyro_y;
    z = _data.gyro_z;
}

const BMI270_Data& BMI270::getData()
{
    return _data;
}

float BMI270::lsb_to_mps2(int16_t val, float g_range, uint8_t bit_width)
{
    float half_scale = (float)(1 << (bit_width - 1));
    return (9.80665f * val * g_range) / half_scale;
}

float BMI270::lsb_to_dps(int16_t val, float dps, uint8_t bit_width)
{
    float half_scale = (float)(1 << (bit_width - 1));
    return (dps * val) / half_scale;
}

/* ------------------------------ BMM150 (AUX) ------------------------------ */

namespace {
constexpr uint8_t BMM150_ADDR          = 0x10;
constexpr uint8_t BMM150_CHIP_ID_REG   = 0x40;  // 0x32
constexpr uint8_t BMM150_DATA_X_LSB    = 0x42;  // X, Y, Z, RHALL: 8 bytes
constexpr uint8_t BMM150_POWER_CTRL    = 0x4B;
constexpr uint8_t BMM150_OP_MODE       = 0x4C;  // bits 5:3 ODR (000 = 10 Hz), 2:1 mode (00 normal)
constexpr uint8_t BMM150_REP_XY        = 0x51;
constexpr uint8_t BMM150_REP_Z         = 0x52;
constexpr uint8_t BMM150_TRIM_X1       = 0x5D;  // trim block 0x5D..0x71
}  // namespace

bool BMI270::beginMagnetometer()
{
    if (!_initialized) {
        return false;
    }
    // AUX interface in manual mode, to set up the BMM150
    struct bmi2_sens_config config;
    config.type = BMI2_AUX;
    int8_t rslt = bmi2_get_sensor_config(&config, 1, &_bmi);
    config.cfg.aux.aux_en          = BMI2_ENABLE;
    config.cfg.aux.manual_en       = BMI2_ENABLE;
    config.cfg.aux.fcu_write_en    = BMI2_ENABLE;
    config.cfg.aux.man_rd_burst    = BMI2_AUX_READ_LEN_3;  // 8 bytes
    config.cfg.aux.aux_rd_burst    = BMI2_AUX_READ_LEN_3;
    config.cfg.aux.odr             = BMI2_AUX_ODR_25HZ;
    config.cfg.aux.offset          = 0;
    config.cfg.aux.i2c_device_addr = BMM150_ADDR;
    config.cfg.aux.read_addr       = BMM150_DATA_X_LSB;
    uint8_t sensors[3]             = {BMI2_ACCEL, BMI2_GYRO, BMI2_AUX};
    if (rslt != BMI2_OK || bmi2_set_sensor_config(&config, 1, &_bmi) != BMI2_OK ||
        bmi2_sensor_enable(sensors, 3, &_bmi) != BMI2_OK) {
        ESP_LOGW(TAG, "AUX setup failed");
        return false;
    }

    const uint8_t power_on = 0x01;
    bmi2_write_aux_man_mode(BMM150_POWER_CTRL, &power_on, 1, &_bmi);
    vTaskDelay(pdMS_TO_TICKS(10));  // start-up time 3 ms
    uint8_t id = 0;
    if (bmi2_read_aux_man_mode(BMM150_CHIP_ID_REG, &id, 1, &_bmi) != BMI2_OK || id != 0x32) {
        ESP_LOGW(TAG, "BMM150 not found (chip id 0x%02x)", id);
        return false;
    }
    // Regular preset: 9 XY and 15 Z repetitions, normal mode at 10 Hz
    const uint8_t rep_xy = 0x04, rep_z = 0x0E, op_mode = 0x00;
    bmi2_write_aux_man_mode(BMM150_REP_XY, &rep_xy, 1, &_bmi);
    bmi2_write_aux_man_mode(BMM150_REP_Z, &rep_z, 1, &_bmi);
    bmi2_write_aux_man_mode(BMM150_OP_MODE, &op_mode, 1, &_bmi);

    uint8_t t[21] = {};  // 0x5D..0x71
    if (bmi2_read_aux_man_mode(BMM150_TRIM_X1, t, sizeof(t), &_bmi) != BMI2_OK) {
        ESP_LOGW(TAG, "BMM150 trim read failed");
        return false;
    }
    auto u16 = [&](int i) { return (uint16_t)(t[i] | (t[i + 1] << 8)); };
    _mag_trim.x1   = (int8_t)t[0];                   // 0x5D
    _mag_trim.y1   = (int8_t)t[1];                   // 0x5E
    _mag_trim.z4   = (int16_t)u16(0x62 - 0x5D);
    _mag_trim.x2   = (int8_t)t[0x64 - 0x5D];
    _mag_trim.y2   = (int8_t)t[0x65 - 0x5D];
    _mag_trim.z2   = (int16_t)u16(0x68 - 0x5D);
    _mag_trim.z1   = u16(0x6A - 0x5D);
    _mag_trim.xyz1 = u16(0x6C - 0x5D) & 0x7FFF;
    _mag_trim.z3   = (int16_t)u16(0x6E - 0x5D);
    _mag_trim.xy2  = (int8_t)t[0x70 - 0x5D];
    _mag_trim.xy1  = t[0x71 - 0x5D];

    // Data mode: the BMI270 now reads 0x42..0x49 by itself into its AUX data registers
    config.cfg.aux.manual_en = BMI2_DISABLE;
    if (bmi2_set_sensor_config(&config, 1, &_bmi) != BMI2_OK) {
        ESP_LOGW(TAG, "AUX data mode failed");
        return false;
    }
    _mag_ok = true;
    ESP_LOGI(TAG, "BMM150 magnetometer ok");
    return true;
}

// Raw fields per the BMM150 datasheet, compensated with Bosch's floating-point formulas.
void BMI270::decode_mag(const uint8_t* a)
{
    const int16_t x = (int16_t)(a[0] | (a[1] << 8)) >> 3;  // 13 bit
    const int16_t y = (int16_t)(a[2] | (a[3] << 8)) >> 3;  // 13 bit
    const int16_t z = (int16_t)(a[4] | (a[5] << 8)) >> 1;  // 15 bit
    const uint16_t rhall = (uint16_t)(a[6] | (a[7] << 8)) >> 2;
    _data.mag_raw_x = x;
    _data.mag_raw_y = y;
    _data.mag_raw_z = z;
    _data.mag_rhall = rhall;
    const auto& tr  = _mag_trim;
    if (rhall == 0 || tr.xyz1 == 0 || x == -4096 || y == -4096 || z == -16384) {
        _data.mag_valid = false;  // overflow or no data yet
        return;
    }
    const float r0 = (float)tr.xyz1 * 16384.0f / rhall - 16384.0f;
    auto xy = [&](int16_t v, int8_t d1, int8_t d2) {
        const float p2 = (float)tr.xy2 * (r0 * r0 / 268435456.0f);
        const float p3 = p2 + r0 * (float)tr.xy1 / 16384.0f;
        const float p5 = v * ((p3 + 256.0f) * ((float)d2 + 160.0f));
        return (p5 / 8192.0f + (float)d1 * 8.0f) / 16.0f;
    };
    _data.mag_x = xy(x, tr.x1, tr.x2);
    _data.mag_y = xy(y, tr.y1, tr.y2);
    const float z0  = (float)z - (float)tr.z4;
    const float z2  = (float)tr.z3 * ((float)rhall - (float)tr.xyz1);
    const float z4  = (float)tr.z2 + (float)tr.z1 * (float)rhall / 32768.0f;
    _data.mag_z     = ((z0 * 131072.0f - z2) / (z4 * 4.0f)) / 16.0f;
    _data.mag_valid = true;
}
