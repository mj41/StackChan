/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_embody_mode.h"
#include <apps/common/common.h>
#include <apps/common/loading_page/loading_page.h>
#include <assets/assets.h>
#include <hal/hal.h>
#include <stackchan/stackchan.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>
#include <wifi_manager.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <sdkconfig.h>
#include <algorithm>
#include <cctype>
#include <iterator>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

// Launcher index of this app (see requestWarmReboot in the other apps).
static constexpr int _launcher_index = 0;

static constexpr uint32_t _color_theme = 0x7D8CFF;
static constexpr uint32_t _color_text  = 0x1E2355;
static constexpr uint32_t _color_muted = 0x5A618E;

AppEmbodyMode::AppEmbodyMode()
{
    setAppInfo().name = "Embody Mode";
    static auto icon  = assets::get_image("icon_embody_mode.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = _color_theme;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppEmbodyMode::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppEmbodyMode::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    const std::string server_url = CONFIG_STACKCHAN_EMBODY_SERVER_URL;
    if (!server_url.empty()) {
        std::unique_ptr<view::LoadingPage> loading_page;
        {
            LvglLockGuard lock;
            loading_page = std::make_unique<view::LoadingPage>(_color_theme, _color_text);
        }
        GetHAL().startNetwork([&](std::string_view msg) {
            LvglLockGuard lock;
            loading_page->setMessage(msg);
        });
        _network_started = true;

        LvglLockGuard lock;
        loading_page.reset();
    }

    {
        LvglLockGuard lock;
        create_view();
    }

    if (server_url.empty()) {
        LvglLockGuard lock;
        _status->setText("Set CONFIG_STACKCHAN_EMBODY_SERVER_URL in sdkconfig.defaults.local");
        return;
    }

    auto robot_id = "stackchan-" + GetHAL().getFactoryMacString();
    std::transform(robot_id.begin(), robot_id.end(), robot_id.begin(), [](unsigned char c) { return std::tolower(c); });

    _client = std::make_unique<embody::Client>(embody::Client::Config{
        .serverUrl    = server_url,
        .token        = CONFIG_STACKCHAN_EMBODY_TOKEN,
        .robotId      = robot_id,
        .model        = "stackchan-cores3",
        .firmware     = esp_app_get_description()->version,
        .commands     = {"nod"},
        .measurements = {"battery_pct", "charging", "head_yaw_deg", "head_pitch_deg", "wifi_rssi_dbm",
                         "free_heap_kb"},
    });
    _client->onCommand = [this](const std::string& command) {
        _last_command      = command;
        _rendered_revision = UINT32_MAX;
        if (command == "nod") {
            start_nod();
        }
    };
    _client->collectTelemetry = []() {
        auto& motion = GetStackChan().motion();
        return embody::Client::Telemetry{
            {"battery_pct", (float)GetHAL().getBatteryLevel()},
            {"charging", GetHAL().isBatteryCharging() ? 1.0f : 0.0f},
            {"head_yaw_deg", motion.getCurrentYawAngle() / 10.0f},
            {"head_pitch_deg", motion.getCurrentPitchAngle() / 10.0f},
            {"wifi_rssi_dbm", (float)WifiManager::GetInstance().GetRssi()},
            {"free_heap_kb", (float)(esp_get_free_heap_size() / 1024)},
        };
    };
}

void AppEmbodyMode::onRunning()
{
    // Network I/O happens outside the LVGL lock; connecting may block.
    if (_client) {
        _client->update();
    }
    update_nod();

    LvglLockGuard lock;
    render();
    GetStackChan().update();
    view::update_home_indicator();
    view::update_status_bar();
}

void AppEmbodyMode::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _client.reset();

    {
        LvglLockGuard lock;
        _qr = nullptr;
        _detail.reset();
        _code.reset();
        _status.reset();
        _qr_hint.reset();
        _qr_box.reset();
        _title.reset();
        _panel.reset();
        view::destroy_home_indicator();
        view::destroy_status_bar();
    }
    _rendered_revision = UINT32_MAX;
    _rendered_url.clear();

    // Wi-Fi can't be torn down cleanly; reboot back to the launcher like AVATAR does.
    if (_network_started) {
        GetHAL().requestWarmReboot(_launcher_index);
    }
}

/* ---------------------------------- View ---------------------------------- */

