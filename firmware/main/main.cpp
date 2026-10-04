/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <apps/app_embody_mode/usb_setup.h>
#include <apps/app_embody_mode/automation.h>
#include <hal/hal.h>
#include <sdkconfig.h>

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    // Setup over the USB cable (chan.w42.eu/setup, s-w42-eu-usb): server, token, Wi-Fi.
    embody::note_boot();  // before the launcher reads the boot app
    embody::startUsbSetup();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

#ifdef CONFIG_STACKCHAN_EMBODY_ONLY
    // Embody-only: no AI.AGENT, so its start-on-boot setting is ignored.
    const bool skip_mooncake = false;
#else
    const bool skip_mooncake =
        GetHAL().getXiaozhiConfig().startAiAgentOnBoot && GetHAL().getWarmRebootTarget() < 0;
#endif

    if (!skip_mooncake) {
        // Install apps
        GetMooncake().installApp(std::make_unique<AppLauncher>());
        GetMooncake().installApp(std::make_unique<AppEmbodyMode>());
#ifndef CONFIG_STACKCHAN_EMBODY_ONLY
        GetMooncake().installApp(std::make_unique<AppAiAgent>());
        GetMooncake().installApp(std::make_unique<AppAvatar>());
        GetMooncake().installApp(std::make_unique<AppEspnowControl>());
        GetMooncake().installApp(std::make_unique<AppAppCenter>());
        GetMooncake().installApp(std::make_unique<AppEzdata>());
        GetMooncake().installApp(std::make_unique<AppDance>());
#endif
        // SETUP returns with requestWarmReboot(7); the launcher clamps that to the
        // last app, so it stays correct in the Embody-only list too.
        GetMooncake().installApp(std::make_unique<AppSetup>());

        // Main loop
        while (1) {
            GetHAL().feedTheDog();
            GetHAL().updateHeapStatusLog();

            GetMooncake().update();

            if (GetHAL().isXiaozhiStartRequested()) {
                break;
            }
        }

        // Uninstall all apps and destroy mooncake
        GetMooncake().uninstallAllApps();
        DestroyMooncake();
    }

    // Start xiaozhi, never returns
    GetHAL().startXiaozhi();
}
