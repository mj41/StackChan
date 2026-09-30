/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "drivers/bmi270/bmi270.h"
#include "utils/motion_detector/motion_detector.h"
#include <mooncake_log.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <esp_timer.h>

static const std::string_view _tag = "HAL-IMU";

static std::unique_ptr<BMI270> _bmi270;
static std::mutex _sample_mutex;
static ImuSample_t _sample;
static bool _has_sample = false;
static std::atomic<bool> _streaming{false};
static std::vector<ImuStreamSample_t> _stream;  // under _sample_mutex
static constexpr size_t _stream_max = 200;      // 2 s at 100 Hz

static void _imu_task(void* param)
{
    auto motion_detector = std::make_unique<MotionDetector>();
    motion_detector->setShakeThreshold(16.0f);

    // 10 Hz normally; 100 Hz while streaming. The shake detector keeps getting 10 Hz,
    // as its threshold is tuned for that.
    TickType_t wake = xTaskGetTickCount();
    uint32_t n      = 0;
    while (1) {
        const bool streaming = _streaming;
        if (_bmi270 && _bmi270->update()) {
            auto& data = _bmi270->getData();
            {
                std::lock_guard<std::mutex> lock(_sample_mutex);
                _sample = {{data.accel_x, data.accel_y, data.accel_z},
                           {data.gyro_x, data.gyro_y, data.gyro_z},
                           data.mag_valid,
                           {data.mag_x, data.mag_y, data.mag_z},
                           {data.mag_raw_x, data.mag_raw_y, data.mag_raw_z},
                           data.mag_rhall};
                _has_sample = true;
                if (streaming) {
                    if (_stream.size() >= _stream_max) {
                        _stream.erase(_stream.begin());  // nobody collects: keep the newest
                    }
                    _stream.push_back({(uint32_t)(esp_timer_get_time() / 1000),
                                       {data.accel_x, data.accel_y, data.accel_z, data.gyro_x, data.gyro_y,
                                        data.gyro_z, data.mag_x, data.mag_y, data.mag_z}});
                }
            }

            if (!streaming || n % 10 == 0) {
                motion_detector->update(data.accel_x, data.accel_y, data.accel_z);
                if (motion_detector->isShakeDetected()) {
                    mclog::tagInfo(_tag, "Shake Detected!");
                    GetHAL().onImuMotionEvent.emit(ImuMotionEvent::Shake);
                }
            }
        }
        n++;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(streaming ? 10 : 100));
    }
}

void Hal::imu_init()
{
    mclog::tagInfo(_tag, "init");

    auto i2c_bus = hal_bridge::board_get_i2c_bus();

    _bmi270 = std::make_unique<BMI270>(i2c_bus, 0x69);
    if (!_bmi270->begin()) {
        _bmi270.reset();
        mclog::tagError(_tag, "BMI270 init failed");
        return;
    }
    mclog::tagInfo(_tag, "BMI270 init ok");
    if (!_bmi270->beginMagnetometer()) {
        mclog::tagWarn(_tag, "BMM150 magnetometer not available");
    }

    // xTaskCreateWithCaps(_imu_task, "imu", 4096, NULL, 2, NULL, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(_imu_task, "imu", 4096, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
}

bool Hal::getImuSample(ImuSample_t& out)
{
    std::lock_guard<std::mutex> lock(_sample_mutex);
    out = _sample;
    return _has_sample;
}

void Hal::setImuStreaming(bool on)
{
    _streaming = on;
    if (!on) {
        std::lock_guard<std::mutex> lock(_sample_mutex);
        _stream.clear();
    }
}

void Hal::takeImuStream(std::vector<ImuStreamSample_t>& out)
{
    out.clear();
    std::lock_guard<std::mutex> lock(_sample_mutex);
    out.swap(_stream);
}
