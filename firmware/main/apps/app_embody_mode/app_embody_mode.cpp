/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
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
#include <settings.h>
#include <mooncake.h>
#include "automation.h"
#include "usb_setup.h"
#include <audio_codec.h>
#include <lvgl_image.h>
#include <jpg/image_to_jpeg.h>
#include <ArduinoJson.hpp>
#include <driver/usb_serial_jtag.h>
#include <esp_app_desc.h>
#include <esp_wifi.h>
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

// "wss://chan.w42.eu/x" -> "chan.w42.eu": the default name of a server entry
static std::string host_of(const std::string& url)
{
    auto start = url.find("://");
    start      = start == std::string::npos ? 0 : start + 3;
    return url.substr(start, url.find_first_of(":/", start) - start);
}

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
static constexpr uint8_t _bin_audio_multi = 0x04;  // rate, channel count, interleaved PCM
static constexpr uint8_t _bin_imu         = 0x05;  // count, then (uint32 ms, 9 x float32) per sample
static constexpr uint8_t _bin_snapshot    = 0x07;  // full-resolution JPEG still
static constexpr uint8_t _bin_touch       = 0x06;
static constexpr uint8_t _bin_light       = 0x08;  // count, then (uint32 ms, uint16 ps, uint16 ch0, uint16 ch1) per sample  // frames: (uint32 ms, uint8 n, n x (uint8 id, uint16 x, uint16 y))
static constexpr uint8_t _bin_speaker_pcm = 0x03;
static constexpr uint8_t _bin_show_jpeg   = 0x10;
static constexpr uint8_t _bin_asset_chunk = 0x11;  // server -> robot: an upload chunk (see AssetStore::put)

static constexpr uint32_t _camera_interval_ms = 200;  // up to 5 fps
static constexpr int _camera_jpeg_quality     = 25;
static constexpr size_t _mic_max_frames       = 24000;  // ~1 s at 24 kHz; older audio is dropped

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

    // The server list first: the release build has no built-in server, so the robot is set
    // up when its settings hold one (written over USB, offered or added).
    load_servers();
    const bool set_up = _servers.size() > 1 || !_servers[0].url.empty();
    if (set_up) {
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

    if (!set_up) {  // nothing to contact: no Wi-Fi, no hotspot, only how to set it up
        mclog::tagInfo(_tag, "not set up: connect over USB at chan.w42.eu/setup");
        LvglLockGuard lock;
        _status->setText("Not set up yet: plug me into a computer and open chan.w42.eu/setup in Chrome");
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
    commands.push_back("hold");
    commands.push_back("imu_stream");
    commands.push_back("touch_stream");
    commands.push_back("light_stream");
    commands.push_back("assets");
    commands.push_back("screen_snapshot");
    commands.push_back("play");
    commands.push_back("play_stop");
    commands.push_back("speaker_flush");
    commands.push_back("sprite");
    commands.push_back("sprite_hide");
    commands.push_back("sprite_clear");
    commands.push_back("picture");
    commands.push_back("asset_delete");
    commands.push_back("servo_power");
    commands.push_back("rotate");
    for (const char* c : {"server_add", "server_remove", "server_default", "server_switch", "server_e2e", "e2e_forget"}) {
        commands.push_back(c);
    }
    if (hal_bridge::board_get_camera()) {
        commands.push_back("snapshot");
        commands.push_back("camera_config");
        commands.push_back("camera_reg");
    }
    setup_car_commands(commands);
#if CONFIG_STACKCHAN_EMBODY_AUTOMATION
    for (const char* c : {"automation", "restart", "launch"}) {
        commands.push_back(c);
    }
#endif

    _robot_id = robot_id;
    _commands = commands;
    _assets.mount();
    embody::E2E::selfTest();  // the reference vectors, logged
    _e2e_ok = _e2e.begin(_robot_id);
    load_e2e_urls();
    load_servers();
    if (!_default_url.empty()) {
        connect_server(_server_index);
    }  // else: the QR screen is a chooser (Next, Connect); nothing is contacted until then

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
    // Server switching: from the QR screen buttons or the server_switch command
    if (const int nav = _nav_request.exchange(0); nav && !_servers.empty()) {
        if (nav == 2) {  // Pin: make the shown server the default, or clear it if it is already
            const auto& url = _servers[_shown_index].url;
            _default_url    = _default_url == url ? "" : url;
            save_servers();
            announce_servers();
            _servers_rev++;
        } else if (nav == 3) {  // the bottom-right button: Close on the current server, else Connect
            if (_client && _shown_index == _server_index) {
                _qr_hide_requested = true;
            } else {
                _pending_switch = (int)_shown_index;
                _qr_pinned      = true;  // chosen on the QR screen: stay there for its code
            }
        } else if (_servers.size() > 1) {  // Next: only browse, the connection stays
            _shown_index = (_shown_index + 1) % _servers.size();
            _servers_rev++;
        }
    }
    if (_pending_switch >= 0) {
        const size_t target = (size_t)_pending_switch;
        _pending_switch     = -1;
        connect_server(target);
        _servers_rev++;
    }

    // Network I/O, JPEG work and camera capture happen outside the LVGL lock.
    if (_client) {
        _client->update();
        if (_client->isRegistered() && !_servers_announced) {
            _servers_announced = true;
            announce_servers();
#if CONFIG_STACKCHAN_EMBODY_AUTOMATION
            queue_event("automation", {{"autostart", embody::autostart() ? 1.0 : 0.0}});
#endif
        }

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
        send_imu_stream();
    }

    if (_snapshot_requested) {
        _snapshot_requested = false;
        take_snapshot();
    }
    update_leds();
    update_sound();
    update_light();
    update_ir();
    update_power_events();
    update_car();

    prepare_pictures();  // decode stored pictures before taking the LVGL lock
    LvglLockGuard lock;
    for (const auto& [command, args] : _pending_commands) {
        run_command(command, args);
    }
    _pending_commands.clear();

    if (_pending_picture) {
        _picture = std::move(_pending_picture);  // keep the pixels alive while shown
        lv_image_set_src(_picture_obj, _picture->image_dsc());
        lv_obj_remove_flag(_picture_obj, LV_OBJ_FLAG_HIDDEN);
        _picture_asset = "sent";
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

    if (const std::string& pair = _client ? _client->pairUrl() : std::string(); pair != _published_pair) {
        _published_pair = pair;
        embody::setPairUrl(pair);
    }
    // Wi-Fi modem sleep makes every packet to the robot wait for the next beacon (up to about
    // 100 ms): off for 2 minutes after the last command and while the camera or microphone
    // streams, on otherwise (it saves battery). The first command after a quiet spell is slow.
    if (_client && _client->commandCount() != _ps_seen_commands) {
        _ps_seen_commands   = _client->commandCount();
        _last_command_ms = GetHAL().millis();
    }
    if (const bool want = _client && (_camera_on || _mic_running ||
                                      (_ps_seen_commands > 0 && GetHAL().millis() - _last_command_ms < 120000));
        want != _wifi_low_latency) {
        _wifi_low_latency = want;
        const esp_err_t err = esp_wifi_set_ps(want ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
        mclog::tagInfo(_tag, "wifi power save {}: {}", want ? "off (in use)" : "on (idle)", esp_err_to_name(err));
    }
    if (!_boot_stable && GetHAL().millis() > 60000) {
        _boot_stable = true;
        embody::mark_stable();
    }
    render_server_row();
    update_motion();
    update_hold();
    if (_rotate_until && (int32_t)(GetHAL().millis() - _rotate_until) >= 0) {
        stop_rotate();
    }
    check_rotate_safety();
    update_touch();
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
    embody::setPairUrl("");
    if (_wifi_low_latency) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        _wifi_low_latency = false;
    }

    stop_mic();
    stop_sound(false);
    stop_speaker();
    stop_nfc();
    stop_rotate();
    stop_hold();
    if (_car) {
        _car->stop();  // stops the motors first
    }
    _touch_streaming = false;
    _imu_streaming = false;
    GetHAL().setImuStreaming(false);
    set_light_stream(false);
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
        if (_touch_indev) {
            lv_indev_remove_event_cb_with_user_data(_touch_indev, on_face_input, this);
            _touch_indev = nullptr;
        }
        _face_obj = nullptr;
        GetStackChan().clearModifiers();
        GetStackChan().resetAvatar();
        if (_live_badge) {
            lv_obj_delete(_live_badge);
            _live_badge = nullptr;
        }
        _sprite_layer.destroy();
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
        _server_pos.reset();
        _qr_box.reset();
        _title.reset();
        _panel.reset();
        view::destroy_home_indicator();
        view::destroy_status_bar();
    }
    _rendered_revision = UINT32_MAX;
    _rendered_url.clear();
    _qr_visible = true;
    _rendered_servers_rev = UINT32_MAX;  // the row's buttons went with the panel
    for (auto& b : _server_buttons) {
        b = nullptr;
    }

    // Wi-Fi can't be torn down cleanly; reboot back to the launcher like AVATAR does.
    if (_network_started) {
        GetHAL().requestWarmReboot(_launcher_index);
    }
}

/* ---------------------------------- View ---------------------------------- */

// Tap -> "screen_tap" event with coordinates; long press -> "screen_long_press" (free for apps).
// Runs in the LVGL task, so it only queues.
void AppEmbodyMode::on_screen_event(lv_event_t* e)
{
    static_cast<AppEmbodyMode*>(lv_event_get_user_data(e))->screen_input(lv_event_get_code(e));
}

// Taps on the face come from the touch input device itself: the face's parts (eyes,
// mouth, speech bubble, stickers) are LVGL objects that would take a tap on them, and
// the face would never hear it (e.g. a menu tile drawn over an eye).
void AppEmbodyMode::on_face_input(lv_event_t* e)
{
    auto* self = static_cast<AppEmbodyMode*>(lv_event_get_user_data(e));
    auto code  = lv_event_get_code(e);
    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_LONG_PRESSED) {
        return;
    }
    bool onFace = false;  // not a picture or the QR panel: they listen themselves
    for (lv_obj_t* o = lv_indev_get_active_obj(); o && self->_face_obj; o = lv_obj_get_parent(o)) {
        if (o == self->_face_obj) {
            onFace = true;
            break;
        }
    }
    if (!onFace) {
        return;
    }
    self->screen_input(code);
    auto* indev = lv_indev_active();
    if (code == LV_EVENT_SHORT_CLICKED && indev && lv_indev_get_short_click_streak(indev) % 3 == 2) {
        self->screen_input(LV_EVENT_DOUBLE_CLICKED);  // the input device gets no double click
    }
}

