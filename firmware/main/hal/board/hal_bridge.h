/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "stackchan_camera.h"
#include <cstdint>
#include <lvgl.h>
#include <driver/i2c_master.h>
#include <string_view>

namespace hal_bridge {

struct TouchPoint_t {
    int num = 0;
    int x   = -1;
    int y   = -1;
};

struct Data_t {
    TouchPoint_t touchPoint;
    bool isXiaozhiMode              = false;
    bool isXiaozhiModeToggleEnabled = false;
};

struct XiaozhiConfig_t {
    uint32_t idleShutdownTimeSeconds = 600;
    bool allowShutdownWhenCharging   = false;
    uint8_t idleRandomMovementLevel  = 2;
    bool startAiAgentOnBoot          = false;
};

void lock();
void unlock();
Data_t& get_data();

void set_touch_point(int num, int x, int y);
TouchPoint_t get_touch_point();

bool is_xiaozhi_mode();
void set_xiaozhi_mode(bool mode);
void toggle_xiaozhi_chat_state();

void disply_lvgl_lock();
void disply_lvgl_unlock();
lv_disp_t* display_get_lvgl_display();

void xiaozhi_board_init();
void start_xiaozhi_app();
bool is_xiaozhi_ready();
bool is_xiaozhi_idle();
XiaozhiConfig_t get_xiaozhi_config();
void set_xiaozhi_config(const XiaozhiConfig_t& config);

i2c_master_bus_handle_t board_get_i2c_bus();
// Red power/charge LED: 0 off, 1 blink 1 Hz, 2 blink 4 Hz, 3 on, 4 driven by the charger.
void board_set_charge_led(int mode);

// Raw AXP2101 (CoreS3 power chip) readings.
struct PmicStatus {
    int battery_mv   = 0;  // ADC, 1 mV
    int vbus_mv      = 0;  // USB input, 0 without USB
    int system_mv    = 0;  // VSYS
    float die_temp_c = 0;
    int battery_pct  = 0;  // fuel gauge
    uint8_t status1  = 0;  // reg 0x00: bit 5 VBUS good, bit 3 battery present
    uint8_t status2  = 0;  // reg 0x01: bits 6:5 01 charging / 10 discharging / 00 idle, bits 2:0 charge phase
};
bool board_get_pmic_status(PmicStatus& out);
// Power key and plug events since the last call (AXP2101 IRQ status 2, reg 0x49, cleared here):
// bit 2 long press, 3 short press, 4 battery removed, 5 battery inserted, 6 USB removed, 7 USB inserted.
uint8_t board_take_pmic_events();
StackChanCamera* board_get_camera();
int board_get_battery_level();
bool board_is_battery_charging();
void board_set_backlight_brightness(uint8_t brightness, bool permanent = false);
uint8_t board_get_backlight_brightness();
void board_set_speaker_volume(uint8_t volume, bool permanent = false);
uint8_t board_get_speaker_volume();

void app_play_sound(const std::string_view& sound);

}  // namespace hal_bridge
