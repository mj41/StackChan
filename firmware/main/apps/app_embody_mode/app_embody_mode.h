/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "embody_client.h"
#include <hal/drivers/ST25R3916/st25r3916.h>
#include <hal/drivers/LTR553/ltr553.h>
#include <hal/drivers/IrRemote/ir_remote.h>
#include <hal/drivers/INA226/ina226.h>
#include <mooncake.h>
#include <lvgl.h>
#include <atomic>
#include <cstddef>
#include <deque>
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
 * @brief Embody Mode: remote control through stackchan-server.
 *
 * Connects to CONFIG_STACKCHAN_EMBODY_SERVER_URL and shows the server's pairing
 * URL as a QR code. Once a browser pairs, the robot shows its face; a long
 * press brings the QR back, a tap is reported as a "screen_tap" event.
 * Handles the basic command set (see stackchan-server internal/wire), shows
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
    bool _network_started = false;

    // QR panel, shown over the face until a browser pairs
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _panel;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _title;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _qr_box;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _qr_hint;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _status;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _code;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _detail;
    lv_obj_t* _qr = nullptr;  // child of _qr_box, deleted with it

    // Picture from the phone, shown over the face (below the QR panel)
    lv_obj_t* _picture_obj = nullptr;
    std::shared_ptr<LvglAllocatedImage> _picture;
    std::shared_ptr<LvglAllocatedImage> _pending_picture;  // decoded, waiting for the LVGL lock
    std::string _pending_picture_jpeg;                     // received, waiting to be decoded

    // "LIVE" badge on the top layer while the camera or microphone streams
    lv_obj_t* _live_badge = nullptr;

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
    bool _auto_brightness     = false;
    float _lux                = -1;  // smoothed; < 0 until the first reading
    uint16_t _proximity       = 0;
    bool _proximity_on        = true;  // the "proximity" command; off = the sensor's IR LED stops
    float _prox_base          = -1;  // slowly adapting "nobody near" level
    bool _near                = false;
    uint32_t _last_light_read = 0;
    uint32_t _last_prox_read  = 0;

    // Power: the body battery monitor (INA226) and the CoreS3 power chip (AXP2101)
    // for telemetry; power-key and plug events are polled from the AXP2101.
    std::unique_ptr<INA226> _body_power;
    uint32_t _last_power_poll = 0;

    // Infrared (LED on G5, receiver on G10): "ir_send" and "ir_received" events.
    std::unique_ptr<IrRemote> _ir;

    // ESP32-S3 internal temperature sensor, for the chip_temp_c measurement.
    temperature_sensor_handle_t _tsens = nullptr;

    // Camera: frames are captured and JPEG-encoded in the app loop while on.
    bool _camera_on           = false;
    uint32_t _last_frame_tick = 0;

    // Microphone: a task reads the codec into _mic_samples; the app loop sends them.
    std::atomic<bool> _mic_running{false};
    std::atomic<TaskHandle_t> _mic_task{nullptr};  // cleared by the task when it exits
    std::mutex _mic_mutex;
    std::vector<int16_t> _mic_samples;
    int _mic_rate = 0;

    // Speaker: browser audio (binary 0x03) is resampled to the codec rate and
    // queued; a task plays it, and the mouth moves while it does.
    std::mutex _spk_mutex;
    std::deque<int16_t> _spk_samples;
    std::atomic<bool> _spk_running{false};
    std::atomic<TaskHandle_t> _spk_task{nullptr};  // cleared by the task when it exits
    std::atomic<uint32_t> _spk_last_audio{0};      // ms, last chunk written to the codec
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
    void add_sensor_telemetry(embody::Client::Telemetry& t);
    void update_light();
    void update_ir();
    void update_power_events();
    void queue_event(const char* name, embody::Client::Telemetry data = {}, embody::Client::Texts text = {});
    static void on_screen_event(lv_event_t* e);
    void pause_angle_sync();
    void start_gesture(const GestureStep* steps, size_t count);
    void update_motion();
    void send_camera_frame();
    void start_mic();
    void stop_mic();
    static void mic_task(void* arg);
    void send_mic_audio();
    void queue_speaker_audio(const std::string& payload);
    void stop_speaker();
    static void speaker_task(void* arg);
    void run_leds(const ArduinoJson::JsonDocument& args);
    void update_leds();
    void stop_led_effect(bool restore_sides);
    void start_nfc();
    void stop_nfc();
    static void nfc_task(void* arg);
};