void AppEmbodyMode::screen_input(lv_event_code_t code)
{
    lv_point_t p{};
    if (auto* indev = lv_indev_active()) {
        lv_indev_get_point(indev, &p);
    }
    if (code == LV_EVENT_LONG_PRESSED) {  // free for apps (the QR screen has its own button)
        queue_tap("screen_long_press", p.x, p.y);
    } else if (code == LV_EVENT_DOUBLE_CLICKED) {
        embody::SpriteLayer::Hit hit;
        if (!_sprite_layer.hit(p.x, p.y, hit)) {  // on a button it is two presses (a game, a menu)
            _blank_requested = true;
        }
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        queue_tap("screen_tap", p.x, p.y);
    }
}

// queue_tap reports a tap (or long press) with what it hit: the topmost tappable sprite
// ("sprite", its "asset" and the point in that picture, "sprite_x"/"sprite_y") and the
// full-screen picture shown ("picture"). A server draws menus from sprites and reads
// the choice from here. Runs in the LVGL task.
void AppEmbodyMode::queue_tap(const char* name, int x, int y)
{
    embody::Client::Telemetry data = {{"x", (double)x}, {"y", (double)y}};
    embody::Client::Texts text;
    embody::SpriteLayer::Hit hit;
    if (_sprite_layer.hit(x, y, hit)) {
        data.emplace_back("sprite_x", (double)hit.x);
        data.emplace_back("sprite_y", (double)hit.y);
        text.emplace_back("sprite", hit.id);
        text.emplace_back("asset", hit.asset);
    }
    if (!_picture_asset.empty()) {
        text.emplace_back("picture", _picture_asset);
    }
    queue_event(name, data, text);
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
    _face_obj = avatar->getPanel()->get();
    lv_obj_add_flag(_face_obj, LV_OBJ_FLAG_CLICKABLE);
    for (lv_indev_t* i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i)) {
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) {  // the touch screen
            _touch_indev = i;
            lv_indev_add_event_cb(i, on_face_input, LV_EVENT_ALL, this);
            break;
        }
    }
    GetStackChan().attachAvatar(std::move(avatar));

    _picture_obj = lv_image_create(lv_screen_active());
    lv_obj_set_size(_picture_obj, 320, 240);
    lv_obj_align(_picture_obj, LV_ALIGN_CENTER, 0, 0);
    _sprite_layer.create(lv_screen_active());  // above the face and pictures, below the QR panel
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

    // Top row: [Pin = default] server name (n/m) [Next >]; "Back to app" / "Connect" at the bottom right.
    // Big buttons: small icons are hard to hit on this screen.
    _title = std::make_unique<Label>(*_panel);
    _title->setText("Embody Mode");
    _title->setTextFont(&lv_font_montserrat_20);
    _title->setTextColor(lv_color_hex(_color_text));
    _title->setSize(160, 24);  // fixed height: one line, long names end in "..."
    _title->setLongMode(LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(_title->get(), LV_TEXT_ALIGN_CENTER, 0);
    _title->align(LV_ALIGN_TOP_LEFT, 62, 4);
    _server_pos = std::make_unique<Label>(*_panel);
    _server_pos->setText("");
    _server_pos->setTextFont(&lv_font_montserrat_16);
    _server_pos->setTextColor(lv_color_hex(_color_muted));
    _server_pos->setWidth(160);
    lv_obj_set_style_text_align(_server_pos->get(), LV_TEXT_ALIGN_CENTER, 0);
    _server_pos->align(LV_ALIGN_TOP_LEFT, 62, 28);
    const char* labels[3] = {"Pin", "Next " LV_SYMBOL_RIGHT, "Back to app"};
    const int xs[3]        = {6, 226, 182};
    const int ys[3]        = {4, 4, 172};  // close (or connect): bottom right, above the home swipe zone
    const int ws[3]        = {50, 88, 132};
    const int actions[3]   = {2, 1, 3};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* b = lv_button_create(_panel->get());
        lv_obj_set_size(b, ws[i], 44);
        lv_obj_set_pos(b, xs[i], ys[i]);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xE8EBFF), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_user_data(b, (void*)(intptr_t)actions[i]);
        lv_obj_add_event_cb(b, on_server_nav, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, labels[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(_color_text), 0);
        lv_obj_center(l);
        _server_buttons[i] = b;
    }

    // Left: QR code on a white card (y 58..214, below the server row, above the home swipe zone)
    _qr_box = std::make_unique<Container>(*_panel);
    _qr_box->setSize(156, 156);
    _qr_box->align(LV_ALIGN_TOP_LEFT, 14, 58);
    _qr_box->setBgColor(lv_color_hex(0xFFFFFF));
    _qr_box->setBorderWidth(0);
    _qr_box->setRadius(12);
    _qr_box->setPadding(0, 0, 0, 0);
    _qr_box->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _qr_box->removeFlag(LV_OBJ_FLAG_CLICKABLE);

    _qr = lv_qrcode_create(_qr_box->get());
    // Quiet zone on: LVGL then picks the QR version (up to two above the minimum) that
    // fills the canvas best, so short and long server URLs both come out as version 4
    // at 4 px per module instead of 116 px vs 132 px.
    lv_qrcode_set_size(_qr, 148);
    lv_qrcode_set_quiet_zone(_qr, true);
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
    _status->align(LV_ALIGN_TOP_LEFT, 182, 60);
    _status->setText("Connecting to server...");

    _code = std::make_unique<Label>(*_panel);
    _code->setTextFont(&lv_font_montserrat_20);
    _code->setTextColor(lv_color_hex(_color_text));
    _code->align(LV_ALIGN_TOP_LEFT, 182, 104);
    _code->setText("");

    _detail = std::make_unique<Label>(*_panel);
    _detail->setTextFont(&lv_font_montserrat_16);
    _detail->setTextColor(lv_color_hex(_color_muted));
    _detail->setWidth(128);
    _detail->setLongMode(LV_LABEL_LONG_MODE_WRAP);
    _detail->align(LV_ALIGN_TOP_LEFT, 182, 132);
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
    // Second button in the swipe-up bar: show or hide the pairing QR screen
    view::set_home_indicator_extra_button("QR", [this]() { _toggle_qr_requested = true; });
    view::create_status_bar(_color_theme, _color_text);
}

