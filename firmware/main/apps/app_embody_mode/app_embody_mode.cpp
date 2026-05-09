/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_embody_mode.h"
#include <apps/common/common.h>
#include <assets/assets.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>

using namespace mooncake;
using namespace smooth_ui_toolkit::lvgl_cpp;

AppEmbodyMode::AppEmbodyMode()
{
    setAppInfo().name = "Embody Mode";
    static auto icon  = assets::get_image("icon_setup.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = 0x7D8CFF;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppEmbodyMode::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppEmbodyMode::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _status_text              = "Tap a card";
    _last_rendered_status_text.clear();

    LvglLockGuard lock;

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setSize(320, 240);
    _panel->setBgColor(lv_color_hex(0xF4F6FF));
    _panel->setBorderWidth(0);
    _panel->setRadius(0);

    _title = std::make_unique<Label>(*_panel);
    _title->setText("Embody Mode");
    _title->setTextFont(&lv_font_montserrat_24);
    _title->setTextColor(lv_color_hex(0x1E2355));
    _title->align(LV_ALIGN_TOP_MID, 0, 18);

    _subtitle = std::make_unique<Label>(*_panel);
    _subtitle->setText("POC screen between AI.AGENT and AVATAR");
    _subtitle->setTextFont(&lv_font_montserrat_16);
    _subtitle->setTextColor(lv_color_hex(0x5A618E));
    _subtitle->setWidth(280);
    _subtitle->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _subtitle->setLongMode(LV_LABEL_LONG_MODE_WRAP);
    _subtitle->align(LV_ALIGN_TOP_MID, 0, 52);

    _ai_agent_button = std::make_unique<Button>(*_panel);
    _ai_agent_button->setSize(258, 52);
    _ai_agent_button->align(LV_ALIGN_TOP_MID, 0, 96);
    _ai_agent_button->setBgColor(lv_color_hex(0xDCE7FF));
    _ai_agent_button->setBorderWidth(0);
    _ai_agent_button->setShadowWidth(0);
    _ai_agent_button->setRadius(18);
    _ai_agent_button->label().setText("AI.AGENT");
    _ai_agent_button->label().setTextFont(&lv_font_montserrat_20);
    _ai_agent_button->label().setTextColor(lv_color_hex(0x16204A));
    _ai_agent_button->label().setWidth(230);
    _ai_agent_button->label().setTextAlign(LV_TEXT_ALIGN_CENTER);
    _ai_agent_button->label().setLongMode(LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    _ai_agent_button->onClick().connect([this]() {
        _status_text = "Preview card: AI.AGENT";
        mclog::tagInfo(getAppInfo().name, "selected AI.AGENT");
    });

    _avatar_button = std::make_unique<Button>(*_panel);
    _avatar_button->setSize(258, 52);
    _avatar_button->align(LV_ALIGN_TOP_MID, 0, 158);
    _avatar_button->setBgColor(lv_color_hex(0xFFE2EB));
    _avatar_button->setBorderWidth(0);
    _avatar_button->setShadowWidth(0);
    _avatar_button->setRadius(18);
    _avatar_button->label().setText("AVATAR");
    _avatar_button->label().setTextFont(&lv_font_montserrat_20);
    _avatar_button->label().setTextColor(lv_color_hex(0x4A1A2C));
    _avatar_button->label().setWidth(230);
    _avatar_button->label().setTextAlign(LV_TEXT_ALIGN_CENTER);
    _avatar_button->label().setLongMode(LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    _avatar_button->onClick().connect([this]() {
        _status_text = "Preview card: AVATAR";
        mclog::tagInfo(getAppInfo().name, "selected AVATAR");
    });

    _status = std::make_unique<Label>(*_panel);
    _status->setText(_status_text);
    _status->setTextFont(&lv_font_montserrat_16);
    _status->setTextColor(lv_color_hex(0x384066));
    _status->setWidth(280);
    _status->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _status->setLongMode(LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    _status->align(LV_ALIGN_BOTTOM_MID, 0, -20);

    view::create_home_indicator([&]() { close(); }, 0x7D8CFF, 0x1E2355);
    view::create_status_bar(0x7D8CFF, 0x1E2355);
}

void AppEmbodyMode::onRunning()
{
    LvglLockGuard lock;

    if (_status && _status_text != _last_rendered_status_text) {
        _status->setText(_status_text);
        _last_rendered_status_text = _status_text;
    }

    view::update_home_indicator();
    view::update_status_bar();
}

void AppEmbodyMode::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    LvglLockGuard lock;

    _panel.reset();
    _title.reset();
    _subtitle.reset();
    _status.reset();
    _ai_agent_button.reset();
    _avatar_button.reset();

    view::destroy_home_indicator();
    view::destroy_status_bar();
}