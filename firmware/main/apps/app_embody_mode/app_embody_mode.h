/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "embody_client.h"
#include "manager_channel.h"
#include "e2e.h"
#include "asset_store.h"
#include "sprite_layer.h"
#include "sample_ring.h"
#include "car_ble.h"
#include <hal/drivers/ST25R3916/st25r3916.h>
#include <hal/drivers/LTR553/ltr553.h>
#include <hal/drivers/IrRemote/ir_remote.h>
#include <hal/drivers/INA226/ina226.h>
#include <mooncake.h>
#include <lvgl.h>
#include <atomic>
#include <cstddef>
#include <deque>
#include <map>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ArduinoJson.hpp>
#include <driver/temperature_sensor.h>

class LvglAllocatedImage;

namespace smooth_ui_toolkit::lvgl_cpp {
class Container;
class Label;
}  // namespace smooth_ui_toolkit::lvgl_cpp

/**
 * @brief Embody Mode: remote control through s-w42-eu-raw.
 *
 * Connects to CONFIG_STACKCHAN_EMBODY_SERVER_URL and shows the server's pairing
 * URL as a QR code. Once a browser pairs, the robot shows its face; a long
 * press brings the QR back, a tap is reported as a "screen_tap" event.
 * Handles the basic command set (see s-w42-eu-raw internal/wire), shows
 * pictures sent from the phone, streams camera and microphone while someone
 * watches, plays browser audio, streams telemetry and reports robot events
 * (touch, IMU, NFC tags).
 */
class AppEmbodyMode : public mooncake::AppAbility {
public:
    AppEmbodyMode();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

    struct GestureStep {
        char axis;   // 'y' yaw or 'p' pitch
        int offset;  // 0.1 degree, relative to the angle when the gesture started
    };

private:
    std::unique_ptr<embody::Client> _client;

    // Servers (apps) the robot can use: the built-in one (Kconfig), its managers' apps and ones
    // written over USB. Stored in NVS ("embody"). The QR screen switches between them; the default
    // one is used at start. Apps never add, remove or pin apps: that is the managers' job.
    struct ServerEntry {
        std::string name, url, token, origin;  // origin: built-in, manager:<id>, added (USB)
    };
    std::vector<ServerEntry> _servers;
    size_t _server_index = 0;  // the server the client talks to
    size_t _shown_index  = 0;  // the server shown on the QR screen: Next browses, Connect switches
    std::string _default_url;
    std::string _robot_id;
    std::vector<std::string> _commands;
    bool _servers_announced = false;
    int _pending_switch     = -1;                // from server_switch, done at the top of the loop
    // A server's server_switch, or a manager's new start app, is confirmed on
    // the robot's screen first, as a new default over USB: only the person at the robot moves it.
    std::shared_ptr<std::atomic<int>> _server_answer;  // 0 waiting, 1 Yes, -1 No or no answer
    std::string _server_ask_url;
    bool _server_ask_default = false;  // true: make it the default; false: switch to it
    std::atomic<int> _nav_request{0};            // from the QR screen: +1 next, 2 pin, 3 connect/close
    lv_obj_t* _server_buttons[4] = {};           // pin, next, back to app / connect, the Manager screen
    std::atomic<bool> _qr_hide_requested{false};  // the close button on the QR screen
    bool _qr_pinned           = false;
    bool _qr_back_to_app      = false;  // Connect on the QR screen: its face once connected
    std::atomic<bool> _reset_app_look{false};  // another app now: reset_for_app
    std::string _last_app_url;                  // the app connect_server last went to
    uint8_t _start_volume = 50;                 // the speaker volume at start: every app starts with it
    embody::ManagerChannel _channel;  // to the robot's manager (manager_channel.h)
    // The Manager screen (update_manager_screen)
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _mgr_panel;
    lv_obj_t* _mgr_name = nullptr;
    lv_obj_t* _mgr_state = nullptr;
    lv_obj_t* _mgr_qr = nullptr;
    lv_obj_t* _mgr_page = nullptr;
    lv_obj_t* _mgr_second = nullptr;
    std::string _mgr_qr_text;
    bool _mgr_show = false;
    std::atomic<bool> _mgr_use_second{false};
    lv_obj_t* _mgr_toggle_btn = nullptr;  // "Turn off" / "Turn on"
    std::atomic<int> _mgr_toggle{0};      // 7: off, 8: on, asked on the screen
    bool _mgr_toggle_on = false;
    std::shared_ptr<std::atomic<int>> _mgr_toggle_answer;
    std::shared_ptr<std::atomic<int>> _mgr_answer;
    uint32_t _mgr_rendered_at = 0;
    uint32_t _mgr_state_at = 0;
    uint32_t _mgr_apps_rev = UINT32_MAX;
    std::string _mgr_apps;       // its apps as {id, name}, for the channel's Hello
    std::string _switch_answer;  // the last answer to a switch: switched, not confirmed, refused: …
    bool _rendered_qr_visible = false;  // for the swipe-up bar's QR / APP text
    uint32_t _servers_rev          = 0;          // bumped on any change, for the QR screen row
    uint32_t _rendered_servers_rev = UINT32_MAX;
    bool _network_started = false;

