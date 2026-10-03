/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_launcher.h"
#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <apps/app_embody_mode/automation.h>
#include <strings.h>
#include <cstdint>

using namespace mooncake;

void AppLauncher::onLauncherCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");

    // 打开自己
    open();
}

void AppLauncher::onLauncherOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    LvglLockGuard lock;

    if (!_startup_checked && !GetHAL().isAppConfiged()) {
        mclog::tagInfo(getAppInfo().name, "app not configured, start startup worker");
        _startup_worker = std::make_unique<setup_workers::StartupWorker>();
    } else {
        create_launcher_view();
    }
}

void AppLauncher::onLauncherRunning()
{
    LvglLockGuard lock;

    if (_startup_worker) {
        _startup_worker->update();
        if (_startup_worker->isDone()) {
            _startup_worker.reset();
            _startup_checked = true;
            create_launcher_view();
        }
    } else {
        if (_boot_app_id >= 0) {
            const int id = _boot_app_id;
            _boot_app_id = -1;
            openApp(id);
            return;
        }
        _view->update();
        screensaver_update();
    }

    GetStackChan().update();
}

void AppLauncher::onLauncherClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    LvglLockGuard lock;

    _view.reset();
}

void AppLauncher::onLauncherDestroy()
{
    mclog::tagInfo(getAppInfo().name, "on close");
}

// Optional automation (CONFIG_STACKCHAN_EMBODY_AUTOMATION): the app to open by itself
// after a boot. Must run before the view reads and clears the warm-reboot target.
void AppLauncher::check_boot_app()
{
    if (_boot_app_checked) {
        return;
    }
    _boot_app_checked   = true;
    const auto boot_app = embody::take_boot_app(GetHAL().getWarmRebootTarget() >= 0);
    if (boot_app.empty()) {
        return;
    }
    for (const auto& props : getAppProps()) {
        if (strcasecmp(props.info.name.c_str(), boot_app.c_str()) == 0) {
            mclog::tagInfo(getAppInfo().name, "automation: opening {}", props.info.name);
            _boot_app_id = props.appID;
            return;
        }
    }
    mclog::tagWarn(getAppInfo().name, "automation: no app named {}", boot_app);
}

void AppLauncher::create_launcher_view()
{
    check_boot_app();
    _view = std::make_unique<view::LauncherView>();
    _view->init(getAppProps());
    _view->onAppClicked = [&](int appID) {
        mclog::tagInfo(getAppInfo().name, "handle open app, app id: {}", appID);
        openApp(appID);
    };
}

void AppLauncher::screensaver_update()
{
    const uint32_t SCREENSAVER_TIMEOUT_MS = 30000;

    uint32_t idle_time = lv_display_get_inactive_time(NULL);
    if (idle_time >= SCREENSAVER_TIMEOUT_MS) {
        if (!_screensaver) {
            _screensaver = std::make_unique<view::Screensaver>();
            _screensaver->init();
        }
    } else if (_screensaver) {
        _screensaver.reset();
    }

    // Update in 30ms interval
    if (_screensaver && GetHAL().millis() - _screensaver_timecount > 30) {
        _screensaver_timecount = GetHAL().millis();
        _screensaver->update();
    }
}
