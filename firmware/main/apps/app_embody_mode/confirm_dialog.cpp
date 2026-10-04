/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#include "confirm_dialog.h"
#include <hal/hal.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <memory>

namespace {

struct Dialog;

struct Button {
    Dialog* dialog;
    bool yes;
};

struct Dialog {
    lv_obj_t* box      = nullptr;
    lv_timer_t* timer  = nullptr;
    bool finished      = false;
    std::function<void(bool)> done;
    Button yes{nullptr, true};
    Button no{nullptr, false};
};

void finish(Dialog* d, bool answer)
{
    if (d->finished) {
        return;
    }
    d->finished = true;
    if (d->timer) {
        lv_timer_delete(d->timer);
        d->timer = nullptr;
    }
    auto done = std::move(d->done);
    lv_obj_delete_async(d->box);  // frees the dialog (LV_EVENT_DELETE below)
    if (done) {
        done(answer);
    }
}

void on_button(lv_event_t* e)
{
    auto* b = static_cast<Button*>(lv_event_get_user_data(e));
    finish(b->dialog, b->yes);
}

void on_timeout(lv_timer_t* t)
{
    auto* d  = static_cast<Dialog*>(lv_timer_get_user_data(t));
    d->timer = nullptr;  // LVGL deletes a one-shot timer itself
    finish(d, false);
}

void on_delete(lv_event_t* e)
{
    delete static_cast<Dialog*>(lv_event_get_user_data(e));
}

}  // namespace

void embody::askOnScreen(const std::string& question, const std::string& detail, int seconds, std::function<void(bool)> done)
{
    auto* d   = new Dialog;
    d->done   = std::move(done);
    d->yes.dialog = d;
    d->no.dialog  = d;

    d->box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(d->box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(d->box, lv_color_hex(0x1E2355), 0);
    lv_obj_set_style_bg_opa(d->box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d->box, 0, 0);
    lv_obj_set_style_radius(d->box, 0, 0);
    lv_obj_add_flag(d->box, LV_OBJ_FLAG_CLICKABLE);  // nothing below reacts meanwhile
    lv_obj_remove_flag(d->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(d->box, on_delete, LV_EVENT_DELETE, d);

    lv_obj_t* q = lv_label_create(d->box);
    lv_label_set_text(q, question.c_str());
    lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(q, LV_PCT(100));
    lv_obj_set_style_text_font(q, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(q, lv_color_white(), 0);
    lv_obj_align(q, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_t* t = lv_label_create(d->box);
    lv_label_set_text(t, detail.c_str());
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, LV_PCT(100));
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xA3A8CC), 0);
    lv_obj_align_to(t, q, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

    struct {
        const char* text;
        uint32_t color;
        Button* button;
        lv_align_t align;
    } buttons[] = {{"Yes", 0x2E9E5B, &d->yes, LV_ALIGN_BOTTOM_LEFT}, {"No", 0xC0392B, &d->no, LV_ALIGN_BOTTOM_RIGHT}};
    for (const auto& bt : buttons) {
        lv_obj_t* b = lv_button_create(d->box);
        lv_obj_set_size(b, 130, 60);
        lv_obj_set_style_bg_color(b, lv_color_hex(bt.color), 0);
        lv_obj_align(b, bt.align, 0, 0);
        lv_obj_add_event_cb(b, on_button, LV_EVENT_CLICKED, bt.button);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, bt.text);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_center(l);
    }

    d->timer = lv_timer_create(on_timeout, (uint32_t)seconds * 1000u, d);
    lv_timer_set_repeat_count(d->timer, 1);
}

bool embody::askOnScreenAndWait(const std::string& question, const std::string& detail, int seconds)
{
    auto answer = std::make_shared<std::atomic<int>>(0);
    {
        LvglLockGuard lock;
        askOnScreen(question, detail, seconds, [answer](bool yes) { *answer = yes ? 1 : -1; });
    }
    while (*answer == 0) {  // the dialog's own timer ends it
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return *answer == 1;
}
