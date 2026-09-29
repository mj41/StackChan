/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "embody_client.h"
#include <mooncake.h>
#include <lvgl.h>
#include <cstdint>
#include <memory>
#include <string>

namespace smooth_ui_toolkit::lvgl_cpp {
class Container;
class Label;
}  // namespace smooth_ui_toolkit::lvgl_cpp

/**
 * @brief Embody Mode: remote control through stackchan-server.
 *
 * Connects to CONFIG_STACKCHAN_EMBODY_SERVER_URL, shows the server's pairing
 * URL as a QR code, streams telemetry, and nods on the "nod" command.
 */
class AppEmbodyMode : public mooncake::AppAbility {
public:
    AppEmbodyMode();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    std::unique_ptr<embody::Client> _client;
    bool _network_started = false;

    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _panel;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _title;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _qr_box;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _qr_hint;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _status;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _code;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _detail;
    lv_obj_t* _qr = nullptr;  // child of _qr_box, deleted with it

    uint32_t _rendered_revision = UINT32_MAX;
    std::string _rendered_url;
    std::string _last_command;

    // Nod: a few timed pitch moves around the starting angle.
    int _nod_step            = -1;
    uint32_t _nod_next_tick  = 0;
    int _nod_base_pitch      = 0;
    uint32_t _angle_sync_tick = 0;

    void create_view();
    void render();
    void start_nod();
    void update_nod();
};
