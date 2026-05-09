/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <memory>
#include <string>

namespace smooth_ui_toolkit::lvgl_cpp {
class Button;
class Container;
class Label;
}  // namespace smooth_ui_toolkit::lvgl_cpp

class AppEmbodyMode : public mooncake::AppAbility {
public:
    AppEmbodyMode();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _panel;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _title;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _subtitle;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _status;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> _ai_agent_button;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> _avatar_button;

    std::string _status_text;
    std::string _last_rendered_status_text;
};