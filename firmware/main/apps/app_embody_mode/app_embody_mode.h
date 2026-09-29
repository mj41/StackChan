/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "embody_client.h"
#include <mooncake.h>
#include <lvgl.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
 * watches, streams telemetry and reports robot events.
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

    bool _qr_visible = true;
    int _rendered_viewers = 0;
    std::atomic<bool> _toggle_qr_requested{false};  // set from LVGL event callbacks
    uint32_t _rendered_revision = UINT32_MAX;
    std::string _rendered_url;
    std::string _last_command;

    // Commands arrive during _client->update(), outside the LVGL lock, and run
    // afterwards under it.
    std::vector<std::pair<std::string, std::string>> _pending_commands;

    // Robot events come from HAL tasks (IMU, head touch) and LVGL callbacks (taps).
    std::mutex _event_mutex;
    std::vector<std::pair<std::string, embody::Client::Telemetry>> _pending_events;
    size_t _imu_connection = 0;
    size_t _head_connection = 0;

    // Camera: frames are captured and JPEG-encoded in the app loop while on.
    bool _camera_on           = false;
    uint32_t _last_frame_tick = 0;

    // Microphone: a task reads the codec into _mic_samples; the app loop sends them.
    std::atomic<bool> _mic_running{false};
    std::atomic<TaskHandle_t> _mic_task{nullptr};  // cleared by the task when it exits
    std::mutex _mic_mutex;
    std::vector<int16_t> _mic_samples;
    int _mic_rate = 0;

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
    void enter_blank(bool manual);
    void leave_blank();
    void wake_screen();
    void run_command(const std::string& command, const std::string& args);
    void queue_event(const char* name, embody::Client::Telemetry data = {});
    static void on_screen_event(lv_event_t* e);
    void pause_angle_sync();
    void start_gesture(const GestureStep* steps, size_t count);
    void update_motion();
    void send_camera_frame();
    void start_mic();
    void stop_mic();
    static void mic_task(void* arg);
    void send_mic_audio();
};