    // QR panel, shown over the face until a browser pairs
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _panel;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _title;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _server_pos;  // "1/2" under the server name
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _qr_box;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _qr_hint;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _status;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _code;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _detail;
    lv_obj_t* _qr = nullptr;  // child of _qr_box, deleted with it

    // Picture from the phone, shown over the face (below the QR panel)
    lv_obj_t* _picture_obj = nullptr;
    lv_obj_t* _face_obj    = nullptr;  // the avatar's panel: taps on it and its parts (on_face_input)
    lv_indev_t* _touch_indev = nullptr;
    std::shared_ptr<LvglAllocatedImage> _picture;
    std::string _picture_asset;  // the shown picture's file ("sent" for a JPEG from the server), "" = the face
    void queue_tap(const char* name, int x, int y);
    std::shared_ptr<LvglAllocatedImage> _pending_picture;  // decoded, waiting for the LVGL lock
    std::string _pending_picture_jpeg;                     // received, waiting to be decoded

    // "LIVE" badge on the top layer while the camera or microphone streams
    lv_obj_t* _live_badge = nullptr;
    // The app's name for a moment after connecting, so a switch shows (even with sealed traffic).
    lv_obj_t* _app_card       = nullptr;
    std::string _app_card_text;  // set when registered (outside the LVGL lock), shown under it
    uint32_t _app_card_until  = 0;
    lv_obj_t* _app_badge      = nullptr;  // then a small label with its name, while connected

    // Screensaver: a blank screen. "Auto" after CONFIG_STACKCHAN_EMBODY_SCREENSAVER_S
    // without touch or command (commands and live media count as use); "manual"
    // from a double tap or the screensaver command, and only a touch or
    // "screensaver off" ends it. The Embody screen stays intact underneath.
    lv_obj_t* _blank_screen = nullptr;
    lv_obj_t* _prev_screen  = nullptr;
    bool _blank_manual      = false;
    uint32_t _blank_since   = 0;
    uint32_t _last_activity = 0;  // last command, picture or live media (ms)
    uint32_t _seen_commands = 0;  // embody::Client::commandCount() already counted
    std::atomic<bool> _blank_requested{false};  // double tap, set from the LVGL task
    // Touching the robot outside the screen counts like a screen touch: a head
    // press or swipe, or a new NFC tag (ms; set from the head-touch and NFC tasks).
    std::atomic<uint32_t> _last_physical{0};

    // Standby (the "standby" command): offline, backlight off, blank screen,
    // LEDs/camera/mic off, until _standby_until or a touch.
    uint32_t _standby_until        = 0;  // ms; 0 = not in standby
    uint32_t _standby_since        = 0;
    bool _standby_disconnect       = false;  // disconnect once the "standby" event is sent
    uint8_t _standby_brightness    = 60;

    bool _qr_visible = true;
    int _rendered_viewers = 0;
    std::atomic<bool> _toggle_qr_requested{false};  // set from LVGL event callbacks
    uint32_t _rendered_revision = UINT32_MAX;
    std::string _rendered_url;
    std::string _last_command;

    // Commands arrive during _client->update(), outside the LVGL lock, and run
    // afterwards under it.
    std::vector<std::pair<std::string, std::string>> _pending_commands;