void AppEmbodyMode::create_view()
{
    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setSize(320, 240);
    _panel->setBgColor(lv_color_hex(0xF4F6FF));
    _panel->setBorderWidth(0);
    _panel->setRadius(0);
    // Default theme padding would shift every child; lay out in absolute screen coords
    _panel->setPadding(0, 0, 0, 0);
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _title = std::make_unique<Label>(*_panel);
    _title->setText("Embody Mode");
    _title->setTextFont(&lv_font_montserrat_24);
    _title->setTextColor(lv_color_hex(_color_text));
    _title->align(LV_ALIGN_TOP_MID, 0, 10);

    // Left: QR code on a white card (y 50..206, above the home swipe zone)
    _qr_box = std::make_unique<Container>(*_panel);
    _qr_box->setSize(156, 156);
    _qr_box->align(LV_ALIGN_TOP_LEFT, 14, 50);
    _qr_box->setBgColor(lv_color_hex(0xFFFFFF));
    _qr_box->setBorderWidth(0);
    _qr_box->setRadius(12);
    _qr_box->setPadding(0, 0, 0, 0);
    _qr_box->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _qr = lv_qrcode_create(_qr_box->get());
    lv_qrcode_set_size(_qr, 136);
    lv_qrcode_set_dark_color(_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(_qr, lv_color_hex(0xFFFFFF));
    lv_obj_center(_qr);
    lv_obj_add_flag(_qr, LV_OBJ_FLAG_HIDDEN);

    _qr_hint = std::make_unique<Label>(*_qr_box);
    _qr_hint->setText("No code yet");
    _qr_hint->setTextFont(&lv_font_montserrat_16);
    _qr_hint->setTextColor(lv_color_hex(_color_muted));
    _qr_hint->align(LV_ALIGN_CENTER, 0, 0);

    // Right: status, pairing code, details
    _status = std::make_unique<Label>(*_panel);
    _status->setTextFont(&lv_font_montserrat_16);
    _status->setTextColor(lv_color_hex(_color_text));
    _status->setWidth(128);
    _status->setLongMode(LV_LABEL_LONG_MODE_WRAP);
    _status->align(LV_ALIGN_TOP_LEFT, 182, 56);
    _status->setText("Connecting to server...");

    _code = std::make_unique<Label>(*_panel);
    _code->setTextFont(&lv_font_montserrat_20);
    _code->setTextColor(lv_color_hex(_color_text));
    _code->align(LV_ALIGN_TOP_LEFT, 182, 146);
    _code->setText("");

    _detail = std::make_unique<Label>(*_panel);
    _detail->setTextFont(&lv_font_montserrat_16);
    _detail->setTextColor(lv_color_hex(_color_muted));
    _detail->setWidth(128);
    _detail->setLongMode(LV_LABEL_LONG_MODE_WRAP);
    _detail->align(LV_ALIGN_TOP_LEFT, 182, 176);
    _detail->setText("");

    view::create_home_indicator([&]() { close(); }, _color_theme, _color_text);
    view::create_status_bar(_color_theme, _color_text);
}

void AppEmbodyMode::render()
{
    if (!_client || !_panel) {
        return;
    }
    if (_client->revision() == _rendered_revision) {
        return;
    }
    _rendered_revision = _client->revision();

    _status->setText(_client->statusText());

    const auto& url = _client->pairUrl();
    if (url != _rendered_url) {
        _rendered_url = url;
        if (url.empty()) {
            lv_obj_add_flag(_qr, LV_OBJ_FLAG_HIDDEN);
            _qr_hint->setHidden(false);
        } else {
            lv_qrcode_update(_qr, url.data(), url.size());
            lv_obj_remove_flag(_qr, LV_OBJ_FLAG_HIDDEN);
            _qr_hint->setHidden(true);
        }
    }

    // Show the code in two groups of four so it is easy to read out
    const auto& code = _client->pairCode();
    _code->setText(code.size() == 8 ? code.substr(0, 4) + " " + code.substr(4) : code);
    _detail->setText(_last_command.empty() ? "" : "Last: " + _last_command);
}

/* ----------------------------------- Nod ---------------------------------- */

// Pitch offsets in 0.1 degree, relative to the pitch at the start of the nod.
static constexpr int _nod_offsets[]  = {-120, 100, -120, 0};
static constexpr uint32_t _nod_step_ms = 260;
static constexpr int _nod_speed        = 700;

void AppEmbodyMode::start_nod()
{
    if (_nod_step >= 0) {
        return;  // already nodding
    }
    auto& motion = GetStackChan().motion();
    // Hand-moved angle sync would fight commanded moves
    motion.setAutoAngleSyncEnabled(false);
    _nod_base_pitch = motion.getCurrentPitchAngle();
    _nod_step       = 0;
    _nod_next_tick  = GetHAL().millis();
    _angle_sync_tick = 0;
}

void AppEmbodyMode::update_nod()
{
    auto now = GetHAL().millis();

    if (_nod_step < 0) {
        if (_angle_sync_tick != 0 && (int32_t)(now - _angle_sync_tick) >= 0) {
            GetStackChan().motion().setAutoAngleSyncEnabled(true);
            _angle_sync_tick = 0;
        }
        return;
    }
    if ((int32_t)(now - _nod_next_tick) < 0) {
        return;
    }
    if (_nod_step >= (int)std::size(_nod_offsets)) {
        _nod_step        = -1;
        _angle_sync_tick = now + 1500;
        _rendered_revision = UINT32_MAX;  // refresh "Last: nod"
        return;
    }
    // Pitch servo range is 30..870 (0.1 degree)
    int target = std::clamp(_nod_base_pitch + _nod_offsets[_nod_step], 60, 840);
    GetStackChan().motion().movePitchWithSpeed(target, _nod_speed);
    _nod_step++;
    _nod_next_tick = now + _nod_step_ms;
}
