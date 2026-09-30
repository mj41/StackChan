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
#include <hal/board/hal_bridge.h>
#include <hal/utils/jpeg_to_image/jpeg_decoder.h>
#include <stackchan/stackchan.h>
#include <stackchan/avatar/decorators/decorators.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>
#include <wifi_manager.h>
#include <board.h>
#include <audio_codec.h>
#include <lvgl_image.h>
#include <jpg/image_to_jpeg.h>
#include <ArduinoJson.hpp>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <sdkconfig.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace stackchan;

static const char* _tag = "Embody Mode";

// Launcher index of this app (see requestWarmReboot in the other apps).
static constexpr int _launcher_index = 0;

static constexpr uint32_t _color_theme = 0x7D8CFF;
static constexpr uint32_t _color_text  = 0x1E2355;
static constexpr uint32_t _color_muted = 0x5A618E;

// Servo limits in 0.1 degree. Pitch stays within 5..85 degrees: M5Stack warns that
// extreme Y angles can stall the servo and damage it (HAL allows 3..87).
static constexpr int _yaw_min = -1280, _yaw_max = 1280;
static constexpr int _pitch_min = 50, _pitch_max = 850;
static constexpr int _motion_speed = 600;

static constexpr AppEmbodyMode::GestureStep _nod[]   = {{'p', -120}, {'p', 100}, {'p', -120}, {'p', 0}};
static constexpr AppEmbodyMode::GestureStep _shake[] = {{'y', -150}, {'y', 150}, {'y', -150}, {'y', 0}};
static constexpr uint32_t _gesture_step_ms           = 260;

// Binary message types (stackchan-server internal/wire)
static constexpr uint8_t _bin_camera_jpeg = 0x01;
static constexpr uint8_t _bin_audio_pcm   = 0x02;
static constexpr uint8_t _bin_speaker_pcm = 0x03;
static constexpr uint8_t _bin_show_jpeg   = 0x10;

static constexpr uint32_t _camera_interval_ms = 200;  // up to 5 fps
static constexpr int _camera_jpeg_quality     = 25;
static constexpr size_t _mic_max_buffer       = 24000;  // ~1 s at 24 kHz; older samples are dropped

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
    mclog::tagInfo(_tag, "on create");
}

void AppEmbodyMode::onOpen()
{
    mclog::tagInfo(_tag, "on open");

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

    // NFC reader (ST25R3916 on the internal I2C bus): "nfc" is offered only when it answers.
    _nfc = std::make_unique<ST25R3916>();
    if (!_nfc->begin(hal_bridge::board_get_i2c_bus())) {
        mclog::tagWarn(_tag, "nfc: no ST25R3916 reader");
        _nfc.reset();
    }
    // Light/proximity sensor in the CoreS3: auto-brightness is on when it answers.
    _light = std::make_unique<LTR553>();
    if (!_light->begin(hal_bridge::board_get_i2c_bus())) {
        mclog::tagWarn(_tag, "light: no LTR-553");
        _light.reset();
    }
    _auto_brightness = _light != nullptr;

    // Body battery monitor on the Power board.
    _body_power = std::make_unique<INA226>();
    if (!_body_power->begin(hal_bridge::board_get_i2c_bus())) {
        mclog::tagWarn(_tag, "power: no INA226");
        _body_power.reset();
    }
    hal_bridge::board_take_pmic_events();  // drop presses from before the app started

    // Infrared transmitter and receiver in the body.
    _ir = std::make_unique<IrRemote>();
    if (!_ir->begin(GPIO_NUM_5, GPIO_NUM_10)) {
        mclog::tagWarn(_tag, "ir: RMT setup failed");
        _ir.reset();
    }

    std::vector<std::string> commands = {"ping",    "nod",  "shake", "look",   "home", "emotion",     "say",
                                         "leds",    "brightness", "volume", "sticker", "face", "image",
                                         "camera",  "mic",  "screensaver", "standby", "speaker"};
    if (_nfc) {
        commands.push_back("nfc");
    }
    if (_ir) {
        commands.push_back("ir_send");
    }
    if (_light) {
        commands.push_back("proximity");
    }
    commands.push_back("power_led");

    _client = std::make_unique<embody::Client>(embody::Client::Config{
        .serverUrl = server_url,
        .token     = CONFIG_STACKCHAN_EMBODY_TOKEN,
        .robotId   = robot_id,
        .model     = "stackchan-cores3",
        .firmware  = esp_app_get_description()->version,
        .commands  = commands,
        .measurements = {"battery_pct", "charging", "head_yaw_deg", "head_pitch_deg", "wifi_rssi_dbm",
                         "free_heap_kb", "uptime_s", "brightness_pct", "volume_pct", "screensaver",
                         "imu_ax_g", "imu_ay_g", "imu_az_g", "imu_gyro_dps", "yaw_load_pct", "pitch_load_pct",
                         "yaw_temp_c", "pitch_temp_c", "servo_voltage_v", "chip_temp_c", "light_lux",
                         "proximity", "proximity_on", "auto_brightness", "core_battery_v", "core_vbus_v",
                         "core_system_v", "core_charge", "core_charge_phase", "pmic_temp_c", "pmic_status1",
                         "pmic_status2", "body_battery_v", "body_current_ma", "body_power_mw", "body_shunt_uv"},
    });
    _client->onCommand = [this](const std::string& command, const std::string& args) {
        _pending_commands.emplace_back(command, args);
    };
    _client->onBinary = [this](uint8_t type, const std::string& payload) {
        if (type == _bin_show_jpeg) {
            _pending_picture_jpeg = payload;  // decoded in the app loop, shown under the LVGL lock
        } else if (type == _bin_speaker_pcm) {
            queue_speaker_audio(payload);
        }
    };
    _client->collectTelemetry = [this]() {
        auto& motion = GetStackChan().motion();
        embody::Client::Telemetry t{
            {"battery_pct", (float)GetHAL().getBatteryLevel()},
            {"charging", GetHAL().isBatteryCharging() ? 1.0f : 0.0f},
            {"head_yaw_deg", motion.getCurrentYawAngle() / 10.0f},
            {"head_pitch_deg", motion.getCurrentPitchAngle() / 10.0f},
            {"wifi_rssi_dbm", (float)WifiManager::GetInstance().GetRssi()},
            {"free_heap_kb", (float)(esp_get_free_heap_size() / 1024)},
            {"uptime_s", (float)(esp_timer_get_time() / 1000000)},
            {"brightness_pct", (float)GetHAL().getBackLightBrightness()},
            {"volume_pct", (float)GetHAL().getSpeakerVolume()},
            {"screensaver", !_blank_screen ? 0.0f : (_blank_manual ? 2.0f : 1.0f)},  // 0 off, 1 auto, 2 manual
        };
        add_sensor_telemetry(t);
        return t;
    };

    _imu_connection = GetHAL().onImuMotionEvent.connect([this](ImuMotionEvent event) {
        if (event == ImuMotionEvent::Shake) {
            queue_event("shake");
        } else if (event == ImuMotionEvent::PickUp) {
            queue_event("pickup");
        }
    });
    _head_connection = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture gesture) {
        _last_physical = GetHAL().millis();  // wakes the screen like a touch
        switch (gesture) {
            case HeadPetGesture::Press: {
                // Zone intensities 0-3, in the order a forward swipe crosses them
                auto z         = GetHAL().getHeadTouchZones();
                _head_press_ms = GetHAL().millis();
                queue_event("head_press", {{"z0", (float)z[0]}, {"z1", (float)z[1]}, {"z2", (float)z[2]}});
                break;
            }
            case HeadPetGesture::Release:
                queue_event("head_release", {{"ms", (float)(GetHAL().millis() - _head_press_ms)}});
                break;
            case HeadPetGesture::SwipeForward:
                queue_event("head_swipe_forward");
                break;
            case HeadPetGesture::SwipeBackward:
                queue_event("head_swipe_backward");
                break;
            default:
                break;
        }
    });

    if (_nfc) {
        _nfc_enabled = true;
        start_nfc();
    }

    temperature_sensor_config_t tsens_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&tsens_config, &_tsens) != ESP_OK) {
        _tsens = nullptr;
    } else if (temperature_sensor_enable(_tsens) != ESP_OK) {
        temperature_sensor_uninstall(_tsens);
        _tsens = nullptr;
    }
}