    // End-to-end encryption (e2e.h), per server: on for the URLs in _e2e_urls (NVS).
    embody::E2E _e2e;
    bool _e2e_ok = false;
    bool _boot_stable = false;  // ran a minute: the boot-loop guard counts from zero again
    bool _wifi_low_latency = false;  // Wi-Fi power save off while someone uses the robot
    uint32_t _rotate_check_ms = 0;   // rotation: last servo feedback check
    uint32_t _rotate_stall_ms = 0;   // rotation: since when the head has not moved (0: it moves)
    uint32_t _rotate_start_ms = 0;
    int _rotate_last_pos      = -1;  // raw yaw position at the last check (-1: none yet)
    std::string _rotate_trace;       // "ms:pos:speed:current:load;" every check, sent as rotate_trace
    uint32_t _ps_seen_commands = 0;  // the client's command count when power save last looked
    uint32_t _last_command_ms = 0;   // when a command last arrived (any, ping too)
    std::string _published_pair;  // the pairing link given to the USB setup ({"op":"pair"})
    std::string _published_status;  // what {"op":"status"} answers over USB (publish_status)
    uint32_t _status_at = 0;
    void publish_status(uint32_t now);
    std::vector<std::string> _e2e_urls;
    void load_e2e_urls();
    bool is_e2e(const std::string& url) const;
    void e2e_command(const std::string& command, const std::string& args, bool sealed);

    // Robot events come from HAL tasks (IMU, head touch), the NFC task and LVGL callbacks (taps).
    struct PendingEvent {
        std::string name;
        embody::Client::Telemetry data;
        embody::Client::Texts text;
    };
    std::mutex _event_mutex;
    std::vector<PendingEvent> _pending_events;
    size_t _imu_connection = 0;
    size_t _head_connection = 0;
    uint32_t _head_press_ms = 0;  // head-touch task only

    // Light + proximity (LTR-553 in the CoreS3), read in the app loop:
    // telemetry, auto-brightness from the room light (on by default), and an
    // approach wakes the screen like a touch.
    std::unique_ptr<LTR553> _light;

    // Uploaded pictures and sounds (binary 0x11), kept in the userdata partition.
    embody::AssetStore _assets;
    void send_asset_list();
    // Stored pictures shown over the face ("sprite") or full screen ("picture" with an asset).
    embody::SpriteLayer _sprite_layer;
    std::map<std::string, std::string> _asset_load_errors;  // why a file could not be decoded
    void prepare_pictures();
    bool _auto_brightness     = false;
    float _lux                = -1;  // smoothed; < 0 until the first reading
    uint16_t _proximity       = 0;
    bool _proximity_on        = true;  // the "proximity" command; off = the sensor's IR LED stops
    float _prox_base          = -1;  // slowly adapting "nobody near" level
    bool _near                = false;
    uint32_t _last_light_read = 0;
    uint32_t _last_prox_read  = 0;
    uint32_t _last_lux_update = 0;  // auto-brightness keeps its slow pace while streaming
    uint32_t _light_unsteady_at = 0;  // something moved near the sensor (proximity): lux unreliable
    uint32_t _last_auto_bright  = 0;  // the last auto-brightness step
    // Raw light stream (binary 0x08), on while asked (light_stream): 20 samples/s.
    bool _light_streaming         = false;
    std::string _light_samples;  // pending: (uint32 ms, uint16 ps, uint16 ch0, uint16 ch1) each
    uint16_t _light_sample_count  = 0;
    uint32_t _last_light_sample   = 0;
    uint32_t _last_light_send     = 0;
    void set_light_stream(bool on);

    // Power: the body battery monitor (INA226) and the CoreS3 power chip (AXP2101)
    // for telemetry; power-key and plug events are polled from the AXP2101.
    std::unique_ptr<INA226> _body_power;
    uint32_t _last_power_poll = 0;

    // Infrared (LED on G5, receiver on G10): "ir_send" and "ir_received" events.
    std::unique_ptr<IrRemote> _ir;

