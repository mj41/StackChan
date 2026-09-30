/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * Embody Mode sprites on the host (tests/sim): the real SpriteLayer and the real LVGL
 * (configured as on the robot) render into memory; the test checks pixels.
 * SNAPSHOT_DIR=/tmp/x also writes each checked screen as a PPM to look at.
 */
#include <apps/app_embody_mode/asset_store.h>
#include <apps/app_embody_mode/sprite_layer.h>
#include <lvgl.h>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

uint32_t g_tick = 0;
int g_failures  = 0;

void flush(lv_display_t* disp, const lv_area_t*, uint8_t*)
{
    lv_display_flush_ready(disp);
}

struct Rgb {
    int r, g, b;
};

// Screen is a rendered snapshot of the screen (ARGB8888).
struct Screen {
    lv_draw_buf_t* buf = nullptr;
    explicit Screen(const char* name)
    {
        lv_refr_now(nullptr);
        buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
        if (const char* dir = std::getenv("SNAPSHOT_DIR"); dir && buf) {
            FILE* f = std::fopen((std::string(dir) + "/" + name + ".ppm").c_str(), "wb");
            std::fprintf(f, "P6 %d %d 255\n", (int)buf->header.w, (int)buf->header.h);
            for (int y = 0; y < (int)buf->header.h; y++) {
                for (int x = 0; x < (int)buf->header.w; x++) {
                    const Rgb c = at(x, y);
                    std::fputc(c.r, f), std::fputc(c.g, f), std::fputc(c.b, f);
                }
            }
            std::fclose(f);
        }
    }
    ~Screen() { lv_draw_buf_destroy(buf); }
    Rgb at(int x, int y) const
    {
        const uint8_t* p = buf->data + y * buf->header.stride + x * 4;  // B, G, R, A
        return {p[2], p[1], p[0]};
    }
    // centerX is the middle of the red pixels in row y (-1 if none).
    int centerX(int y) const
    {
        int first = -1, last = -1;
        for (int x = 0; x < (int)buf->header.w; x++) {
            if (isRed(at(x, y))) {
                last = x;
                if (first < 0) first = x;
            }
        }
        return first < 0 ? -1 : (first + last) / 2;
    }
    static bool isRed(Rgb c) { return c.r > 200 && c.g < 40 && c.b < 40; }
    static bool isBlue(Rgb c) { return c.b > 200 && c.r < 40 && c.g < 40; }
};

void expect(bool ok, const std::string& what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        g_failures++;
    }
}

void set(embody::SpriteLayer& layer, const char* json)
{
    ArduinoJson::JsonDocument args;
    ArduinoJson::deserializeJson(args, json);
    const std::string err = layer.set(args);
    expect(err.empty(), std::string("sprite ") + json + ": " + err);
}

void advance(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        g_tick += 10;
        lv_timer_handler();
    }
}

}  // namespace

int main()
{
    lv_init();
    lv_tick_set_cb([]() { return g_tick; });
    lv_display_t* disp = lv_display_create(320, 240);
    static uint8_t draw_buf[320 * 240 * 2];
    lv_display_set_buffers(disp, draw_buf, nullptr, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x0000FF), 0);  // blue: shows through transparency

    embody::AssetStore store;
    embody::SpriteLayer layer;
    layer.create(lv_screen_active());

    // A 32x32 PNG: left half opaque red, right half transparent.
    expect(layer.load(store, "red_half.png").empty(), "load red_half.png");
    set(layer, R"({"id": "a", "asset": "red_half.png", "x": 160, "y": 120})");
    {
        Screen s("placed");
        expect(Screen::isRed(s.at(150, 120)), "the opaque half is red at its place");
        expect(Screen::isBlue(s.at(170, 120)), "the transparent half shows the background");
        expect(Screen::isBlue(s.at(20, 20)), "the rest is background");
    }

    // A glide: halfway in time is about halfway in space (the ease is symmetric).
    set(layer, R"({"id": "a", "x": 260, "y": 120, "ms": 1000})");
    advance(500);
    {
        Screen s("glide_half");
        const int x = s.centerX(120);  // the red half's middle: sprite center - 8
        expect(x > 190 && x < 222, "halfway through a glide from 160 to 260: red at " + std::to_string(x));
    }
    advance(600);
    {
        Screen s("glide_end");
        const int x = s.centerX(120);
        expect(x > 245 && x < 258, "after the glide: red at " + std::to_string(x));
    }

    // Created and glided in one go (as the pet's food does): starts where it was placed.
    set(layer, R"({"id": "b", "asset": "red_half.png", "x": 60, "y": 30})");
    set(layer, R"({"id": "b", "x": 60, "y": 210, "ms": 1000})");
    advance(100);
    {
        Screen s("new_then_glide");
        expect(s.centerX(40) > 40 && s.centerX(40) < 70, "a new sprite glides from where it was placed, not from (0,0)");
    }

    // Scaled sprites still draw the picture (no garbage).
    set(layer, R"({"id": "c", "asset": "red_half.png", "x": 160, "y": 200, "scale": 2})");
    advance(20);
    {
        Screen s("scaled");
        expect(Screen::isRed(s.at(140, 200)), "a scaled sprite is red where its opaque half is");
    }

    // Taps: the topmost sprite marked "tap", where its picture is not transparent.
    set(layer, R"({"id": "btn", "asset": "red_half.png", "x": 160, "y": 60, "scale": 2, "tap": true, "z": 9})");
    {
        embody::SpriteLayer::Hit hit;
        expect(layer.hit(150, 60, hit) && hit.id == "btn" && hit.asset == "red_half.png",
               "a tap on the opaque half hits the button");
        expect(hit.x == 11 && hit.y == 16, "the tap in the picture's pixels (scale 2): " + std::to_string(hit.x) + "," +
                                               std::to_string(hit.y));
        expect(!layer.hit(170, 60, hit), "a tap on the transparent half hits nothing");
        expect(!layer.hit(150, 200, hit), "a sprite without tap is not a button");
    }

    layer.hide("a");
    layer.clear();
    {
        Screen s("cleared");
        expect(Screen::isBlue(s.at(150, 120)) && Screen::isBlue(s.at(140, 200)), "cleared sprites are gone");
    }
    layer.destroy();

    if (g_failures) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "sprite_layer_test: ok\n";
    return 0;
}
