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

// Servo limits in 0.1 degree (hal_servo.cpp)
static constexpr int _yaw_min = -1280, _yaw_max = 1280;
static constexpr int _pitch_min = 30, _pitch_max = 870;
static constexpr int _motion_speed = 600;

static constexpr AppEmbodyMode::GestureStep _nod[]   = {{'p', -120}, {'p', 100}, {'p', -120}, {'p', 0}};
static constexpr AppEmbodyMode::GestureStep _shake[] = {{'y', -150}, {'y', 150}, {'y', -150}, {'y', 0}};
static constexpr uint32_t _gesture_step_ms           = 260;

// Binary message types (stackchan-server internal/wire)
static constexpr uint8_t _bin_camera_jpeg = 0x01;
static constexpr uint8_t _bin_audio_pcm   = 0x02;
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

    _client = std::make_unique<embody::Client>(embody::Client::Config{
        .serverUrl = server_url,
        .token     = CONFIG_STACKCHAN_EMBODY_TOKEN,
        .robotId   = robot_id,
        .model     = "stackchan-cores3",
        .firmware  = esp_app_get_description()->version,
        .commands  = {"ping", "nod", "shake", "look", "home", "emotion", "say", "leds", "brightness", "volume",
                      "sticker", "face", "image", "camera", "mic"},
        .measurements = {"battery_pct", "charging", "head_yaw_deg", "head_pitch_deg", "wifi_rssi_dbm",
                         "free_heap_kb", "uptime_s", "brightness_pct", "volume_pct", "screensaver"},
    });
    _client->onCommand = [this](const std::string& command, const std::string& args) {
        _pending_commands.emplace_back(command, args);
    };
    _client->onBinary = [this](uint8_t type, const std::string& payload) {
        if (type == _bin_show_jpeg) {
            _pending_picture_jpeg = payload;  // decoded in the app loop, shown under the LVGL lock
        }
    };
    _client->collectTelemetry = [this]() {
        auto& motion = GetStackChan().motion();
        return embody::Client::Telemetry{
            {"battery_pct", (float)GetHAL().getBatteryLevel()},
            {"charging", GetHAL().isBatteryCharging() ? 1.0f : 0.0f},
            {"head_yaw_deg", motion.getCurrentYawAngle() / 10.0f},
            {"head_pitch_deg", motion.getCurrentPitchAngle() / 10.0f},
            {"wifi_rssi_dbm", (float)WifiManager::GetInstance().GetRssi()},
            {"free_heap_kb", (float)(esp_get_free_heap_size() / 1024)},
            {"uptime_s", (float)(esp_timer_get_time() / 1000000)},
            {"brightness_pct", (float)GetHAL().getBackLightBrightness()},
            {"volume_pct", (float)GetHAL().getSpeakerVolume()},
            {"screensaver", _blank_screen ? 1.0f : 0.0f},
        };
    };

    _imu_connection = GetHAL().onImuMotionEvent.connect([this](ImuMotionEvent event) {
        if (event == ImuMotionEvent::Shake) {
            queue_event("shake");
        } else if (event == ImuMotionEvent::PickUp) {
            queue_event("pickup");
        }
    });
    _head_connection = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture gesture) {
        switch (gesture) {
            case HeadPetGesture::Press:
                queue_event("head_press");
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
}

void AppEmbodyMode::onRunning()
{
    // Network I/O, JPEG work and camera capture happen outside the LVGL lock.
    if (_client) {
        _client->update();

        std::vector<std::pair<std::string, embody::Client::Telemetry>> events;
        {
            std::lock_guard<std::mutex> lock(_event_mutex);
            events.swap(_pending_events);
        }
        for (const auto& [name, data] : events) {
            _client->sendEvent(name, data);
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

    update_motion();
    render();
    update_screensaver();
    GetStackChan().update();
    view::update_home_indicator();
    view::update_status_bar();
}

void AppEmbodyMode::onClose()
{
    mclog::tagInfo(_tag, "on close");

    stop_mic();
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

void AppEmbodyMode::queue_event(const char* name, embody::Client::Telemetry data)
{
    std::lock_guard<std::mutex> lock(_event_mutex);
    if (_pending_events.size() < 16) {
        _pending_events.emplace_back(name, std::move(data));
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
    if (command == "emotion" || command == "say" || command == "sticker" || command == "face") {
        wake_screen();  // the change should be visible, not hidden behind the screensaver
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
    } else if (command == "camera") {
        _camera_on = args["on"] | false;
        // StackChanCamera logs a warning for every captured frame; keep the log readable.
        esp_log_level_set("StackChanCamera", _camera_on ? ESP_LOG_ERROR : ESP_LOG_INFO);
        mclog::tagInfo(_tag, "camera {}", _camera_on ? "on" : "off");
    } else if (command == "mic") {
        (args["on"] | false) ? start_mic() : stop_mic();
    } else if (command == "leds") {
        uint32_t color = 0;
        if (parse_hex_color(args["left"] | "", color)) {
            sc.leftNeonLight().setColor(color);
        }
        if (parse_hex_color(args["right"] | "", color)) {
            sc.rightNeonLight().setColor(color);
        }
    } else if (command == "brightness") {
        GetHAL().setBackLightBrightness(std::clamp(args["value"] | 60, 1, 100));
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

// After CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S without touch, load a blank
// (black) screen; any touch, or wake_screen(), loads the Embody screen back.
// The top layer (LIVE badge) stays visible. Runs under the LVGL lock.
void AppEmbodyMode::update_screensaver()
{
#if CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S > 0
    if (!_panel) {
        return;
    }
    const uint32_t timeout_ms = CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S * 1000u;
    if (lv_display_get_inactive_time(nullptr) >= timeout_ms) {
        if (!_blank_screen) {
            _prev_screen  = lv_screen_active();
            _blank_screen = lv_obj_create(nullptr);
            lv_obj_set_style_bg_color(_blank_screen, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(_blank_screen, LV_OPA_COVER, 0);
            lv_obj_remove_flag(_blank_screen, LV_OBJ_FLAG_SCROLLABLE);
            lv_screen_load(_blank_screen);
            queue_event("screensaver_on");
            mclog::tagInfo(_tag, "screensaver on");
        }
    } else if (_blank_screen) {
        lv_screen_load(_prev_screen);
        lv_obj_delete(_blank_screen);
        _blank_screen = nullptr;
        queue_event("screensaver_off");
        mclog::tagInfo(_tag, "screensaver off");
    }
#endif
}

// Counts as user activity, so the screensaver closes on the next update.
void AppEmbodyMode::wake_screen()
{
    lv_display_trigger_activity(nullptr);
}