    // Optional TPBot car over BLE (CONFIG_STACKCHAN_EMBODY_CAR, then car_enable).
    std::unique_ptr<embody::CarBle> _car;
    std::atomic<bool> _car_enabled{false};  // read by the socket task (car fast path)
    uint8_t _car_board         = 1;  // 0 both, 1 V1, 2 V2 frames
    uint32_t _car_last_tele    = 0;
    bool _car_offline_stopped  = false;
    std::vector<std::string> _car_measurements;
    void setup_car_commands(std::vector<std::string>& commands);
    void update_car();
    void car_telemetry(embody::Client::Telemetry& t);
    bool car_command(const std::string& command, const ArduinoJson::JsonDocument& args);

    // Optional automation (CONFIG_STACKCHAN_EMBODY_AUTOMATION): autostart, restart, launch.
    bool automation_command(const std::string& command, const ArduinoJson::JsonDocument& args);

    // ESP32-S3 internal temperature sensor, for the chip_temp_c measurement.
    temperature_sensor_handle_t _tsens = nullptr;

    // Camera: frames are captured and JPEG-encoded in the app loop while on.
    bool _camera_on           = false;
    uint32_t _last_frame_tick = 0;
    int _camera_want_w = 320, _camera_want_h = 240;  // the stream's size ("camera" "size"); switched outside the LVGL lock
    int _camera_quality = 25;                         // JPEG quality, lowered when a 640x480 frame does not fit a message

    // Microphone: a task reads the codec into _mic_samples; the app loop sends them.
    std::atomic<bool> _mic_running{false};
    std::atomic<TaskHandle_t> _mic_task{nullptr};  // cleared by the task when it exits
    std::mutex _mic_mutex;
    std::vector<int16_t> _mic_samples;
    int _mic_rate     = 0;
    int _mic_channels = 1;  // all codec input channels, interleaved in _mic_samples

    // Speaker: browser audio (binary 0x03) is resampled to the codec rate and
    // queued; a task plays it, and the mouth moves while it does.
    std::mutex _spk_mutex;
    embody::SampleRing _spk_samples;  // streamed audio (speech): 3 s at the codec rate, in PSRAM
    embody::SampleRing _snd_samples;  // stored sounds ("play"): mixed with the stream
    std::atomic<bool> _spk_running{false};
    std::atomic<TaskHandle_t> _spk_task{nullptr};  // cleared by the task when it exits
    std::atomic<uint32_t> _spk_last_audio{0};      // ms, last chunk written to the codec

    // A stored sound ("play" a WAV from the file store), fed to the speaker in pieces
    // so it is never more than about half a second ahead.
    FILE* _snd_file = nullptr;
    std::string _snd_asset;
    uint32_t _snd_left = 0;  // data bytes left
    int _snd_rate = 0, _snd_channels = 0;
    float _snd_gain = 1.0f;
    void start_sound(const std::string& asset, float gain);
    void stop_sound(bool finished);
    void flush_speaker();
    void update_sound();
    uint32_t _speaking_until = 0;

    // LEDs: 12 pixels, left 0-5 and right 6-11. "leds" fades a side (NeonLight),
    // sets single pixels, or runs an effect that update_leds() draws in the app loop.
    enum class LedEffect { None, Rainbow, Breathe, Chase, Blink };
    LedEffect _led_effect    = LedEffect::None;
    uint32_t _led_color      = 0xFFFFFF;  // effect colour
    float _led_speed         = 1.0f;
    uint32_t _led_start      = 0;
    uint32_t _led_until      = 0;  // ms; 0 = until the next "leds"
    uint32_t _led_last_frame = 0;
    uint32_t _led_left       = 0;  // side colours, restored when a timed effect ends
    uint32_t _led_right      = 0;

    // NFC: a task polls the ST25R3916 reader (the RF field is on only while it
    // polls) and reports "nfc_tag" / "nfc_removed" events. On by default when
    // the reader answers; the "nfc" command switches polling.
    std::unique_ptr<ST25R3916> _nfc;
    bool _nfc_enabled = false;
    std::atomic<bool> _nfc_running{false};
    std::atomic<TaskHandle_t> _nfc_task{nullptr};  // cleared by the task when it exits

    // Head gestures (nod, shake) as timed moves; auto angle sync is paused
    // while the head is commanded so hand-moved-angle tracking doesn't fight it.
    const GestureStep* _gesture = nullptr;
    size_t _gesture_len         = 0;
    size_t _gesture_step        = 0;
    uint32_t _gesture_next_tick = 0;
    int _gesture_base_yaw       = 0;
    int _gesture_base_pitch     = 0;
    uint32_t _last_motion_tick  = 0;
    bool _angle_sync_paused     = false;