void AppEmbodyMode::render()
{
    if (!_panel) {
        return;
    }

    bool changed = false;
    if (_client) {
        // Browsers paired: show the face. Not for a mere "paired before" on reconnect while
        // the QR screen is pinned (browsing servers there); a new scan always shows the face.
        if (_client->viewers() > _rendered_viewers && !(_qr_pinned && _client->pairedOnReconnect())) {
            _qr_visible = false;
            _qr_pinned  = false;
            changed     = true;
            wake_screen();
        }
        _rendered_viewers = _client->viewers();
    }
    if (_toggle_qr_requested.exchange(false) && (!_client || _client->isRegistered())) {
        _qr_visible = !_qr_visible;
        changed     = true;
    }
    if (_qr_hide_requested.exchange(false) && _qr_visible) {
        _qr_visible = false;
        _qr_pinned  = false;
        changed     = true;
    }
    if (changed) {
        _panel->setHidden(!_qr_visible);
    }
    if (_qr_visible != _rendered_qr_visible) {  // the swipe-up bar's button goes where you are not
        _rendered_qr_visible = _qr_visible;
        view::set_home_indicator_extra_text(_qr_visible ? "APP" : "QR");
    }

    // Browsing: the shown server is not the connected one (or nothing is connected yet,
    // with no default server). Its code comes only after Connect.
    if (!_client || _shown_index != _server_index) {
        if (_rendered_revision != UINT32_MAX - 1) {
            _rendered_revision = UINT32_MAX - 1;
            _rendered_url.clear();
            _status->setText(_client ? "On " + _servers[_server_index].name : "No server pinned");
            lv_obj_add_flag(_qr, LV_OBJ_FLAG_HIDDEN);
            _qr_hint->setText("Not connected");
            _qr_hint->setHidden(false);
            _code->setText("");
            _detail->setText(_client ? "Connect to switch" : "Next to choose, Connect to start");
        }
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
            _qr_hint->setText("No code yet");
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

// Sensor values for the 2 s telemetry. Runs in the app loop, which also drives
// the servos, so reading the servo bus here is safe.
void AppEmbodyMode::add_sensor_telemetry(embody::Client::Telemetry& t)
{
    car_telemetry(t);
    auto round_to = [](float v, float step) { return std::round(v / step) * step; };

    ImuSample_t imu;
    if (GetHAL().getImuSample(imu)) {
        constexpr float g = 9.80665f;  // m/s^2 per g
        t.emplace_back("imu_ax_g", round_to(imu.accel[0] / g, 0.01f));
        t.emplace_back("imu_ay_g", round_to(imu.accel[1] / g, 0.01f));
        t.emplace_back("imu_az_g", round_to(imu.accel[2] / g, 0.01f));
        t.emplace_back("imu_gyro_dps", round_to(std::hypot(imu.gyro[0], imu.gyro[1], imu.gyro[2]), 0.1f));
        if (imu.mag_valid) {  // BMM150: compensated uT, and the raw counts
            t.emplace_back("mag_x_ut", round_to(imu.mag[0], 0.1f));
            t.emplace_back("mag_y_ut", round_to(imu.mag[1], 0.1f));
            t.emplace_back("mag_z_ut", round_to(imu.mag[2], 0.1f));
            t.emplace_back("mag_raw_x", (float)imu.mag_raw[0]);
            t.emplace_back("mag_raw_y", (float)imu.mag_raw[1]);
            t.emplace_back("mag_raw_z", (float)imu.mag_raw[2]);
            t.emplace_back("mag_rhall", (float)imu.mag_rhall);
        }
    }

    ServoStatus_t servo;
    if (GetHAL().readServoStatus(1, servo)) {
        t.emplace_back("yaw_load_pct", servo.load / 10.0f);
        t.emplace_back("yaw_temp_c", (float)servo.temperature);
        t.emplace_back("servo_voltage_v", servo.voltage);
        t.emplace_back("yaw_pos_raw", (float)servo.position);
        t.emplace_back("yaw_speed_raw", (float)servo.speed);
        t.emplace_back("yaw_current_raw", (float)servo.current);
        t.emplace_back("yaw_moving", servo.moving ? 1.0f : 0.0f);
    }
    if (GetHAL().readServoStatus(2, servo)) {
        t.emplace_back("pitch_load_pct", servo.load / 10.0f);
        t.emplace_back("pitch_temp_c", (float)servo.temperature);
        t.emplace_back("pitch_voltage_v", servo.voltage);
        t.emplace_back("pitch_pos_raw", (float)servo.position);
        t.emplace_back("pitch_speed_raw", (float)servo.speed);
        t.emplace_back("pitch_current_raw", (float)servo.current);
        t.emplace_back("pitch_moving", servo.moving ? 1.0f : 0.0f);
    }
    // Head touch: current intensity per zone (0-3)
    const auto zones = GetHAL().getHeadTouchZones();
    t.emplace_back("head_zone0", (float)zones[0]);
    t.emplace_back("head_zone1", (float)zones[1]);
    t.emplace_back("head_zone2", (float)zones[2]);

    int64_t rtc = 0;
    if (GetHAL().getRtcUnix(rtc)) {
        t.emplace_back("rtc_unix", (double)rtc);
    }
    if (_light) {
        t.emplace_back("light_ch0", (double)_light_ch0);  // raw: visible + IR
        t.emplace_back("light_ch1", (double)_light_ch1);  // raw: IR
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
        t.emplace_back("pmic_ts_raw", (double)pmic.ts_raw);  // TS pin ADC (battery thermistor input)
    }
    // A computer on the CoreS3's USB-C (it sends USB frames; a charger or power bank does not)
    t.emplace_back("usb_data", usb_serial_jtag_is_connected() ? 1.0f : 0.0f);
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

    if (car_command(command, args) || automation_command(command, args)) {
        return;
    }

    auto& sc     = GetStackChan();
    auto& motion = sc.motion();

    if (_rotate_until && (command == "nod" || command == "shake" || command == "look" || command == "home" ||
                          command == "hold")) {
        stop_rotate();  // any positional head command ends a rotation
    }
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
        _picture_asset.clear();
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
    } else if (command == "server_add") {
        // {"url": "ws://...|wss://...", "name", "token"}: add or update an entry (not the built-in one)
        const std::string url = args["url"] | "";
        if (url.rfind("ws://", 0) == 0 || url.rfind("wss://", 0) == 0) {
            const std::string name = args["name"] | host_of(url).c_str();
            const int i            = find_server(url);
            if (i < 0) {
                _servers.push_back({name, url, args["token"] | "", "added"});
            } else if (i > 0) {
                _servers[i].name = name;
                if (args["token"].is<const char*>()) {
                    _servers[i].token = args["token"].as<const char*>();
                }
            }
            save_servers();
        }
        announce_servers();
    } else if (command == "server_remove") {
        // {"server": url or name}; not the built-in or the current one
        const int i = find_server(args["server"] | "");
        if (i > 0 && (size_t)i != _server_index) {
            if (_servers[i].url == _default_url) {
                _default_url = _servers[0].url;
            }
            _servers.erase(_servers.begin() + i);
            if ((size_t)i < _server_index) {
                _server_index--;
            }
            _shown_index = _server_index;
            _servers_rev++;
            save_servers();
        }
        announce_servers();
    } else if (command == "server_default") {
        // {"server": url or name}; "" clears it (the robot then asks at start)
        const std::string key = args["server"] | "";
        const int i           = find_server(key);
        if (i >= 0 || key.empty()) {
            _default_url = i >= 0 ? _servers[i].url : "";
            save_servers();
        }
        announce_servers();
    } else if (command == "server_switch") {
        // Leaves this server: the robot reconnects to the other one right away
        const int i = find_server(args["server"] | "");
        if (i >= 0 && (size_t)i != _server_index) {
            _pending_switch = i;
        }
    } else if (command == "snapshot") {
        _snapshot_requested = true;  // taken in the app loop, outside the LVGL lock
    } else if (command == "camera_config") {
        // {"mirror": bool, "flip": bool}: sensor mirror / flip (both default off)
        if (auto camera = hal_bridge::board_get_camera()) {
            if (args["mirror"].is<bool>()) {
                camera->SetHMirror(args["mirror"].as<bool>());
            }
            if (args["flip"].is<bool>()) {
                camera->SetVFlip(args["flip"].as<bool>());
            }
        }
    } else if (command == "camera_reg") {
        // {"reg": n} reads, {"reg": n, "value": v} writes then reads back a raw GC0308 register
        auto camera   = hal_bridge::board_get_camera();
        const int reg = args["reg"] | -1;
        uint8_t value = 0;
        bool ok       = camera && reg >= 0 && reg <= 0xFFFF;
        if (ok && args["value"].is<int>()) {
            ok = camera->WriteSensorRegister(reg, (uint8_t)(args["value"].as<int>() & 0xFF));
        }
        ok = ok && camera->ReadSensorRegister(reg, value);
        queue_event("camera_reg", {{"reg", (double)reg}, {"value", ok ? (double)value : -1.0}});
    } else if (command == "touch_stream") {
        _touch_streaming = args["on"] | false;
        _touch_frames.clear();
        _touch_frame_count = 0;
    } else if (command == "servo_power") {
        // Off: both servos lose power (the head is limp); on: powered again, torque off until the next move
        stop_rotate();
        stop_hold();
        _servo_power = args["on"] | true;
        GetHAL().setServoPowerEnabled(_servo_power);
        queue_event(_servo_power ? "servo_power_on" : "servo_power_off");
    } else if (command == "rotate") {
        // {"velocity": -1000..1000, "seconds": 1..30, "no_head_cable": true, "usb_power_ok": true};
        // velocity 0 stops
        const int velocity = args["velocity"] | 0;
        velocity ? start_rotate(velocity, args["seconds"] | 5, args["no_head_cable"] | false, args["usb_power_ok"] | false)
                 : stop_rotate();
    } else if (command == "assets") {
        send_asset_list();
    } else if (command == "screen_snapshot") {
        // What the screen shows right now (face, pictures, sprites), as a JPEG snapshot (binary 0x07).
        uint8_t* jpeg = nullptr;
        size_t len    = 0;
        lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
        if (snap && image_to_jpeg((uint8_t*)snap->data, snap->data_size, snap->header.w, snap->header.h,
                                  V4L2_PIX_FMT_RGB565, 70, &jpeg, &len) && jpeg) {
            if (len <= 65535 && _client) {
                _client->sendBinary(_bin_snapshot, jpeg, len);
            }
            free(jpeg);
        } else {
            queue_event("screen_snapshot_failed");
        }
        if (snap) {
            lv_draw_buf_destroy(snap);
        }
    } else if (command == "sprite") {
        // {"id", "asset", "x", "y" (center), "scale", "angle", "opacity", "z", "hidden", "ms" (move time)}
        const std::string err = _sprite_layer.set(args);
        if (!err.empty()) {
            const std::string asset = args["asset"] | "";
            const auto why          = _asset_load_errors.find(asset);
            queue_event("sprite_error", {}, {{"id", args["id"] | ""}, {"reason", why != _asset_load_errors.end() ? err + ": " + why->second : err}});
        }
    } else if (command == "play") {
        // {"asset": "snd/hello.wav", "volume": 0..100 (of the speaker volume, default 100)}
        start_sound(args["asset"] | "", std::clamp(args["volume"] | 100.0f, 0.0f, 100.0f) / 100.0f);
    } else if (command == "play_stop") {
        stop_sound(false);
    } else if (command == "sprite_hide") {
        _sprite_layer.hide(args["id"] | "");
    } else if (command == "sprite_clear") {
        _sprite_layer.clear();
    } else if (command == "picture") {
        // {"asset": "pet/dream.jpg"}: a stored picture instead of the face ("face" ends it)
        const std::string asset = args["asset"] | "";
        if (auto image = _sprite_layer.cached(asset)) {
            _picture       = image;
            _picture_asset = asset;
            lv_image_set_src(_picture_obj, _picture->image_dsc());
            lv_obj_remove_flag(_picture_obj, LV_OBJ_FLAG_HIDDEN);
            wake_screen();
        } else {
            const auto why = _asset_load_errors.find(asset);
            queue_event("sprite_error", {}, {{"id", "picture"}, {"reason", "cannot show " + asset + (why != _asset_load_errors.end() ? ": " + why->second : "")}});
        }
    } else if (command == "asset_delete") {
        // {"name": "food/cake.png"}
        const std::string name = args["name"] | "";
        _sprite_layer.forget(name);
        if (_assets.remove(name)) {
            queue_event("asset_deleted", {}, {{"name", name}});
        } else {
            queue_event("asset_error", {}, {{"name", name}, {"reason", "no such file"}});
        }
    } else if (command == "light_stream") {
        set_light_stream(args["on"] | false);
    } else if (command == "imu_stream") {
        _imu_streaming = args["on"] | false;
        GetHAL().setImuStreaming(_imu_streaming);
        mclog::tagInfo(_tag, "imu stream {}", _imu_streaming ? "on" : "off");
    } else if (command == "hold") {
        const int seconds = args["seconds"] | 0;  // 30..300; 0 = release now
        seconds > 0 ? start_hold(seconds) : stop_hold();
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

// Full resolution still: switch the sensor to 640x480, let it settle, grab and encode one
// frame (quality stepped down until it fits one WebSocket message), send it as binary 0x07,
// and switch back to the 320x240 video mode.
void AppEmbodyMode::take_snapshot()
{
    auto camera = hal_bridge::board_get_camera();
    if (!camera || !_client) {
        return;
    }
    const int w0 = camera->GetFrameWidth(), h0 = camera->GetFrameHeight();
    if (!camera->SetSensorSize(640, 480)) {
        queue_event("snapshot_failed", {}, {{"reason", "the sensor did not switch to 640x480"}});
        camera->SetSensorSize(w0, h0);
        return;
    }
    bool ok = false;
    for (int i = 0; i < 5; i++) {  // the first frames after a mode switch can be dark or torn (seen once)
        ok = camera->StreamCaptures();
    }
    size_t sent = 0;
    if (ok) {
        for (int quality : {60, 45, 30, 20}) {
            uint8_t* jpeg = nullptr;
            size_t len    = 0;
            if (!image_to_jpeg((uint8_t*)camera->GetFrameData(), camera->GetFrameSize(), camera->GetFrameWidth(),
                               camera->GetFrameHeight(), (v4l2_pix_fmt_t)camera->GetFrameFormat(), quality, &jpeg,
                               &len) ||
                !jpeg) {
                break;
            }
            if (len + 1 <= 65535) {
                sent = _client->sendBinary(_bin_snapshot, jpeg, len) ? len : 0;
                free(jpeg);
                break;
            }
            free(jpeg);
        }
    }
    const int w = camera->GetFrameWidth(), h = camera->GetFrameHeight();
    camera->SetSensorSize(w0, h0);
    if (!sent) {
        queue_event("snapshot_failed", {}, {{"reason", ok ? "encoding failed" : "no frame"}});
    }
    mclog::tagInfo(_tag, "snapshot {}x{}: {} bytes", w, h, sent);
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
    _mic_rate     = codec->input_sample_rate();
    _mic_channels = std::max(codec->input_channels(), 1);
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
    mclog::tagInfo(_tag, "mic on, {} Hz, {} channels", _mic_rate, _mic_channels);
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
    auto* self   = static_cast<AppEmbodyMode*>(arg);
    auto codec   = Board::GetInstance().GetAudioCodec();
    const int ch = self->_mic_channels;
    // 20 ms per read, all channels interleaved as the codec delivers them
    std::vector<int16_t> chunk((self->_mic_rate / 50) * ch);
    const size_t max_samples = _mic_max_frames * ch;

    codec->EnableInput(true);
    while (self->_mic_running) {
        if (!codec->InputData(chunk)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        std::lock_guard<std::mutex> lock(self->_mic_mutex);
        self->_mic_samples.insert(self->_mic_samples.end(), chunk.begin(), chunk.end());
        if (self->_mic_samples.size() > max_samples) {
            self->_mic_samples.erase(self->_mic_samples.begin(), self->_mic_samples.end() - max_samples);
        }
    }
    codec->EnableInput(false);
    self->_mic_task = nullptr;
    vTaskDelete(nullptr);
}

// Sends buffered microphone audio as binary 0x04: sample rate (uint16 LE), channel
// count (uint8), then interleaved s16le PCM of all codec channels, in 50 ms messages.
void AppEmbodyMode::send_mic_audio()
{
    if (!_mic_running) {
        return;
    }
    const int ch = _mic_channels;
    std::vector<int16_t> samples;
    {
        std::lock_guard<std::mutex> lock(_mic_mutex);
        if (_mic_samples.size() < (size_t)(_mic_rate / 50) * ch) {
            return;  // wait for at least 20 ms
        }
        samples.swap(_mic_samples);
    }
    const size_t per_message = (size_t)(_mic_rate / 20) * ch;  // 50 ms
    for (size_t off = 0; off < samples.size(); off += per_message) {
        size_t n = std::min(per_message, samples.size() - off);
        n -= n % ch;  // whole frames only
        std::string msg;
        msg.push_back((char)(_mic_rate & 0xFF));
        msg.push_back((char)(_mic_rate >> 8));
        msg.push_back((char)ch);
        msg.append((const char*)(samples.data() + off), n * sizeof(int16_t));
        _client->sendBinary(_bin_audio_multi, (const uint8_t*)msg.data(), msg.size());
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

// Hold: the target becomes where the head is now (enabling torque would otherwise
// snap back to the last commanded angle), and torque stays on until _hold_until.
void AppEmbodyMode::start_hold(int seconds)
{
    seconds    = std::clamp(seconds, 30, 300);
    auto& m    = GetStackChan().motion();
    pause_angle_sync();
    m.setAutoTorqueReleaseEnabled(false);
    m.moveYawWithSpeed(std::clamp(m.getCurrentYawAngle(), _yaw_min, _yaw_max), _motion_speed);
    m.movePitchWithSpeed(std::clamp(m.getCurrentPitchAngle(), _pitch_min, _pitch_max), _motion_speed);
    const bool was_holding = _hold_until != 0;
    _hold_until            = std::max<uint32_t>(GetHAL().millis() + seconds * 1000u, 1);
    if (!was_holding) {
        queue_event("hold_on", {{"seconds", (float)seconds}});
    }
    mclog::tagInfo(_tag, "hold for {} s", seconds);
}

void AppEmbodyMode::stop_hold()
{
    if (!_hold_until) {
        return;
    }
    _hold_until = 0;
    GetStackChan().motion().setAutoTorqueReleaseEnabled(true);  // released at rest, ~0.2 s later
    queue_event("hold_off");
    mclog::tagInfo(_tag, "hold off");
}

// Ends a hold when its time is up.
void AppEmbodyMode::update_hold()
{
    if (_hold_until && (int32_t)(GetHAL().millis() - _hold_until) >= 0) {
        stop_hold();
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
    stop_sound(false);
    stop_speaker();
    stop_nfc();
    stop_rotate();
    stop_hold();
    _touch_streaming = false;
    _imu_streaming = false;
    GetHAL().setImuStreaming(false);
    set_light_stream(false);
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
// queue_speaker_audio resamples a speaker message (uint16 LE rate, s16le mono) to the codec
// rate into the stream ring, or into the stored-sound ring; the speaker task mixes both.
void AppEmbodyMode::queue_speaker_audio(const std::string& payload, bool stored)
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
        auto& ring = stored ? _snd_samples : _spk_samples;
        if (!ring.reserve((size_t)out_rate * 3)) {  // keeps the newest 3 s
            mclog::tagError(_tag, "speaker: no memory for the buffer");
            return;
        }
        for (size_t i = 0; i < n_out; i++) {  // linear interpolation
            float x  = (float)i * in_rate / out_rate;
            size_t j = (size_t)x;
            float t  = x - j;
            int16_t a = sample(std::min(j, n_in - 1)), b = sample(std::min(j + 1, n_in - 1));
            ring.push((int16_t)(a + (b - a) * t));
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
    _snd_samples.clear();
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

    std::vector<int16_t> stored(chunk_len);
    while (self->_spk_running) {
        chunk.resize(chunk_len);
        {
            // Speech and stored sounds have their own rings and are mixed here: queued
            // into one they would be chopped into each other (noise).
            std::lock_guard<std::mutex> lock(self->_spk_mutex);
            const size_t ns = self->_spk_samples.pop(chunk.data(), chunk_len);
            const size_t nf = self->_snd_samples.pop(stored.data(), chunk_len);
            std::fill(chunk.begin() + ns, chunk.end(), 0);
            embody::mixInto(chunk.data(), stored.data(), nf);
            chunk.resize(std::max(ns, nf));
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
static constexpr uint32_t _light_stream_ms   = 50;  // stream sample period; the sensor reads proximity every 50 ms, light every 100 ms
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

    const uint32_t prox_every  = _light_streaming ? _light_stream_ms : _prox_interval_ms;
    const uint32_t light_every = _light_streaming ? 2 * _light_stream_ms : _light_interval_ms;
    uint16_t ps = 0;
    if (_proximity_on && now - _last_prox_read >= prox_every && _light->readProximity(ps)) {
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
    if (now - _last_light_read >= light_every && _light->readLux(lux, &_light_ch0, &_light_ch1)) {
        _last_light_read = now;
        if (now - _last_lux_update >= _light_interval_ms) {  // lux and auto-brightness keep the slow pace
            _last_lux_update = now;
            _lux             = _lux < 0 ? lux : _lux + (lux - _lux) * 0.3f;
            if (_auto_brightness && !_standby_until) {
                const int target = brightness_for_lux(_lux);
                if (std::abs(target - (int)GetHAL().getBackLightBrightness()) >= 4) {
                    GetHAL().setBackLightBrightness(target);  // not saved; the backlight fades
                }
            }
        }
    }

    // Stream: the latest raw values every 50 ms (proximity 0 while it is off), sent every 200 ms.
    if (!_light_streaming || now - _last_light_sample < _light_stream_ms) {
        return;
    }
    _last_light_sample = now;
    const uint16_t sample[3] = {(uint16_t)(_proximity_on ? _proximity : 0), _light_ch0, _light_ch1};
    _light_samples.append((const char*)&now, 4);  // little-endian
    _light_samples.append((const char*)sample, sizeof(sample));
    _light_sample_count++;
    if (now - _last_light_send >= 200 && _client) {
        _last_light_send = now;
        std::string msg;
        msg.push_back((char)(_light_sample_count & 0xFF));
        msg.push_back((char)(_light_sample_count >> 8));
        msg += _light_samples;
        _client->sendBinary(_bin_light, (const uint8_t*)msg.data(), msg.size());
        _light_samples.clear();
        _light_sample_count = 0;
    }
}

// light_stream: raw proximity and light samples as binary 0x08 (sensor at its fast rate).
void AppEmbodyMode::set_light_stream(bool on)
{
    if (on == _light_streaming) {
        return;
    }
    _light_streaming = on;
    _light_samples.clear();
    _light_sample_count = 0;
    if (_light) {
        _light->setFastRate(on);
    }
    mclog::tagInfo(_tag, "light stream {}", on ? "on" : "off");
}

/* -------------------------------- Rotation -------------------------------- */

void AppEmbodyMode::start_rotate(int velocity, int seconds, bool noHeadCable, bool usbPowerOk)
{
    if (!noHeadCable) {
        queue_event("rotate_refused", {}, {{"reason", "confirm that no cable is in the head's USB-C (no_head_cable)"}});
        return;
    }
    // USB power in: the robot cannot tell the head's USB-C from the stand's, so a second, explicit
    // confirmation that the cable is in the stand.
    hal_bridge::PmicStatus pmic;
    if (!usbPowerOk && hal_bridge::board_get_pmic_status(pmic) && pmic.vbus_mv > 1000) {
        queue_event("rotate_refused", {}, {{"reason", "USB power is in: confirm the cable is in the stand, not the head (usb_power_ok)"}});
        return;
    }
    if (!_servo_power) {
        queue_event("rotate_refused", {}, {{"reason", "servo power is off"}});
        return;
    }
    velocity = std::clamp(velocity, -1000, 1000);
    seconds  = std::clamp(seconds, 1, 30);
    stop_hold();
    _gesture = nullptr;
    pause_angle_sync();
    auto& m = GetStackChan().motion();
    m.setAutoTorqueReleaseEnabled(false);  // the release check would stop the wheel mode
    m.yawServo().rotate(velocity);
    const bool was_rotating = _rotate_until != 0;
    _rotate_until           = std::max<uint32_t>(GetHAL().millis() + seconds * 1000u, 1);
    _rotate_check_ms        = GetHAL().millis();
    _rotate_stall_ms        = 0;
    if (!was_rotating) {
        queue_event("rotate_on", {{"velocity", (double)velocity}, {"seconds", (double)seconds}});
    }
    mclog::tagInfo(_tag, "rotate {} for {} s", velocity, seconds);
}

// While rotating: the yaw servo's load every 200 ms; high for 0.6 s (a cable winding up, the head
// held or blocked) stops the rotation.
void AppEmbodyMode::check_rotate_safety()
{
    const uint32_t now = GetHAL().millis();
    if (!_rotate_until || now - _rotate_check_ms < 200) {
        return;
    }
    _rotate_check_ms = now;
    ServoStatus_t servo;
    if (!GetHAL().readServoStatus(1, servo)) {
        return;
    }
    const float load = std::abs(servo.load / 10.0f);  // percent
    if (load < 80.0f) {
        _rotate_stall_ms = 0;
        return;
    }
    if (!_rotate_stall_ms) {
        _rotate_stall_ms = now;
    } else if (now - _rotate_stall_ms >= 600) {
        stop_rotate();
        queue_event("rotate_stopped", {{"load_pct", load}}, {{"reason", "stall: high servo load"}});
        mclog::tagWarn(_tag, "rotate stopped: yaw load {:.0f}%", load);
    }
}

void AppEmbodyMode::stop_rotate()
{
    if (!_rotate_until) {
        return;
    }
    _rotate_until = 0;
    auto& m       = GetStackChan().motion();
    m.yawServo().rotate(0);
    m.setAutoTorqueReleaseEnabled(!_hold_until);
    _last_motion_tick = GetHAL().millis();
    queue_event("rotate_off");
    mclog::tagInfo(_tag, "rotate off");
}

/* ---------------------------------- Touch --------------------------------- */

// Raw touch points (FT6336, both fingers) every 20 ms: touch_down / touch_up events per
// finger, and while streaming, frames collected into binary 0x06 sent every 100 ms.
void AppEmbodyMode::update_touch()
{
    const uint32_t now = GetHAL().millis();
    if (now - _last_touch_poll < 20) {
        return;
    }
    _last_touch_poll = now;
    hal_bridge::RawTouch raw;
    if (!hal_bridge::board_get_touch(raw)) {
        return;
    }
    bool present[2] = {};
    for (int i = 0; i < raw.num; i++) {
        const int id = raw.id[i] & 1;
        if (raw.ev[i] == 1) {
            continue;  // lift-off report
        }
        present[id] = true;
        _touch_x[id] = raw.x[i];
        _touch_y[id] = raw.y[i];
        if (!_touch_down[id]) {
            _touch_down[id]  = true;
            _touch_since[id] = now;
            queue_event("touch_down", {{"id", (double)id}, {"x", (double)raw.x[i]}, {"y", (double)raw.y[i]}});
        }
    }
    for (int id = 0; id < 2; id++) {
        if (_touch_down[id] && !present[id]) {
            _touch_down[id] = false;
            queue_event("touch_up", {{"id", (double)id},
                                     {"x", (double)_touch_x[id]},
                                     {"y", (double)_touch_y[id]},
                                     {"ms", (double)(now - _touch_since[id])}});
        }
    }
    if (!_touch_streaming) {
        return;
    }
    // Frame: uint32 ms, uint8 n, then n x (uint8 id, uint16 x, uint16 y), little-endian
    const uint8_t n = (uint8_t)(present[0] + present[1]);
    _touch_frames.append((const char*)&now, 4);
    _touch_frames.push_back((char)n);
    for (int id = 0; id < 2; id++) {
        if (present[id]) {
            const uint16_t x = _touch_x[id], y = _touch_y[id];
            _touch_frames.push_back((char)id);
            _touch_frames.append((const char*)&x, 2);
            _touch_frames.append((const char*)&y, 2);
        }
    }
    _touch_frame_count++;
    if (now - _last_touch_send >= 100 && _client) {
        _last_touch_send = now;
        std::string msg;
        msg.push_back((char)(_touch_frame_count & 0xFF));
        msg.push_back((char)(_touch_frame_count >> 8));
        msg += _touch_frames;
        _client->sendBinary(_bin_touch, (const uint8_t*)msg.data(), msg.size());
        _touch_frames.clear();
        _touch_frame_count = 0;
    }
}

/* ------------------------------- IMU stream ------------------------------- */

// Buffered 100 Hz IMU samples as binary 0x05, every 100 ms: uint16 LE count, then per
// sample uint32 LE time (ms) and 9 float32 LE (accel m/s^2, gyro deg/s, magnetic uT).
void AppEmbodyMode::send_imu_stream()
{
    const uint32_t now = GetHAL().millis();
    if (!_imu_streaming || now - _last_imu_send < 100) {
        return;
    }
    _last_imu_send = now;
    std::vector<ImuStreamSample_t> samples;
    GetHAL().takeImuStream(samples);
    if (samples.empty()) {
        return;
    }
    std::string msg;
    msg.reserve(2 + samples.size() * 40);
    msg.push_back((char)(samples.size() & 0xFF));
    msg.push_back((char)(samples.size() >> 8));
    for (const auto& smp : samples) {
        msg.append((const char*)&smp.t_ms, 4);  // the ESP32-S3 is little-endian
        msg.append((const char*)smp.v, sizeof(smp.v));
    }
    _client->sendBinary(_bin_imu, (const uint8_t*)msg.data(), msg.size());
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
        if (_rotate_until) {  // a cable plugged in while the head turns: stop at once
            stop_rotate();
            queue_event("rotate_stopped", {}, {{"reason", "usb plugged in"}});
        }
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


/* --------------------------------- Servers -------------------------------- */

// The built-in server (Kconfig) first, then the stored ones; the default from NVS.
void AppEmbodyMode::load_servers()
{
    _servers.clear();
    // A release build has no built-in server: it is set up over USB (usb_setup.h), which
    // adds the server and makes it the default.
    const std::string builtin = CONFIG_STACKCHAN_EMBODY_SERVER_URL;
    _servers.push_back({builtin.empty() ? "Set up: chan.w42.eu/setup" : host_of(builtin), builtin,
                        CONFIG_STACKCHAN_EMBODY_TOKEN, "built-in"});
    Settings settings("embody", false);
    ArduinoJson::JsonDocument doc;
    if (!ArduinoJson::deserializeJson(doc, settings.GetString("servers", "[]"))) {
        for (ArduinoJson::JsonObject o : doc.as<ArduinoJson::JsonArray>()) {
            const std::string url = o["url"] | "";
            if (!url.empty() && find_server(url) < 0) {
                _servers.push_back({o["name"] | host_of(url).c_str(), url, o["token"] | "", o["origin"] | "added"});
            }
        }
    }
    _default_url      = settings.GetString("default", _servers[0].url);  // "" = none: choose at start
    if (_default_url.empty() && _servers.size() > 1 && builtin.empty()) {
        _default_url = _servers[1].url;  // set up over USB without "default": the one server there is
    }
    const int def     = find_server(_default_url);
    _server_index     = def < 0 ? 0 : def;
    _shown_index      = _server_index;
    _servers_rev++;
}

// Everything but the built-in entry (tokens included: NVS stays on the robot).
void AppEmbodyMode::save_servers()
{
    ArduinoJson::JsonDocument doc;
    auto arr = doc.to<ArduinoJson::JsonArray>();
    for (size_t i = 1; i < _servers.size(); i++) {
        auto o      = arr.add<ArduinoJson::JsonObject>();
        o["name"]   = _servers[i].name;
        o["url"]    = _servers[i].url;
        o["token"]  = _servers[i].token;
        o["origin"] = _servers[i].origin;
    }
    std::string json;
    ArduinoJson::serializeJson(doc, json);
    Settings settings("embody", true);
    settings.SetString("servers", json);
    settings.SetString("default", _default_url);
    _servers_rev++;
}

int AppEmbodyMode::find_server(const std::string& key)
{
    for (size_t i = 0; i < _servers.size(); i++) {
        if (_servers[i].url == key || _servers[i].name == key) {
            return (int)i;
        }
    }
    return -1;
}

// A server offers others (voluntarily): add them, or refresh ones it offered before.
void AppEmbodyMode::merge_offers(const std::string& serversJson)
{
    ArduinoJson::JsonDocument doc;
    if (ArduinoJson::deserializeJson(doc, serversJson)) {
        return;
    }
    for (ArduinoJson::JsonObject o : doc.as<ArduinoJson::JsonArray>()) {
        const std::string url = o["url"] | "";
        if (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0) {
            continue;
        }
        const std::string name = o["name"] | host_of(url).c_str();
        const int i            = find_server(url);
        if (i < 0) {
            _servers.push_back({name, url, o["token"] | "", "offered"});
        } else if (_servers[i].origin == "offered") {
            _servers[i].name = name;
            if (o["token"].is<const char*>()) {
                _servers[i].token = o["token"].as<const char*>();
            }
        }
    }
    save_servers();
    announce_servers();
}

// The "servers" event: the list without tokens, the current and the default one.
void AppEmbodyMode::announce_servers()
{
    ArduinoJson::JsonDocument doc;
    auto arr = doc.to<ArduinoJson::JsonArray>();
    for (const auto& e : _servers) {
        auto o       = arr.add<ArduinoJson::JsonObject>();
        o["name"]    = e.name;
        o["url"]     = e.url;
        o["origin"]  = e.origin;
        o["token"]   = !e.token.empty();  // only whether there is one
    }
    std::string list;
    ArduinoJson::serializeJson(doc, list);
    queue_event("servers", {}, {{"list", list}, {"current", _servers.empty() ? "" : _servers[_server_index].url},
                                {"default", _default_url}});
}

void AppEmbodyMode::render_server_row()
{
    if (!_title || _servers.empty() || _servers_rev == _rendered_servers_rev) {
        return;
    }
    _rendered_servers_rev = _servers_rev;
    _shown_index          = std::min(_shown_index, _servers.size() - 1);
    const auto& e         = _servers[_shown_index];
    const bool is_current = _client && _shown_index == _server_index;
    _title->setText(e.name);
    _server_pos->setText(_servers.size() > 1 ? fmt::format("{}/{}", _shown_index + 1, _servers.size()) : "");
    const bool is_default = e.url == _default_url;
    lv_obj_set_style_bg_color(_server_buttons[0], lv_color_hex(is_default ? _color_theme : 0xE8EBFF), 0);
    lv_label_set_text(lv_obj_get_child(_server_buttons[2], 0), is_current ? "Back to app" : "Connect");
    // The connect button stands out while it would switch servers
    lv_obj_set_style_bg_color(_server_buttons[2], lv_color_hex(is_current ? 0xE8EBFF : _color_theme), 0);
    _rendered_revision = UINT32_MAX;  // the QR side follows the shown server
    // Next only when there is somewhere to go
    _servers.size() > 1 ? lv_obj_remove_flag(_server_buttons[1], LV_OBJ_FLAG_HIDDEN)
                        : lv_obj_add_flag(_server_buttons[1], LV_OBJ_FLAG_HIDDEN);
}

// QR screen buttons (LVGL task): only record the request; the app loop acts on it.
void AppEmbodyMode::on_server_nav(lv_event_t* e)
{
    auto* self       = static_cast<AppEmbodyMode*>(lv_event_get_user_data(e));
    const int action = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
    self->_nav_request = action;
}

// (Re)connects to _servers[index]: a fresh client with that URL and token.
void AppEmbodyMode::connect_server(size_t index)
{
    if (_servers.empty()) {
        return;
    }
    index = std::min(index, _servers.size() - 1);
    if (_servers[index].url.empty()) {  // the release build's empty built-in entry: nothing to contact
        mclog::tagInfo(_tag, "not set up: connect over USB at chan.w42.eu/setup");
        return;
    }
    _server_index = index;
    _shown_index  = index;
    if (_client) {
        // Closing a TLS socket can block for up to 10 s (esp-ml307 waits for its receive
        // task), so the old client is destroyed in a task of its own, not in the app loop.
        // It is no longer updated, so none of its callbacks reach this app any more.
        auto* old = _client.release();
        if (xTaskCreate([](void* p) {
                delete static_cast<embody::Client*>(p);
                vTaskDelete(nullptr);
            }, "embody_close", 4096, old, 2, nullptr) != pdPASS) {
            delete old;
        }
    }
    _rendered_revision = UINT32_MAX;
    _rendered_url.clear();
    _rendered_viewers = 0;
    _qr_visible       = true;
    if (_panel) {
        _panel->setHidden(false);
    }
    mclog::tagInfo(_tag, "server {}: {}", _servers[index].name, _servers[index].url);
    std::vector<std::string> measurements = {"battery_pct", "charging", "head_yaw_deg", "head_pitch_deg", "wifi_rssi_dbm",
                         "free_heap_kb", "uptime_s", "brightness_pct", "volume_pct", "screensaver",
                         "imu_ax_g", "imu_ay_g", "imu_az_g", "imu_gyro_dps", "yaw_load_pct", "pitch_load_pct",
                         "yaw_temp_c", "pitch_temp_c", "servo_voltage_v", "chip_temp_c", "light_lux",
                         "proximity", "proximity_on", "auto_brightness", "core_battery_v", "core_vbus_v",
                         "core_system_v", "core_charge", "core_charge_phase", "pmic_temp_c", "pmic_status1",
                         "pmic_status2", "body_battery_v", "body_current_ma", "body_power_mw", "body_shunt_uv",
                         "hold_s", "usb_data", "yaw_pos_raw", "yaw_speed_raw", "yaw_current_raw", "yaw_moving",
                         "pitch_voltage_v", "pitch_pos_raw", "pitch_speed_raw", "pitch_current_raw", "pitch_moving",
                         "head_zone0", "head_zone1", "head_zone2", "mag_x_ut", "mag_y_ut", "mag_z_ut", "mag_raw_x", "mag_raw_y",
                         "mag_raw_z", "mag_rhall", "light_ch0", "light_ch1", "rtc_unix", "system_unix", "pmic_ts_raw",
                         "servo_power", "rotate_s"};
    measurements.insert(measurements.end(), _car_measurements.begin(), _car_measurements.end());
    _client = std::make_unique<embody::Client>(embody::Client::Config{
        .serverUrl = _servers[index].url,
        .token     = _servers[index].token,
        .robotId   = _robot_id,
        .model     = "stackchan-cores3",
        .firmware  = esp_app_get_description()->version,
        .commands  = _commands,
        .measurements = measurements,
        .e2e          = _e2e_ok && is_e2e(_servers[index].url) ? &_e2e : nullptr,
    });
    _client->onCommand = [this](const std::string& command, const std::string& args) {
        if (command == "speaker_flush") {  // now: the new line's audio arrives right after it
            flush_speaker();
            return;
        }
        if (command == "server_e2e" || command == "e2e_forget") {  // needs to know who sent it
            e2e_command(command, args, _client->lastCommandSealed());
            return;
        }
        _pending_commands.emplace_back(command, args);
    };
    // Driving must not wait for the app loop: car_* commands go straight to BLE from
    // the socket task (CarBle is thread-safe). car_enable and car_board touch the app
    // and NVS, so they take the normal path.
    _client->onFastCommand = [this](const std::string& command, const std::string& args) {
        if (command == "car_enable" || command == "car_board" || !_car || !_car_enabled) {
            return false;
        }
        ArduinoJson::JsonDocument doc;
        ArduinoJson::deserializeJson(doc, args);
        return car_command(command, doc);
    };
    _client->onBinary = [this](uint8_t type, const std::string& payload) {
        if (type == _bin_show_jpeg) {
            _pending_picture_jpeg = payload;  // decoded in the app loop, shown under the LVGL lock
        } else if (type == _bin_speaker_pcm) {
            queue_speaker_audio(payload);
        } else if (type == _bin_asset_chunk) {
            const auto res = _assets.put(payload);
            if (!res.error.empty()) {
                queue_event("asset_error", {}, {{"name", res.name}, {"reason", res.error}});
            } else if (res.done) {
                _sprite_layer.forget(res.name);  // a new version: decode it again
                queue_event("asset_saved", {{"bytes", (double)res.bytes}, {"crc", (double)res.crc}}, {{"name", res.name}});
            }
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
            {"hold_s", _hold_until ? (float)(((int32_t)(_hold_until - GetHAL().millis()) + 999) / 1000) : 0.0f},
            {"rotate_s", _rotate_until ? (float)(((int32_t)(_rotate_until - GetHAL().millis()) + 999) / 1000) : 0.0f},
            {"servo_power", _servo_power ? 1.0 : 0.0},
            {"system_unix", (double)time(nullptr)},
        };
        add_sensor_telemetry(t);
        return t;
    };

    _client->onServerOffer = [this](const std::string& offers) { merge_offers(offers); };
    _servers_announced = false;  // tell the new server the list once registered
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
    int misses        = 0;
    uint32_t polls    = 0;  // logged every 10 s: is the reader alive?
    uint32_t reported = GetHAL().millis();

    while (self->_nfc_running) {
        ST25R3916::Tag tag;
        const uint32_t start = GetHAL().millis();
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
                const std::string mem  = tag.sak == 0x00 ? nfc.readMemoryHex() : "";  // raw Type 2 memory
                nfc.halt();
                present = uid;
                embody::Client::Texts info = {{"uid", uid}, {"type", nfc_tag_type(tag.sak)}};
                if (!text.empty()) {
                    info.emplace_back("text", text);
                }
                if (!mem.empty()) {
                    info.emplace_back("mem", mem);
                }
                const uint32_t read_ms = GetHAL().millis() - start;  // the poll, UID, text and memory
                self->queue_event("nfc_tag", {{"atqa", (float)tag.atqa}, {"sak", (float)tag.sak}, {"read_ms", (double)read_ms}},
                                  std::move(info));
                mclog::tagInfo(_tag, "nfc tag {} ({}) {} in {} ms", uid, nfc_tag_type(tag.sak), text, read_ms);
            }
        } else if (!present.empty() && ++misses >= 2) {
            self->queue_event("nfc_removed", {}, {{"uid", present}});
            mclog::tagInfo(_tag, "nfc tag {} removed", present);
            present.clear();
        }
        nfc.fieldOff();
        polls++;
        if (GetHAL().millis() - reported >= 10000) {
            mclog::tagInfo(_tag, "nfc: {} polls in 10 s, last took {} ms", polls, GetHAL().millis() - start);
            polls    = 0;
            reported = GetHAL().millis();
        }
        for (int i = 0; i < 50 && self->_nfc_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    self->_nfc_task = nullptr;
    vTaskDelete(nullptr);
}

/* --------------------------------- Assets --------------------------------- */

// "assets" event: the stored files as JSON text {"files": [{"name", "bytes", "crc"}]},
// and the space in bytes as data. A server compares it with what it wants there.
void AppEmbodyMode::send_asset_list()
{
    ArduinoJson::JsonDocument doc;
    auto files = doc["files"].to<ArduinoJson::JsonArray>();
    for (const auto& e : _assets.list()) {
        auto o     = files.add<ArduinoJson::JsonObject>();
        o["name"]  = e.name;
        o["bytes"] = e.bytes;
        o["crc"]   = e.crc;
    }
    std::string list;
    ArduinoJson::serializeJson(doc, list);
    uint64_t total = 0, free = 0;
    _assets.usage(total, free);
    queue_event("assets", {{"total", (double)total}, {"free", (double)free}, {"mounted", _assets.mounted() ? 1.0 : 0.0}},
                {{"list", list}});
}

// Decodes the stored pictures that pending sprite/picture commands show, outside the
// LVGL lock (a PNG takes a few ms); run_command then finds them in the cache.
void AppEmbodyMode::prepare_pictures()
{
    for (const auto& [command, args_json] : _pending_commands) {
        if (command != "sprite" && command != "picture") {
            continue;
        }
        ArduinoJson::JsonDocument args;
        ArduinoJson::deserializeJson(args, args_json);
        const std::string asset = args["asset"] | "";
        if (asset.empty()) {
            continue;
        }
        const std::string err = _sprite_layer.load(_assets, asset);
        if (err.empty()) {
            _asset_load_errors.erase(asset);
        } else {
            _asset_load_errors[asset] = err;
        }
    }
}

/* ------------------------------ Stored sounds ----------------------------- */

static uint32_t le32(const uint8_t* b)
{
    return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
}

// start_sound opens a WAV (PCM, 16-bit, mono or stereo, any rate) from the file store;
// update_sound feeds it to the speaker. A new sound replaces the one playing.
void AppEmbodyMode::start_sound(const std::string& asset, float gain)
{
    stop_sound(false);
    if (auto codec = Board::GetInstance().GetAudioCodec()) {  // the old sound fades out, no click
        std::lock_guard<std::mutex> lock(_spk_mutex);
        _snd_samples.fadeOut((size_t)codec->output_sample_rate() / 100);
    }
    const std::string path = _assets.path(asset);
    FILE* f                = path.empty() ? nullptr : fopen(path.c_str(), "rb");
    auto fail              = [&](const char* reason) {
        if (f) {
            fclose(f);
        }
        queue_event("sound_error", {}, {{"asset", asset}, {"reason", reason}});
    };
    if (!f) {
        fail("no such file");
        return;
    }
    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12 || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        fail("not a WAV file");
        return;
    }
    int format = 0, channels = 0, rate = 0, bits = 0;
    uint8_t head[8];
    while (fread(head, 1, 8, f) == 8) {  // chunks: fmt, then data (others skipped)
        const uint32_t size = le32(head + 4);
        if (std::memcmp(head, "fmt ", 4) == 0 && size >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16) {
                break;
            }
            format   = fmt[0] | (fmt[1] << 8);
            channels = fmt[2] | (fmt[3] << 8);
            rate     = (int)le32(fmt + 4);
            bits     = fmt[14] | (fmt[15] << 8);
            fseek(f, (long)(size - 16 + (size & 1)), SEEK_CUR);
        } else if (std::memcmp(head, "data", 4) == 0) {
            if (format != 1 || bits != 16 || channels < 1 || channels > 2 || rate < 4000 || rate > 48000) {
                fail("need 16-bit PCM WAV, mono or stereo, 4-48 kHz");
                return;
            }
            _snd_file     = f;
            _snd_asset    = asset;
            _snd_left     = size;
            _snd_rate     = rate;
            _snd_channels = channels;
            _snd_gain     = gain;
            mclog::tagInfo(_tag, "play {} ({} Hz, {} ch, {} ms)", asset, rate, channels,
                           (int)((uint64_t)size * 1000 / (rate * 2 * channels)));
            return;
        } else {
            fseek(f, (long)(size + (size & 1)), SEEK_CUR);
        }
    }
    fail("no audio data");
}

void AppEmbodyMode::stop_sound(bool finished)
{
    if (!_snd_file) {
        return;
    }
    fclose(_snd_file);
    _snd_file = nullptr;
    if (finished) {
        queue_event("sound_done", {}, {{"asset", _snd_asset}});
    }
    _snd_asset.clear();
}

void AppEmbodyMode::update_sound()
{
    if (!_snd_file) {
        return;
    }
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        stop_sound(false);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_spk_mutex);
        if (_snd_samples.size() > (size_t)codec->output_sample_rate() / 2) {
            return;  // half a second is queued: enough
        }
    }
    const size_t frame = 2 * _snd_channels;
    uint8_t buf[4096];
    const size_t want = std::min<size_t>(_snd_left, sizeof(buf) / frame * frame);
    const size_t got  = fread(buf, 1, want, _snd_file) / frame * frame;
    // The speaker message layout: uint16 LE rate, then mono s16le (stereo is mixed down).
    std::string payload;
    payload.reserve(2 + got / _snd_channels);
    payload.push_back((char)(_snd_rate & 0xFF));
    payload.push_back((char)(_snd_rate >> 8));
    for (size_t i = 0; i < got; i += frame) {
        int32_t sum = 0;
        for (int c = 0; c < _snd_channels; c++) {
            int16_t s;
            std::memcpy(&s, buf + i + 2 * c, 2);
            sum += s;
        }
        const int16_t v = (int16_t)std::clamp<int32_t>((int32_t)(sum / _snd_channels * _snd_gain), -32768, 32767);
        payload.append((const char*)&v, 2);
    }
    if (payload.size() > 2) {
        queue_speaker_audio(payload, true);
    }
    _snd_left -= got;
    if (_snd_left == 0 || got < want) {
        stop_sound(true);
    }
}

// flush_speaker drops the queued speech, fading out over 10 ms (no click): a new line
// replaces an unfinished one ("speaker_flush", run as soon as it arrives).
void AppEmbodyMode::flush_speaker()
{
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        return;
    }
    std::lock_guard<std::mutex> lock(_spk_mutex);
    _spk_samples.fadeOut((size_t)codec->output_sample_rate() / 100);
}

/* ------------------------------- TPBot car -------------------------------- */

// Optional: a TPBot car whose micro:bit runs the tpbot-ble firmware
// (https://github.com/mj41/tpbot-ble), over BLE.
// Off until car_enable {"on": true}; the setting stays in NVS. The car_* names are
// the same as tpbot-bridge's (sbot readme, "Car capability"), so a server cannot
// tell whether the car hangs off this robot or off the bridge.

static constexpr const char* _car_commands[] = {"car_drive", "car_stop", "car_servo", "car_headlights",
                                                "car_sonar", "car_watchdog", "car_board"};

// (Re)builds the car part of the command list from the stored setting, and starts BLE when on.
void AppEmbodyMode::setup_car_commands(std::vector<std::string>& commands)
{
#if CONFIG_STACKCHAN_EMBODY_CAR
    commands.erase(std::remove_if(commands.begin(), commands.end(),
                                  [](const std::string& c) { return c.rfind("car_", 0) == 0; }),
                   commands.end());
    Settings settings("embody", false);
    _car_enabled = settings.GetBool("car_on", false);
    _car_board   = (uint8_t)std::clamp((int)settings.GetInt("car_board", 1), 0, 2);
    commands.push_back("car_enable");
    _car_measurements.clear();
    if (!_car_enabled) {
        if (_car) {
            _car->stop();
        }
        return;
    }
    for (const char* c : _car_commands) {
        commands.push_back(c);
    }
    _car_measurements = {"car_connected", "car_rssi_dbm", "car_echo_us", "car_line_l", "car_line_r",
                         "car_btn_a",     "car_btn_b",    "car_left",    "car_right",  "car_watchdog_stop",
                         "car_uptime_ms", "car_i2c_errors", "car_board"};
    if (!_car) {
        _car = std::make_unique<embody::CarBle>();
    }
    _car->start(_car_board);
    mclog::tagInfo(_tag, "car: on (board {}), free internal heap {} KB", _car_board,
                   heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
#else
    (void)commands;
#endif
}

void AppEmbodyMode::car_telemetry(embody::Client::Telemetry& t)
{
    if (!_car || !_car_enabled) {
        return;
    }
    const bool on = _car->connected();
    t.push_back({"car_connected", on ? 1.0 : 0.0});
    if (!on) {
        return;
    }
    const auto s = _car->state();
    t.push_back({"car_rssi_dbm", (double)_car->rssi()});
    t.push_back({"car_echo_us", (double)s.echo_us});
    t.push_back({"car_line_l", (double)(s.inputs & 1)});
    t.push_back({"car_line_r", (double)((s.inputs >> 1) & 1)});
    t.push_back({"car_btn_a", (double)((s.inputs >> 2) & 1)});
    t.push_back({"car_btn_b", (double)((s.inputs >> 3) & 1)});
    t.push_back({"car_left", (double)s.left});
    t.push_back({"car_right", (double)s.right});
    t.push_back({"car_watchdog_stop", (double)(s.flags & 1)});
    t.push_back({"car_uptime_ms", (double)s.ms});
    t.push_back({"car_i2c_errors", (double)s.i2c_err});
    t.push_back({"car_board", (double)s.board});
}

// App loop: link events, the car's state as telemetry (at most every 50 ms), and a
// stop when the server connection is gone (the micro:bit's watchdog is the backstop).
void AppEmbodyMode::update_car()
{
    if (!_car || !_car_enabled) {
        return;
    }
    for (auto& e : _car->poll()) {
        queue_event(e.name.c_str(), e.name == "car_disconnected" ? embody::Client::Telemetry{{"reason", (double)e.reason}}
                                                                 : embody::Client::Telemetry{},
                    {{"car", e.car}, {"addr", e.addr}});
    }
    const bool online = _client && _client->isRegistered();
    if (!online) {
        if (!_car_offline_stopped && _car->connected()) {
            _car->stopMotors();
        }
        _car_offline_stopped = true;
        return;
    }
    _car_offline_stopped = false;
    bool fresh           = false;
    _car->state(&fresh);  // only peeks at "fresh"; car_telemetry reads the state again
    const uint32_t now = GetHAL().millis();
    if (fresh && now - _car_last_tele >= 50) {
        _car_last_tele = now;
        embody::Client::Telemetry t;
        car_telemetry(t);
        _client->sendTelemetry(t);
    }
}

// Returns true when the command was a car command (handled or refused).
bool AppEmbodyMode::car_command(const std::string& command, const ArduinoJson::JsonDocument& args)
{
    if (command.rfind("car_", 0) != 0) {
        return false;
    }
#if CONFIG_STACKCHAN_EMBODY_CAR
    if (command == "car_enable") {
        const bool on = args["on"] | false;
        Settings settings("embody", true);
        settings.SetBool("car_on", on);
        if (args["board"].is<const char*>()) {
            const std::string b = args["board"].as<const char*>();
            settings.SetInt("car_board", b == "both" ? 0 : b == "v2" ? 2 : 1);
        }
        mclog::tagInfo(_tag, "car_enable {}: registering again", on);
        setup_car_commands(_commands);
        connect_server(_server_index);  // a new Register with the new command list
        return true;
    }
    if (!_car || !_car_enabled) {
        return true;
    }
    auto& c = *_car;
    if (command == "car_drive") {
        c.drive(args["left"] | 0, args["right"] | 0);
    } else if (command == "car_stop") {
        c.stopMotors();
    } else if (command == "car_servo") {
        c.servo(args["port"] | 1, args["angle"] | 90);
    } else if (command == "car_headlights") {
        uint32_t rgb = 0;
        if (parse_hex_color(args["color"] | "#000000", rgb)) {
            c.headlights((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        }
    } else if (command == "car_sonar") {
        c.sonar(args["hz"] | 10);
    } else if (command == "car_watchdog") {
        c.watchdog(args["ms"] | 500);
    } else if (command == "car_board") {
        const std::string b = args["board"] | "v1";
        const uint8_t mode  = b == "both" ? 0 : b == "v2" ? 2 : 1;
        Settings settings("embody", true);
        settings.SetInt("car_board", mode);
        _car_board = mode;
        c.board(mode);
    }
#endif
    return true;
}

/* ------------------------------- Automation ------------------------------- */

// Optional (CONFIG_STACKCHAN_EMBODY_AUTOMATION, off by default): lets a server, or an AI
// agent through it, run the robot without anyone touching it. See automation.h.
//   automation {"autostart": bool}  open Embody Mode after every power-on or restart
//   restart                         restart the robot, back into Embody Mode
//   launch {"app": "<name>"}        restart into another launcher app once ("" = launcher)
// Returns true when the command was one of these.
bool AppEmbodyMode::automation_command(const std::string& command, const ArduinoJson::JsonDocument& args)
{
#if CONFIG_STACKCHAN_EMBODY_AUTOMATION
    if (command == "automation") {
        if (args["autostart"].is<bool>()) {
            embody::set_autostart(args["autostart"].as<bool>());
        }
        const bool on = embody::autostart();
        mclog::tagInfo(_tag, "automation: autostart {}", on);
        queue_event("automation", {{"autostart", on ? 1.0 : 0.0}});
        return true;
    }
    if (command == "restart" || command == "launch") {
        std::string app = command == "restart" ? embody::kEmbodyAppName : (args["app"] | "");
        if (app.empty() || strcasecmp(app.c_str(), embody::kLauncherName) == 0) {
            app = embody::kLauncherName;
        } else {
            const auto apps = mooncake::GetMooncake().getAllAppProps();
            const auto it   = std::find_if(apps.begin(), apps.end(), [&](const auto& p) {
                return strcasecmp(p.info.name.c_str(), app.c_str()) == 0;
            });
            if (it == apps.end()) {
                mclog::tagWarn(_tag, "launch: no app named {}", app);
                queue_event("launch_unknown", {}, {{"app", app}});
                return true;
            }
            app = it->info.name;
        }
        mclog::tagInfo(_tag, "{}: restarting into {}", command, app);
        embody::set_launch_once(app);
        GetHAL().delay(300);  // let the log and the socket flush
        esp_restart();
    }
#else
    (void)command;
    (void)args;
#endif
    return false;
}

/* ----------------------- End-to-end encryption ---------------------------- */

// Which servers get only ciphertext (home-w42-eu docs/e2ee.md): a list of URLs in NVS.
void AppEmbodyMode::load_e2e_urls()
{
    _e2e_urls.clear();
    Settings settings("embody", false);
    ArduinoJson::JsonDocument doc;
    if (!ArduinoJson::deserializeJson(doc, settings.GetString("e2e_urls", "[]"))) {
        for (const char* u : doc.as<ArduinoJson::JsonArray>()) {
            if (u) {
                _e2e_urls.emplace_back(u);
            }
        }
    }
}

bool AppEmbodyMode::is_e2e(const std::string& url) const
{
    return std::find(_e2e_urls.begin(), _e2e_urls.end(), url) != _e2e_urls.end();
}

// server_e2e {server?, on}: anyone may turn encryption on (it only protects more); only an
// enrolled browser (a sealed command) may turn it off, so a relay cannot downgrade it.
// e2e_forget: an enrolled browser forgets every enrolled browser (a new epoch).
void AppEmbodyMode::e2e_command(const std::string& command, const std::string& args_json, bool sealed)
{
    ArduinoJson::JsonDocument args;
    ArduinoJson::deserializeJson(args, args_json);
    auto refuse = [&](const char* reason) {
        mclog::tagWarn(_tag, "{} refused: {}", command, reason);
        queue_event("e2e_refused", {}, {{"command", command}, {"reason", reason}});
    };
    if (command == "e2e_forget") {
        if (!sealed) {
            return refuse("only from an enrolled browser");
        }
        _e2e.forgetAll();
        queue_event("e2e_forgotten");
        return;
    }
    const std::string key = args["server"] | "";
    const int i           = key.empty() ? (int)_server_index : find_server(key);
    const bool on         = args["on"] | false;
    if (i < 0) {
        return refuse("no such server");
    }
    if (on && !_e2e_ok) {
        return refuse("encryption is not available");
    }
    if (!on && !sealed) {
        return refuse("only an enrolled browser may turn encryption off");
    }
    const std::string url = _servers[i].url;
    if (on == is_e2e(url)) {
        return;
    }
    if (on) {
        _e2e_urls.push_back(url);
    } else {
        _e2e_urls.erase(std::remove(_e2e_urls.begin(), _e2e_urls.end(), url), _e2e_urls.end());
    }
    ArduinoJson::JsonDocument doc;
    auto arr = doc.to<ArduinoJson::JsonArray>();
    for (const auto& u : _e2e_urls) {
        arr.add(u);
    }
    std::string json;
    ArduinoJson::serializeJson(doc, json);
    Settings settings("embody", true);
    settings.SetString("e2e_urls", json);
    mclog::tagInfo(_tag, "e2e {} for {}", on ? "on" : "off", url);
    if ((size_t)i == _server_index) {
        _pending_switch = i;  // reconnect: Register says e2e, the QR code gets its fragment
    }
}
