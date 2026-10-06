/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <lvgl.h>
#include <functional>

namespace view {

void create_home_indicator(std::function<void(void)> onGoHome, uint32_t colorButton = 0xB8D3FD,
                           uint32_t colorBorder = 0x26206A, lv_obj_t* parent = lv_screen_active());
void update_home_indicator();
// An optional second button to the right of Home in the swipe-up bar, e.g. an app's own
// screen. Call after create_home_indicator(); onClick runs in update_home_indicator().
void set_home_indicator_extra_button(const char* text, std::function<void(void)> onClick);
void set_home_indicator_extra_text(const char* text);
// Another pointer whose swipe up from the bottom edge also opens the bar (automation over USB).
void set_home_gesture_extra_indev(lv_indev_t* indev);
bool is_home_indicator_created();
void destroy_home_indicator();

}  // namespace view