    // Hold position: torque stays on for a limited time (the "hold" command, 30 s-5 min);
    // otherwise the firmware releases it at rest and the head can be turned by hand.
    uint32_t _hold_until = 0;  // ms; 0 = not holding

    // Continuous yaw rotation ("rotate"), time-limited and only after the person at the robot
    // tapped Yes on its screen: a cable in the head's USB-C would wind up (which port is used
    // cannot be detected). The Yes holds until a cable is plugged in or out, or the app closes.
    uint32_t _rotate_until = 0;  // ms; 0 = not rotating
    bool _rotate_confirmed = false;
    std::shared_ptr<std::atomic<int>> _rotate_answer;  // the open question: 0 waiting, 1 Yes, -1 No or no answer
    int _rotate_ask_velocity = 0;                       // the rotation asked for, started after Yes
    int _rotate_ask_seconds  = 0;
    bool _servo_power      = true;

    // Touch: touch_down/touch_up events per finger; raw frames (binary 0x06) while streaming.
    bool _touch_down[2]         = {};
    int _touch_x[2]             = {}, _touch_y[2] = {};
    uint32_t _touch_since[2]    = {};
    uint32_t _last_touch_poll   = 0;
    uint32_t _last_touch_send   = 0;
    bool _touch_streaming       = false;
    std::string _touch_frames;  // pending stream frames
    uint16_t _touch_frame_count = 0;
    uint16_t _light_ch0 = 0, _light_ch1 = 0;

    // Full-resolution still (the "snapshot" command), taken in the app loop.
    bool _snapshot_requested = false;

    // Raw IMU stream (binary 0x05), on while a browser asks for it (the server sends imu_stream).
    bool _imu_streaming      = false;
    uint32_t _last_imu_send  = 0;

    void create_view();
    void render();
    void update_screensaver();
    void start_standby(int minutes);
    void update_standby();
    void enter_blank(bool manual);
    void leave_blank();
    void wake_screen();
    uint32_t touch_idle_ms();
    void run_command(const std::string& command, const std::string& args);
    void reset_for_app();
    void add_sensor_telemetry(embody::Client::Telemetry& t);
    void update_light();
    void update_ir();
    void update_power_events();
    void load_servers();
    void save_servers();
    void connect_server(size_t index);
    void apply_signed(const std::string& kind, const std::string& payload);
    void publish_manager_state(uint32_t now);
    void update_manager_screen();
    std::string shown_name(size_t index);  // the name on screen: with its manager when two share it
    void announce_servers();
    void render_server_row();
    int find_server(const std::string& key);
    static void on_server_nav(lv_event_t* e);
    void send_imu_stream();
    void request_server_change(int index, bool makeDefault, const std::string& by = "a server");
    void update_server_question();
    void request_rotate(int velocity, int seconds);
    void update_rotate_question();
    void forget_rotate_confirmation(const char* reason);
    void start_rotate(int velocity, int seconds);
    void check_rotate_safety();
    void stop_rotate();
    void update_touch();
    void take_snapshot();
    void queue_event(const char* name, embody::Client::Telemetry data = {}, embody::Client::Texts text = {});
    static void on_screen_event(lv_event_t* e);
    static void on_face_input(lv_event_t* e);
    void screen_input(lv_event_code_t code);
    void pause_angle_sync();
    void start_gesture(const GestureStep* steps, size_t count);
    void update_motion();
    void start_hold(int seconds);
    void stop_hold();
    void update_hold();
    void send_camera_frame();
    void update_camera_size();
    void start_mic();
    void stop_mic();
    static void mic_task(void* arg);
    void send_mic_audio();
    void queue_speaker_audio(const std::string& payload, bool stored = false);
    void stop_speaker();
    static void speaker_task(void* arg);
    void run_leds(const ArduinoJson::JsonDocument& args);
    void update_leds();
    void stop_led_effect(bool restore_sides);
    void start_nfc();
    void stop_nfc();
    static void nfc_task(void* arg);
};