void AppEmbodyMode::onRunning()
{
    // Network I/O, JPEG work and camera capture happen outside the LVGL lock.
    if (_client) {
        _client->update();

        // Using the robot remotely keeps the screensaver away: commands (even ping) and live media.
        if (_client->commandCount() != _seen_commands) {
            _seen_commands  = _client->commandCount();
            _last_activity = GetHAL().millis();
        }
        if (_camera_on || _mic_running) {
            _last_activity = GetHAL().millis();
        }

        // Events wait while offline (e.g. during standby) and go out after reconnecting.
        std::vector<PendingEvent> events;
        if (_client->isRegistered()) {
            std::lock_guard<std::mutex> lock(_event_mutex);
            events.swap(_pending_events);
        }
        for (const auto& event : events) {
            _client->sendEvent(event.name, event.data, event.text);
        }
        if (_standby_disconnect) {  // the "standby" event is out; now go offline
            _standby_disconnect = false;
            _client->standby(_standby_until - GetHAL().millis());
        }

        if (!_pending_picture_jpeg.empty()) {
            auto image = jpeg_dec::decode_to_lvgl((const uint8_t*)_pending_picture_jpeg.data(),
                                                  _pending_picture_jpeg.size());
            if (image) {
                _pending_picture = image;
            } else {
                mclog::tagWarn(_tag, "picture: JPEG decode failed ({} bytes)", _pending_picture_jpeg.size());
            }
            _pending_picture_jpeg.clear();
        }

        if (_camera_on && GetHAL().millis() - _last_frame_tick >= _camera_interval_ms) {
            _last_frame_tick = GetHAL().millis();
            send_camera_frame();
        }
        send_mic_audio();
    }

    update_leds();
    update_light();
    update_ir();
    update_power_events();

    LvglLockGuard lock;
    for (const auto& [command, args] : _pending_commands) {
        run_command(command, args);
    }
    _pending_commands.clear();

    if (_pending_picture) {
        _picture = std::move(_pending_picture);  // keep the pixels alive while shown
        lv_image_set_src(_picture_obj, _picture->image_dsc());
        lv_obj_remove_flag(_picture_obj, LV_OBJ_FLAG_HIDDEN);
        wake_screen();
    }
    if (_live_badge) {
        bool live = _camera_on || _mic_running;
        if (live == lv_obj_has_flag(_live_badge, LV_OBJ_FLAG_HIDDEN)) {
            live ? lv_obj_remove_flag(_live_badge, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(_live_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Move the mouth while the speaker plays
    const uint32_t now = GetHAL().millis();
    if (now - _spk_last_audio < 300 && (int32_t)(now - _speaking_until) > -200 && GetStackChan().hasAvatar()) {
        GetStackChan().addModifier(std::make_unique<SpeakingModifier>(800));
        _speaking_until = now + 800;
    }

    update_motion();
    render();
    update_standby();
    update_screensaver();
    GetStackChan().update();
    view::update_home_indicator();
    view::update_status_bar();
}

void AppEmbodyMode::onClose()
{
    mclog::tagInfo(_tag, "on close");

    stop_mic();
    stop_speaker();
    stop_nfc();
    _nfc.reset();
    _nfc_enabled = false;
    stop_led_effect(false);
    _light.reset();  // standby: the IR LED stops
    _ir.reset();
    if (_tsens) {
        temperature_sensor_disable(_tsens);
        temperature_sensor_uninstall(_tsens);
        _tsens = nullptr;
    }
    _camera_on = false;
    if (_client) {
        GetHAL().onImuMotionEvent.disconnect(_imu_connection);
        GetHAL().onHeadPetGesture.disconnect(_head_connection);
    }
    _client.reset();
    _pending_commands.clear();

    {
        LvglLockGuard lock;
        if (_blank_screen) {  // load the Embody screen back before its objects are deleted
            lv_screen_load(_prev_screen);
            lv_obj_delete(_blank_screen);
            _blank_screen = nullptr;
        }
        GetStackChan().clearModifiers();
        GetStackChan().resetAvatar();
        if (_live_badge) {
            lv_obj_delete(_live_badge);
            _live_badge = nullptr;
        }
        if (_picture_obj) {
            lv_obj_delete(_picture_obj);
            _picture_obj = nullptr;
        }
        _picture.reset();
        _pending_picture.reset();
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
    _qr_visible = true;

    // Wi-Fi can't be torn down cleanly; reboot back to the launcher like AVATAR does.
    if (_network_started) {
        GetHAL().requestWarmReboot(_launcher_index);
    }
}

/* ---------------------------------- View ---------------------------------- */

// Tap -> "screen_tap" event with coordinates; long press -> toggle the QR panel.
// Runs in the LVGL task, so it only queues.
void AppEmbodyMode::on_screen_event(lv_event_t* e)
{
    auto* self = static_cast<AppEmbodyMode*>(lv_event_get_user_data(e));
    auto code  = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        self->_toggle_qr_requested = true;
    } else if (code == LV_EVENT_DOUBLE_CLICKED) {
        self->_blank_requested = true;
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        lv_point_t p{};
        if (auto* indev = lv_indev_active()) {
            lv_indev_get_point(indev, &p);
        }
        self->queue_event("screen_tap", {{"x", (float)p.x}, {"y", (float)p.y}});
    }
}

void AppEmbodyMode::create_view()
{
    auto listen = [this](lv_obj_t* obj) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(obj, on_screen_event, LV_EVENT_SHORT_CLICKED, this);
        lv_obj_add_event_cb(obj, on_screen_event, LV_EVENT_LONG_PRESSED, this);
        lv_obj_add_event_cb(obj, on_screen_event, LV_EVENT_DOUBLE_CLICKED, this);
    };

    // Face at the bottom, then a picture layer, then the QR panel on top.
    auto avatar = std::make_unique<avatar::DefaultAvatar>();
    avatar->init(lv_screen_active());
    listen(avatar->getPanel()->get());
    GetStackChan().attachAvatar(std::move(avatar));

    _picture_obj = lv_image_create(lv_screen_active());
    lv_obj_set_size(_picture_obj, 320, 240);
    lv_obj_align(_picture_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(_picture_obj, LV_OBJ_FLAG_HIDDEN);
    listen(_picture_obj);

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setSize(320, 240);
    _panel->setBgColor(lv_color_hex(0xF4F6FF));
    _panel->setBorderWidth(0);
    _panel->setRadius(0);
    // Default theme padding would shift every child; lay out in absolute screen coords
    _panel->setPadding(0, 0, 0, 0);
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    listen(_panel->get());

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
    _qr_box->removeFlag(LV_OBJ_FLAG_CLICKABLE);

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

    // Privacy indicator above everything while the camera or microphone streams
    _live_badge = lv_label_create(lv_layer_top());
    lv_label_set_text(_live_badge, LV_SYMBOL_EYE_OPEN " LIVE");
    lv_obj_set_style_text_font(_live_badge, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_live_badge, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(_live_badge, lv_color_hex(0xE0245E), 0);
    lv_obj_set_style_bg_opa(_live_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_live_badge, 8, 0);
    lv_obj_set_style_pad_hor(_live_badge, 8, 0);
    lv_obj_set_style_pad_ver(_live_badge, 2, 0);
    lv_obj_align(_live_badge, LV_ALIGN_TOP_LEFT, 6, 32);
    lv_obj_add_flag(_live_badge, LV_OBJ_FLAG_HIDDEN);

    view::create_home_indicator([&]() { close(); }, _color_theme, _color_text);
    view::create_status_bar(_color_theme, _color_text);
}

void AppEmbodyMode::render()
{
    if (!_client || !_panel) {
        return;
    }

    // The first browser to pair switches to the face; a long press toggles the QR.
    bool changed = false;
    if (_client->viewers() > 0 && _rendered_viewers == 0) {
        _qr_visible = false;
        changed     = true;
        wake_screen();
    }
    _rendered_viewers = _client->viewers();
    if (_toggle_qr_requested.exchange(false) && _client->isRegistered()) {
        _qr_visible = !_qr_visible;
        changed     = true;
    }
    if (changed) {
        _panel->setHidden(!_qr_visible);
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
    if (_client->viewers() > 0) {
        _detail->setText("Long press to show the face");
    } else {
        _detail->setText(_last_command.empty() ? "" : "Last: " + _last_command);
    }
}

// Sensor values for the 2 s telemetry. Runs in the app loop, which also drives
// the servos, so reading the servo bus here is safe.
void AppEmbodyMode::add_sensor_telemetry(embody::Client::Telemetry& t)
{
    auto round_to = [](float v, float step) { return std::round(v / step) * step; };

    ImuSample_t imu;
    if (GetHAL().getImuSample(imu)) {
        constexpr float g = 9.80665f;  // m/s^2 per g
        t.emplace_back("imu_ax_g", round_to(imu.accel[0] / g, 0.01f));
        t.emplace_back("imu_ay_g", round_to(imu.accel[1] / g, 0.01f));
        t.emplace_back("imu_az_g", round_to(imu.accel[2] / g, 0.01f));
        t.emplace_back("imu_gyro_dps", round_to(std::hypot(imu.gyro[0], imu.gyro[1], imu.gyro[2]), 0.1f));
    }

    ServoStatus_t servo;
    if (GetHAL().readServoStatus(1, servo)) {
        t.emplace_back("yaw_load_pct", servo.load / 10.0f);
        t.emplace_back("yaw_temp_c", (float)servo.temperature);
        t.emplace_back("servo_voltage_v", servo.voltage);
    }
    if (GetHAL().readServoStatus(2, servo)) {
        t.emplace_back("pitch_load_pct", servo.load / 10.0f);
        t.emplace_back("pitch_temp_c", (float)servo.temperature);
    }

    if (_light) {
        if (_lux >= 0) {
            t.emplace_back("light_lux", round_to(_lux, _lux < 10 ? 0.1f : 1.0f));
        }
        if (_proximity_on) {
            t.emplace_back("proximity", (float)_proximity);
        }
        t.emplace_back("proximity_on", _proximity_on ? 1.0f : 0.0f);
        t.emplace_back("auto_brightness", _auto_brightness ? 1.0f : 0.0f);
    }

    // CoreS3 power chip (its own battery and USB input), raw
    hal_bridge::PmicStatus pmic;
    if (hal_bridge::board_get_pmic_status(pmic)) {
        t.emplace_back("core_battery_v", pmic.battery_mv / 1000.0f);
        t.emplace_back("core_vbus_v", pmic.vbus_mv / 1000.0f);
        t.emplace_back("core_system_v", pmic.system_mv / 1000.0f);
        t.emplace_back("core_charge", (float)((pmic.status2 >> 5) & 0x03));  // 0 idle, 1 charging, 2 discharging
        t.emplace_back("core_charge_phase", (float)(pmic.status2 & 0x07));  // 0 trickle .. 3 CV, 4 done, 5 not charging
        t.emplace_back("pmic_temp_c", round_to(pmic.die_temp_c, 0.1f));
        t.emplace_back("pmic_status1", (float)pmic.status1);
        t.emplace_back("pmic_status2", (float)pmic.status2);
    }
    // Body battery (INA226 on the Power board), raw
    INA226::Reading body;
    if (_body_power && _body_power->read(body)) {
        t.emplace_back("body_battery_v", round_to(body.bus_v, 0.001f));
        t.emplace_back("body_current_ma", round_to(body.current_ma, 0.1f));
        t.emplace_back("body_power_mw", round_to(body.power_mw, 1.0f));
        t.emplace_back("body_shunt_uv", body.shunt_uv);
    }

    float celsius = 0;
    if (_tsens && temperature_sensor_get_celsius(_tsens, &celsius) == ESP_OK) {
        t.emplace_back("chip_temp_c", round_to(celsius, 0.1f));
    }
}

void AppEmbodyMode::queue_event(const char* name, embody::Client::Telemetry data, embody::Client::Texts text)
{
    std::lock_guard<std::mutex> lock(_event_mutex);
    if (_pending_events.size() < 16) {
        _pending_events.push_back({name, std::move(data), std::move(text)});
    }
}

/* -------------------------------- Commands -------------------------------- */

static bool parse_hex_color(const char* text, uint32_t& out)
{
    if (!text || text[0] != '#' || std::strlen(text) != 7) {
        return false;
    }
    char* end = nullptr;
    out       = std::strtoul(text + 1, &end, 16);
    return end && *end == '\0';
}

// Runs under the LVGL lock (avatar and modifiers draw).
void AppEmbodyMode::run_command(const std::string& command, const std::string& args_json)
{
    ArduinoJson::JsonDocument args;
    ArduinoJson::deserializeJson(args, args_json);

    if (command != "camera" && command != "mic") {  // those come from the server, not the user
        _last_command      = command;
        _rendered_revision = UINT32_MAX;  // refresh "Last: ..."
    }

    auto& sc     = GetStackChan();
    auto& motion = sc.motion();

    if (command == "nod") {
        start_gesture(_nod, std::size(_nod));
    } else if (command == "shake") {
        start_gesture(_shake, std::size(_shake));
    } else if (command == "look") {
        pause_angle_sync();
        _gesture  = nullptr;
        int yaw   = std::clamp((int)((args["yaw"] | 0.0f) * 10), _yaw_min, _yaw_max);
        int pitch = std::clamp((int)((args["pitch"] | 45.0f) * 10), _pitch_min, _pitch_max);
        motion.moveWithSpeed(yaw, pitch, _motion_speed);
    } else if (command == "home") {
        pause_angle_sync();
        _gesture = nullptr;
        motion.goHome(_motion_speed);
    } else if (command == "emotion") {
        static const std::pair<const char*, avatar::Emotion> emotions[] = {
            {"neutral", avatar::Emotion::Neutral}, {"happy", avatar::Emotion::Happy},
            {"angry", avatar::Emotion::Angry},     {"sad", avatar::Emotion::Sad},
            {"doubt", avatar::Emotion::Doubt},     {"sleepy", avatar::Emotion::Sleepy},
        };
        std::string name = args["name"] | "";
        for (const auto& [key, emotion] : emotions) {
            if (name == key && sc.hasAvatar()) {
                sc.avatar().setEmotion(emotion);
            }
        }
    } else if (command == "say") {
        std::string text = args["text"] | "";
        if (text.size() > 80) {
            text.resize(80);
        }
        uint32_t ms = std::clamp((int)((args["seconds"] | 6.0f) * 1000), 1000, 30000);
        if (!text.empty() && sc.hasAvatar()) {
            sc.addModifier(std::make_unique<TimedSpeechModifier>(text, ms));
            sc.addModifier(std::make_unique<SpeakingModifier>(std::min<uint32_t>(ms, 3000)));
        }
    } else if (command == "sticker") {
        std::string name = args["name"] | "";
        uint32_t ms      = std::clamp((int)((args["seconds"] | 4.0f) * 1000), 500, 30000);
        if (sc.hasAvatar()) {
            auto* parent = lv_screen_active();
            std::unique_ptr<avatar::Decorator> d;
            if (name == "heart") {
                d = std::make_unique<avatar::HeartDecorator>(parent, ms, 500);
            } else if (name == "angry") {
                d = std::make_unique<avatar::AngryDecorator>(parent, ms, 500);
            } else if (name == "sweat") {
                d = std::make_unique<avatar::SweatDecorator>(parent, ms, 700);
            } else if (name == "shy") {
                d = std::make_unique<avatar::ShyDecorator>(parent, ms);
            } else if (name == "dizzy") {
                d = std::make_unique<avatar::DizzyDecorator>(parent, ms, 300);
            }
            if (d) {
                sc.avatar().addDecorator(std::move(d));
            }
        }
    } else if (command == "face") {
        lv_obj_add_flag(_picture_obj, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(_picture_obj, nullptr);
        _picture.reset();
    } else if (command == "standby") {
        start_standby(args["minutes"] | 5);
    } else if (command == "screensaver") {
        (args["on"] | false) ? enter_blank(true) : leave_blank();
    } else if (command == "camera") {
        _camera_on = args["on"] | false;
        // StackChanCamera logs a warning for every captured frame; keep the log readable.
        esp_log_level_set("StackChanCamera", _camera_on ? ESP_LOG_ERROR : ESP_LOG_INFO);
        mclog::tagInfo(_tag, "camera {}", _camera_on ? "on" : "off");
    } else if (command == "mic") {
        (args["on"] | false) ? start_mic() : stop_mic();
    } else if (command == "ir_send") {
        // {"raw": "9000,4500,562,..."} marks/spaces in us (+ "carrier_hz"), or NEC {"address", "command"};
        // "loopback": true lets the receiver hear the robot's own signal (self-test).
        const std::string raw = args["raw"] | "";
        const bool loopback   = args["loopback"] | false;
        const int repeats     = std::clamp(args["repeat"] | 0, 0, 20);  // like holding the button
        const int frames      = std::clamp(args["frames"] | 1, 1, 5);   // whole frame N times (weak links)
        bool ok               = false;
        // The proximity sensor's IR LED pulses ~10x/s next to ours: pause it while sending.
        const bool pause_proximity = _light && _proximity_on;
        if (pause_proximity) {
            _light->setProximityEnabled(false);
        }
        if (_ir && !raw.empty()) {
            ok = _ir->send(IrRemote::repeated(IrRemote::timingsFromString(raw), std::max(repeats, frames - 1)),
                           args["carrier_hz"] | 38000, loopback);
        } else if (_ir) {
            ok = _ir->sendNec(args["address"] | 0, args["command"] | 0, loopback, repeats, frames);
        }
        if (pause_proximity) {
            _light->setProximityEnabled(true);
        }
        mclog::tagInfo(_tag, "ir: sent {} ({})", raw.empty() ? "NEC" : "raw", ok ? "ok" : "failed");
    } else if (command == "proximity") {
        // Off: the LTR-553's IR LED (next to the camera) stops pulsing; light and auto-brightness keep working.
        _proximity_on = args["on"] | true;
        if (_light) {
            _light->setProximityEnabled(_proximity_on);
        }
        if (!_proximity_on && _near) {
            _near = false;
            queue_event("proximity_far", {{"value", 0.0f}});
        }
        _prox_base = -1;  // re-learn the baseline when it comes back
        mclog::tagInfo(_tag, "proximity {}", _proximity_on ? "on" : "off");
    } else if (command == "power_led") {
        // Red power/charge LED on the PMIC
        static constexpr std::pair<const char*, int> modes[] = {
            {"off", 0}, {"blink", 1}, {"fast", 2}, {"on", 3}, {"charging", 4}};
        const std::string mode = args["mode"] | "";
        for (const auto& [name, value] : modes) {
            if (mode == name) {
                hal_bridge::board_set_charge_led(value);
            }
        }
    } else if (command == "nfc") {
        _nfc_enabled = _nfc && (args["on"] | true);
        _nfc_enabled ? start_nfc() : stop_nfc();
    } else if (command == "leds") {
        run_leds(args);
    } else if (command == "brightness") {
        // {"value": 1..100} sets it by hand and ends auto; {"auto": bool} switches auto.
        if (args["value"].is<int>() || args["value"].is<float>()) {
            _auto_brightness = false;
            GetHAL().setBackLightBrightness(std::clamp(args["value"] | 60, 1, 100));
        }
        if (args["auto"].is<bool>()) {
            _auto_brightness  = _light && args["auto"].as<bool>();
            _last_light_read = 0;  // apply now
        }
    } else if (command == "volume") {
        GetHAL().setSpeakerVolume(std::clamp(args["value"] | 50, 0, 100));
    } else {
        mclog::tagWarn(_tag, "unknown command: {}", command);
    }
}

/* ------------------------------ Camera and mic ----------------------------- */

void AppEmbodyMode::send_camera_frame()
{
    auto camera = hal_bridge::board_get_camera();
    if (!camera || !camera->StreamCaptures()) {
        return;
    }
    uint8_t* jpeg = nullptr;
    size_t len    = 0;
    if (image_to_jpeg((uint8_t*)camera->GetFrameData(), camera->GetFrameSize(), camera->GetFrameWidth(),
                      camera->GetFrameHeight(), (v4l2_pix_fmt_t)camera->GetFrameFormat(), _camera_jpeg_quality, &jpeg,
                      &len)) {
        if (jpeg) {
            _client->sendBinary(_bin_camera_jpeg, jpeg, len);
            free(jpeg);
        }
    }
}

void AppEmbodyMode::start_mic()
{
    if (_mic_running) {
        return;
    }
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        mclog::tagWarn(_tag, "mic: no audio codec");
        return;
    }
    _mic_rate = codec->input_sample_rate();
    {
        std::lock_guard<std::mutex> lock(_mic_mutex);
        _mic_samples.clear();
    }
    _mic_running = true;
    TaskHandle_t handle = nullptr;
    if (xTaskCreate(mic_task, "embody_mic", 4096, this, 5, &handle) != pdPASS) {
        _mic_running = false;
        _mic_task    = nullptr;
        mclog::tagError(_tag, "mic: task create failed");
        return;
    }
    _mic_task = handle;
    mclog::tagInfo(_tag, "mic on, {} Hz", _mic_rate);
}

void AppEmbodyMode::stop_mic()
{
    if (!_mic_running) {
        return;
    }
    _mic_running = false;
    // The task disables the codec input and clears _mic_task when it exits.
    for (int i = 0; i < 50 && _mic_task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    mclog::tagInfo(_tag, "mic off");
}

void AppEmbodyMode::mic_task(void* arg)
{
    auto* self  = static_cast<AppEmbodyMode*>(arg);
    auto codec  = Board::GetInstance().GetAudioCodec();
    int channels = std::max(codec->input_channels(), 1);
    // 20 ms per read; with several channels, the microphone is channel 1 (as in the SETUP mic test)
    std::vector<int16_t> chunk((self->_mic_rate / 50) * channels);

    codec->EnableInput(true);
    while (self->_mic_running) {
        if (!codec->InputData(chunk)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        std::lock_guard<std::mutex> lock(self->_mic_mutex);
        for (size_t i = 0; i < chunk.size() / channels; i++) {
            self->_mic_samples.push_back(chunk[i * channels + (channels > 1 ? 1 : 0)]);
        }
        if (self->_mic_samples.size() > _mic_max_buffer) {
            self->_mic_samples.erase(self->_mic_samples.begin(),
                                     self->_mic_samples.end() - _mic_max_buffer);
        }
    }
    codec->EnableInput(false);
    self->_mic_task = nullptr;
    vTaskDelete(nullptr);
}

// Sends buffered microphone audio: sample rate (uint16 LE), then s16le mono PCM.
void AppEmbodyMode::send_mic_audio()
{
    if (!_mic_running) {
        return;
    }
    std::vector<int16_t> samples;
    {
        std::lock_guard<std::mutex> lock(_mic_mutex);
        if (_mic_samples.size() < (size_t)(_mic_rate / 50)) {
            return;  // wait for at least 20 ms
        }
        samples.swap(_mic_samples);
    }
    const size_t per_message = _mic_rate / 20;  // 50 ms
    for (size_t off = 0; off < samples.size(); off += per_message) {
        size_t n = std::min(per_message, samples.size() - off);
        std::string msg;
        msg.push_back((char)(_mic_rate & 0xFF));
        msg.push_back((char)(_mic_rate >> 8));
        msg.append((const char*)(samples.data() + off), n * sizeof(int16_t));
        _client->sendBinary(_bin_audio_pcm, (const uint8_t*)msg.data(), msg.size());
    }
}

/* --------------------------------- Motion --------------------------------- */

void AppEmbodyMode::pause_angle_sync()
{
    if (!_angle_sync_paused) {
        GetStackChan().motion().setAutoAngleSyncEnabled(false);
        _angle_sync_paused = true;
    }
    _last_motion_tick = GetHAL().millis();
}

void AppEmbodyMode::start_gesture(const GestureStep* steps, size_t count)
{
    if (_gesture) {
        return;  // one gesture at a time
    }
    pause_angle_sync();
    auto& motion        = GetStackChan().motion();
    _gesture_base_yaw   = motion.getCurrentYawAngle();
    _gesture_base_pitch = motion.getCurrentPitchAngle();
    _gesture            = steps;
    _gesture_len        = count;
    _gesture_step       = 0;
    _gesture_next_tick  = GetHAL().millis();
}

void AppEmbodyMode::update_motion()
{
    auto now = GetHAL().millis();

    if (_gesture && (int32_t)(now - _gesture_next_tick) >= 0) {
        if (_gesture_step >= _gesture_len) {
            _gesture = nullptr;
        } else {
            const auto& step = _gesture[_gesture_step++];
            auto& motion     = GetStackChan().motion();
            if (step.axis == 'y') {
                motion.moveYawWithSpeed(std::clamp(_gesture_base_yaw + step.offset, _yaw_min, _yaw_max), 700);
            } else {
                motion.movePitchWithSpeed(std::clamp(_gesture_base_pitch + step.offset, _pitch_min, _pitch_max), 700);
            }
            _gesture_next_tick = now + _gesture_step_ms;
            _last_motion_tick  = now;
        }
    }

    // Let hand-moved-angle tracking resume a while after the last commanded move.
    if (_angle_sync_paused && !_gesture && now - _last_motion_tick > 2000) {
        GetStackChan().motion().setAutoAngleSyncEnabled(true);
        _angle_sync_paused = false;
    }
}

/* ------------------------------- Screensaver ------------------------------- */

// Blank screen, two kinds (runs under the LVGL lock):
// - auto: after CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S without touch, command or
//   live media; any of those ends it.
// - manual: a double tap or the "screensaver" command; only a touch or
//   "screensaver off" ends it, so remote use doesn't undo it.
void AppEmbodyMode::update_screensaver()
{
    if (!_panel || _standby_until) {
        return;  // standby owns the screen
    }
    const uint32_t now        = GetHAL().millis();
    const uint32_t touch_idle = touch_idle_ms();
    const uint32_t use_idle   = std::min(touch_idle, now - _last_activity);

    if (_blank_requested.exchange(false)) {
        if (_blank_screen) {
            _blank_manual = true;
        } else {
            enter_blank(true);
        }
        return;
    }

    if (_blank_screen) {
        bool touched = touch_idle < now - _blank_since;  // a touch after the screen went blank
        if (touched || (!_blank_manual && use_idle < now - _blank_since)) {
            leave_blank();
        }
        return;
    }

#if CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S > 0
    if (use_idle >= CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S * 1000u) {
        enter_blank(false);
    }
#endif
}

void AppEmbodyMode::enter_blank(bool manual)
{
    if (_blank_screen) {
        _blank_manual = _blank_manual || manual;
        return;
    }
    _prev_screen  = lv_screen_active();
    _blank_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_blank_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_blank_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(_blank_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(_blank_screen);
    _blank_manual = manual;
    _blank_since  = GetHAL().millis();
    queue_event("screensaver_on", {{"manual", manual ? 1.0f : 0.0f}});
    mclog::tagInfo(_tag, "screensaver on ({})", manual ? "manual" : "auto");
}

void AppEmbodyMode::leave_blank()
{
    if (!_blank_screen) {
        return;
    }
    lv_screen_load(_prev_screen);
    lv_obj_delete(_blank_screen);
    _blank_screen  = nullptr;
    _blank_manual  = false;
    _last_activity = GetHAL().millis();  // don't blank again right away
    queue_event("screensaver_off");
    mclog::tagInfo(_tag, "screensaver off");
}

// Counts as use (keeps or ends an auto screensaver), e.g. a new picture.
void AppEmbodyMode::wake_screen()
{
    _last_activity = GetHAL().millis();
}

// Time since someone touched the robot: the screen, the head, or an NFC tag.
uint32_t AppEmbodyMode::touch_idle_ms()
{
    return std::min<uint32_t>(lv_display_get_inactive_time(nullptr), GetHAL().millis() - _last_physical);
}

/* --------------------------------- Standby -------------------------------- */

// Soft standby for N minutes: tell the server, then go offline with the
// backlight off, a blank screen, and LEDs, camera and microphone off. The
// servos already release torque when idle. A touch or the timeout ends it.
// Runs under the LVGL lock.
void AppEmbodyMode::start_standby(int minutes)
{
    minutes = std::clamp(minutes, 1, 120);
    stop_mic();
    stop_speaker();
    stop_nfc();
    _camera_on = false;
    auto& sc   = GetStackChan();
    stop_led_effect(false);
    _led_left = _led_right = 0;
    sc.leftNeonLight().setColor((uint32_t)0);
    sc.rightNeonLight().setColor((uint32_t)0);
    enter_blank(true);

    _standby_brightness = GetHAL().getBackLightBrightness();
    GetHAL().setBackLightBrightness(0);
    _standby_since      = GetHAL().millis();
    _standby_until      = _standby_since + minutes * 60000u;
    if (_standby_until == 0) {
        _standby_until = 1;  // 0 means "not in standby"
    }
    queue_event("standby", {{"minutes", (float)minutes}});
    _standby_disconnect = true;
    mclog::tagInfo(_tag, "standby for {} min", minutes);
}

void AppEmbodyMode::update_standby()
{
    if (!_standby_until) {
        return;
    }
    const uint32_t now = GetHAL().millis();
    bool touched       = touch_idle_ms() < now - _standby_since;
    if (!touched && (int32_t)(now - _standby_until) < 0) {
        return;
    }
    _standby_until = 0;
    GetHAL().setBackLightBrightness(_standby_brightness);
    leave_blank();
    queue_event("standby_end", {{"touched", touched ? 1.0f : 0.0f}});
    if (_nfc_enabled) {
        start_nfc();
    }
    if (_client) {
        _client->wakeNow();
    }
    mclog::tagInfo(_tag, "standby end ({})", touched ? "touch" : "timeout");
}

/* --------------------------------- Speaker -------------------------------- */

// Browser audio: sample rate (uint16 LE), then s16le mono. Resampled to the
// codec rate and queued (at most ~3 s; the oldest audio goes first). Main loop.
void AppEmbodyMode::queue_speaker_audio(const std::string& payload)
{
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || payload.size() < 4) {
        return;
    }
    const int in_rate  = (uint8_t)payload[0] | ((uint8_t)payload[1] << 8);
    const int out_rate = codec->output_sample_rate();
    const size_t n_in  = (payload.size() - 2) / 2;
    if (in_rate <= 0 || out_rate <= 0 || n_in == 0) {
        return;
    }
    auto sample = [&](size_t i) {
        int16_t s;
        std::memcpy(&s, payload.data() + 2 + i * 2, 2);
        return s;
    };
    const size_t n_out = (size_t)((uint64_t)n_in * out_rate / in_rate);
    {
        std::lock_guard<std::mutex> lock(_spk_mutex);
        for (size_t i = 0; i < n_out; i++) {  // linear interpolation
            float x  = (float)i * in_rate / out_rate;
            size_t j = (size_t)x;
            float t  = x - j;
            int16_t a = sample(std::min(j, n_in - 1)), b = sample(std::min(j + 1, n_in - 1));
            _spk_samples.push_back((int16_t)(a + (b - a) * t));
        }
        const size_t max_samples = (size_t)out_rate * 3;
        while (_spk_samples.size() > max_samples) {
            _spk_samples.pop_front();
        }
    }
    _last_activity = GetHAL().millis();  // someone is talking through the robot

    if (!_spk_running) {
        _spk_running = true;
        TaskHandle_t handle = nullptr;
        if (xTaskCreate(speaker_task, "embody_spk", 4096, this, 5, &handle) != pdPASS) {
            _spk_running = false;
            mclog::tagError(_tag, "speaker: task create failed");
            return;
        }
        _spk_task = handle;
        mclog::tagInfo(_tag, "speaker on, {} Hz from {} Hz", out_rate, in_rate);
    }
}

void AppEmbodyMode::stop_speaker()
{
    if (!_spk_running) {
        return;
    }
    _spk_running = false;
    for (int i = 0; i < 50 && _spk_task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    std::lock_guard<std::mutex> lock(_spk_mutex);
    _spk_samples.clear();
}

// Plays queued audio in 20 ms chunks; switches the codec output off after
// 0.5 s of silence.
void AppEmbodyMode::speaker_task(void* arg)
{
    auto* self  = static_cast<AppEmbodyMode*>(arg);
    auto codec  = Board::GetInstance().GetAudioCodec();
    const size_t chunk_len = codec->output_sample_rate() / 50;
    std::vector<int16_t> chunk;
    chunk.reserve(chunk_len);
    bool enabled        = false;
    uint32_t idle_since = GetHAL().millis();

    while (self->_spk_running) {
        chunk.clear();
        {
            std::lock_guard<std::mutex> lock(self->_spk_mutex);
            while (!self->_spk_samples.empty() && chunk.size() < chunk_len) {
                chunk.push_back(self->_spk_samples.front());
                self->_spk_samples.pop_front();
            }
        }
        if (!chunk.empty()) {
            if (!enabled) {
                codec->EnableOutput(true);
                enabled = true;
            }
            codec->OutputData(chunk);  // blocks for about the chunk's duration
            idle_since            = GetHAL().millis();
            self->_spk_last_audio = idle_since;
        } else {
            if (enabled && GetHAL().millis() - idle_since > 500) {
                codec->EnableOutput(false);
                enabled = false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (enabled) {
        codec->EnableOutput(false);
    }
    self->_spk_task = nullptr;
    vTaskDelete(nullptr);
}

/* ----------------------------- Light, proximity ---------------------------- */

static constexpr uint32_t _prox_interval_ms  = 200;
static constexpr uint32_t _light_interval_ms = 500;
// Proximity above the "nobody near" baseline: enter near / leave near (hysteresis).
static constexpr float _prox_near_delta = 150;
static constexpr float _prox_far_delta  = 60;

// Room light to backlight: 10 % in the dark, about 55 % at 100 lux (a living
// room), 100 % from 10k lux (daylight).
static int brightness_for_lux(float lux)
{
    return std::clamp((int)std::lround(10 + 22.5f * std::log10(std::max(lux, 1.0f))), 10, 100);
}

void AppEmbodyMode::update_light()
{
    if (!_light) {
        return;
    }
    const uint32_t now = GetHAL().millis();

    uint16_t ps = 0;
    if (_proximity_on && now - _last_prox_read >= _prox_interval_ms && _light->readProximity(ps)) {
        _last_prox_read = now;
        _proximity      = ps;
        if (_prox_base < 0) {
            _prox_base = ps;
        }
        if (!_near && ps > _prox_base + _prox_near_delta) {
            _near = true;
            if (!_standby_until) {  // someone came close: wake like a touch (standby keeps sleeping)
                _last_physical = now;
            }
            queue_event("proximity_near", {{"value", (float)ps}});
        } else if (_near && ps < _prox_base + _prox_far_delta) {
            _near = false;
            queue_event("proximity_far", {{"value", (float)ps}});
        }
        // The baseline follows drops at once and rises slowly (~40 s), so
        // reflections from the cover glass or a wall nearby don't count as "near".
        if (ps < _prox_base) {
            _prox_base = ps;
        } else if (!_near) {
            _prox_base += (ps - _prox_base) * 0.005f;
        }
    }

    float lux = 0;
    if (now - _last_light_read >= _light_interval_ms && _light->readLux(lux)) {
        _last_light_read = now;
        _lux             = _lux < 0 ? lux : _lux + (lux - _lux) * 0.3f;
        if (_auto_brightness && !_standby_until) {
            const int target = brightness_for_lux(_lux);
            if (std::abs(target - (int)GetHAL().getBackLightBrightness()) >= 4) {
                GetHAL().setBackLightBrightness(target);  // not saved; the backlight fades
            }
        }
    }
}

/* --------------------------------- Power ---------------------------------- */

// Power key and plug changes latched by the AXP2101, polled every 100 ms. A short
// press of the power button also wakes the screen, like a touch.
void AppEmbodyMode::update_power_events()
{
    const uint32_t now = GetHAL().millis();
    if (now - _last_power_poll < 100) {
        return;
    }
    _last_power_poll = now;
    const uint8_t ev = hal_bridge::board_take_pmic_events();
    if (!ev) {
        return;
    }
    if (ev & 0x08) {
        _last_physical = now;
        queue_event("power_button", {}, {{"press", "short"}});
    }
    if (ev & 0x04) {
        queue_event("power_button", {}, {{"press", "long"}});
    }
    if (ev & 0x80) {
        queue_event("usb_plugged");
    }
    if (ev & 0x40) {
        queue_event("usb_unplugged");
    }
    if (ev & 0x20) {
        queue_event("battery_inserted");
    }
    if (ev & 0x10) {
        queue_event("battery_removed");
    }
    mclog::tagInfo(_tag, "power events 0x{:02X}", ev);
}

/* ---------------------------------- IR ------------------------------------ */

// Received IR frames become "ir_received" events: NEC address/command when it
// decodes, and always the raw timings, so the browser can replay any remote.
// NEC repeat codes (a held button) and very short bursts (noise) are skipped.
void AppEmbodyMode::update_ir()
{
    IrRemote::Frame frame;
    while (_ir && _ir->receive(frame)) {
        if (frame.repeat || (!frame.nec && frame.timings.size() < 8)) {
            continue;
        }
        embody::Client::Telemetry data;
        if (frame.nec) {
            data = {{"address", (float)frame.address}, {"command", (float)frame.command}};
        }
        queue_event("ir_received", std::move(data),
                    {{"protocol", frame.nec ? "nec" : "raw"}, {"raw", IrRemote::timingsToString(frame.timings)}});
        mclog::tagInfo(_tag, "ir: received {} ({} timings)", frame.nec ? "NEC" : "raw", frame.timings.size());
    }
}

/* ---------------------------------- LEDs ---------------------------------- */

static constexpr int _leds_per_side          = 6;
static constexpr uint32_t _led_frame_ms      = 40;  // 25 fps
static constexpr uint32_t _led_max_seconds   = 3600;

static void set_pixel(int index, uint32_t color, float level = 1.0f)
{
    level = std::clamp(level, 0.0f, 1.0f);
    GetHAL().setRgbColor(index, (uint8_t)(((color >> 16) & 0xFF) * level), (uint8_t)(((color >> 8) & 0xFF) * level),
                         (uint8_t)((color & 0xFF) * level));
}

// Hue 0..1 to a fully saturated colour at 60 % (a rainbow at full power is glaring).
static uint32_t hue_color(float hue)
{
    const float x = (hue - std::floor(hue)) * 6.0f;
    const float f = x - std::floor(x);
    float r = 0, g = 0, b = 0;
    switch ((int)x % 6) {
        case 0: r = 1, g = f; break;
        case 1: r = 1 - f, g = 1; break;
        case 2: g = 1, b = f; break;
        case 3: g = 1 - f, b = 1; break;
        case 4: r = f, b = 1; break;
        default: r = 1, b = 1 - f; break;
    }
    return ((uint32_t)(r * 153) << 16) | ((uint32_t)(g * 153) << 8) | (uint32_t)(b * 153);
}

// "leds" args, all optional, applied in this order:
//   {"left": "#rrggbb", "right": "#rrggbb"}  fade a whole side
//   {"pixels": ["#rrggbb" or null, ...]}     up to 12 single LEDs, left 0-5 then right 6-11
//   {"effect": "rainbow|breathe|chase|blink|off", "color": "#rrggbb", "speed": 0.2..5, "seconds": 0..3600}
// Sides and pixels stop a running effect. A timed effect restores the side colours when it ends.
void AppEmbodyMode::run_leds(const ArduinoJson::JsonDocument& args)
{
    auto& sc       = GetStackChan();
    uint32_t color = 0;
    if (parse_hex_color(args["left"] | "", color)) {
        stop_led_effect(false);
        _led_left = color;
        sc.leftNeonLight().setColor(color);
    }
    if (parse_hex_color(args["right"] | "", color)) {
        stop_led_effect(false);
        _led_right = color;
        sc.rightNeonLight().setColor(color);
    }

    auto pixels = args["pixels"].as<ArduinoJson::JsonArrayConst>();
    if (!pixels.isNull()) {
        stop_led_effect(false);
        for (size_t i = 0; i < pixels.size() && i < 2 * _leds_per_side; i++) {
            if (parse_hex_color(pixels[i] | "", color)) {
                set_pixel(i, color);
            }
        }
        GetHAL().refreshRgb();
    }

    const std::string effect = args["effect"] | "";
    if (effect.empty()) {
        return;
    }
    static constexpr std::pair<const char*, LedEffect> effects[] = {
        {"rainbow", LedEffect::Rainbow}, {"breathe", LedEffect::Breathe},
        {"chase", LedEffect::Chase},     {"blink", LedEffect::Blink},
    };
    auto it = std::find_if(std::begin(effects), std::end(effects), [&](const auto& e) { return effect == e.first; });
    if (it == std::end(effects)) {  // "off", or an effect this firmware doesn't know
        stop_led_effect(false);
        _led_left = _led_right = 0;
        sc.leftNeonLight().setColor((uint32_t)0);
        sc.rightNeonLight().setColor((uint32_t)0);
        return;
    }
    _led_effect = it->second;
    _led_color  = parse_hex_color(args["color"] | "", color) ? color : 0xFFFFFF;
    _led_speed  = std::clamp(args["speed"] | 1.0f, 0.2f, 5.0f);
    _led_start  = GetHAL().millis();
    const uint32_t seconds = std::min<uint32_t>(args["seconds"] | 0, _led_max_seconds);
    _led_until             = seconds ? std::max<uint32_t>(_led_start + seconds * 1000, 1) : 0;
    _led_last_frame        = _led_start - _led_frame_ms;  // draw now
    mclog::tagInfo(_tag, "leds: {} effect", effect);
}

void AppEmbodyMode::stop_led_effect(bool restore_sides)
{
    if (_led_effect == LedEffect::None) {
        return;
    }
    _led_effect = LedEffect::None;
    if (restore_sides) {  // NeonLight rewrites all six LEDs of each side
        GetStackChan().leftNeonLight().setColor(_led_left);
        GetStackChan().rightNeonLight().setColor(_led_right);
    }
}

// Draws the running effect, both sides mirrored.
void AppEmbodyMode::update_leds()
{
    if (_led_effect == LedEffect::None) {
        return;
    }
    const uint32_t now = GetHAL().millis();
    if (_led_until && (int32_t)(now - _led_until) >= 0) {
        stop_led_effect(true);
        return;
    }
    if (now - _led_last_frame < _led_frame_ms) {
        return;
    }
    _led_last_frame = now;
    const float t   = (now - _led_start) / 1000.0f * _led_speed;  // effect time, seconds at speed 1

    for (int i = 0; i < _leds_per_side; i++) {
        uint32_t color = _led_color;
        float level    = 1.0f;
        switch (_led_effect) {
            case LedEffect::Rainbow:  // one turn every 5 s, spread over the side
                color = hue_color(t / 5.0f + (float)i / _leds_per_side);
                break;
            case LedEffect::Breathe:  // 2.5 s per breath
                level = 0.5f - 0.5f * std::cos(2.0f * (float)M_PI * t / 2.5f);
                break;
            case LedEffect::Chase: {  // a dot with a fading tail, 6 LEDs per second
                float d = std::fmod(t * _leds_per_side - i, (float)_leds_per_side);
                level   = std::max(0.0f, 1.0f - (d < 0 ? d + _leds_per_side : d) / 2.5f);
                break;
            }
            case LedEffect::Blink:  // once per second
                level = std::fmod(t, 1.0f) < 0.5f ? 1.0f : 0.0f;
                break;
            default:
                break;
        }
        set_pixel(i, color, level);
        set_pixel(i + _leds_per_side, color, level);
    }
    GetHAL().refreshRgb();
}

/* ----------------------------------- NFC ---------------------------------- */

void AppEmbodyMode::start_nfc()
{
    if (!_nfc || _nfc_running) {
        return;
    }
    _nfc_running        = true;
    TaskHandle_t handle = nullptr;
    // Priority 1 on core 1: the driver busy-waits a few ms per step; this keeps it
    // below head touch (priority 2) and away from the app loop (core 0).
    if (xTaskCreatePinnedToCore(nfc_task, "embody_nfc", 4096, this, 1, &handle, 1) != pdPASS) {
        _nfc_running = false;
        mclog::tagError(_tag, "nfc: task create failed");
        return;
    }
    _nfc_task = handle;
    mclog::tagInfo(_tag, "nfc on");
}

void AppEmbodyMode::stop_nfc()
{
    if (!_nfc_running) {
        return;
    }
    _nfc_running = false;
    // The task switches the field off and clears _nfc_task when it exits.
    for (int i = 0; i < 100 && _nfc_task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    mclog::tagInfo(_tag, "nfc off");
}

static const char* nfc_tag_type(uint8_t sak)
{
    switch (sak) {
        case 0x00:
            return "type2";  // NTAG, MIFARE Ultralight
        case 0x08:
            return "mifare_classic_1k";
        case 0x09:
            return "mifare_mini";
        case 0x18:
            return "mifare_classic_4k";
        default:
            return (sak & 0x20) ? "iso14443_4" : "unknown";  // e.g. phones, bank cards, DESFire
    }
}

// Polls twice a second with the RF field on only while reading. A new tag is
// reported once, with the text of its first NDEF record for Type 2 tags; two
// missed polls in a row mean it is gone.
void AppEmbodyMode::nfc_task(void* arg)
{
    auto* self = static_cast<AppEmbodyMode*>(arg);
    auto& nfc  = *self->_nfc;
    std::string present;
    int misses = 0;

    while (self->_nfc_running) {
        ST25R3916::Tag tag;
        nfc.fieldOn();
        if (nfc.readUid(tag)) {
            misses = 0;
            std::string uid;
            for (uint8_t i = 0; i < tag.uidLen; i++) {
                char b[4];
                std::snprintf(b, sizeof(b), i ? ":%02X" : "%02X", tag.uid[i]);
                uid += b;
            }
            if (uid != present) {
                self->_last_physical   = GetHAL().millis();  // a new tag wakes the screen like a touch
                const std::string text = tag.sak == 0x00 ? nfc.readNdefText() : "";
                nfc.halt();
                present = uid;
                embody::Client::Texts info = {{"uid", uid}, {"type", nfc_tag_type(tag.sak)}};
                if (!text.empty()) {
                    info.emplace_back("text", text);
                }
                self->queue_event("nfc_tag", {{"atqa", (float)tag.atqa}, {"sak", (float)tag.sak}}, std::move(info));
                mclog::tagInfo(_tag, "nfc tag {} ({}) {}", uid, nfc_tag_type(tag.sak), text);
            }
        } else if (!present.empty() && ++misses >= 2) {
            self->queue_event("nfc_removed", {}, {{"uid", present}});
            mclog::tagInfo(_tag, "nfc tag {} removed", present);
            present.clear();
        }
        nfc.fieldOff();
        for (int i = 0; i < 50 && self->_nfc_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    self->_nfc_task = nullptr;
    vTaskDelete(nullptr);
}
