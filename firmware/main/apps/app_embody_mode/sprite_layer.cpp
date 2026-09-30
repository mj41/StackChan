/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "sprite_layer.h"
#include "asset_store.h"
#include <esp_heap_caps.h>
#include <hal/utils/jpeg_to_image/jpeg_decoder.h>
#include <lvgl_image.h>
#include <mooncake_log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

// LVGL's LodePNG (LV_USE_LODEPNG), compiled as C. Its header turns on C++ overloads
// that clash when included from C++, so only the two functions used are declared.
extern "C" {
unsigned lodepng_decode32(unsigned char** out, unsigned* w, unsigned* h, const unsigned char* in, size_t insize);
const char* lodepng_error_text(unsigned code);
}

namespace embody {

static const char* _tag                  = "Sprites";
static constexpr size_t _max_sprites     = 24;
static constexpr size_t _max_id          = 16;
static constexpr size_t _max_file        = 1 << 20;   // bigger files are not pictures for the screen
static constexpr size_t _max_pixels      = 640 * 480;  // decoded size limit
static constexpr size_t _cache_limit     = 3 << 20;    // decoded pictures kept in PSRAM

void SpriteLayer::create(lv_obj_t* parent)
{
    _layer = lv_obj_create(parent);
    lv_obj_remove_style_all(_layer);
    lv_obj_set_size(_layer, 320, 240);
    lv_obj_align(_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(_layer, LV_OBJ_FLAG_CLICKABLE);  // taps reach the face and pictures below
    lv_obj_remove_flag(_layer, LV_OBJ_FLAG_SCROLLABLE);
}

void SpriteLayer::destroy()
{
    clear();
    if (_layer) {
        lv_obj_delete(_layer);
        _layer = nullptr;
    }
    _cache.clear();
    _cache_bytes = 0;
}

// read_file reads a whole stored file into PSRAM-friendly memory.
static bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = size > 0 && (size_t)size <= _max_file;
    if (ok) {
        out.resize(size);
        ok = fread(out.data(), 1, size, f) == (size_t)size;
    }
    fclose(f);
    return ok;
}

// decode_png turns a PNG into LVGL ARGB8888 (B, G, R, A in memory) in PSRAM.
// LVGL's LodePNG is modified: its "out" is an lv_draw_buf_t holding RGBA rows
// (LVGL's own decoder swaps red and blue the same way).
static SpriteLayer::Image decode_png(const std::vector<uint8_t>& png, std::string& error)
{
    lv_draw_buf_t* buf = nullptr;
    unsigned w = 0, h = 0;
    if (unsigned err = lodepng_decode32((unsigned char**)&buf, &w, &h, png.data(), png.size())) {
        if (buf) {
            lv_draw_buf_destroy(buf);
        }
        error = std::string("PNG: ") + lodepng_error_text(err);
        return nullptr;
    }
    if (!buf || (size_t)w * h > _max_pixels) {
        if (buf) {
            lv_draw_buf_destroy(buf);
        }
        error = "picture too big (max 640x480)";
        return nullptr;
    }
    const size_t bytes = (size_t)w * h * 4;
    auto* argb         = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!argb) {
        lv_draw_buf_destroy(buf);
        error = "out of memory";
        return nullptr;
    }
    for (unsigned y = 0; y < h; y++) {
        const uint8_t* src = buf->data + (size_t)y * buf->header.stride;
        uint8_t* dst       = argb + (size_t)y * w * 4;
        for (unsigned x = 0; x < w; x++, src += 4, dst += 4) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
        }
    }
    lv_draw_buf_destroy(buf);
    return std::make_shared<LvglAllocatedImage>(argb, bytes, (int)w, (int)h, (int)w * 4, LV_COLOR_FORMAT_ARGB8888);
}

std::string SpriteLayer::load(AssetStore& store, const std::string& name)
{
    if (auto it = _cache.find(name); it != _cache.end()) {
        it->second.last_use = ++_use_clock;
        return "";
    }
    const std::string path = store.path(name);
    std::vector<uint8_t> data;
    if (path.empty() || !read_file(path, data)) {
        return "no such file";
    }
    std::string error;
    Image image;
    static const uint8_t png_magic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (data.size() > 8 && std::memcmp(data.data(), png_magic, 8) == 0) {
        image = decode_png(data, error);
    } else if (data.size() > 2 && data[0] == 0xFF && data[1] == 0xD8) {
        image = jpeg_dec::decode_to_lvgl(data.data(), data.size());
        if (!image) {
            error = "JPEG decode failed";
        }
    } else {
        error = "not a PNG or JPEG";
    }
    if (!image) {
        return error;
    }
    const size_t bytes = image->image_dsc()->data_size;
    _cache[name]       = {image, bytes, ++_use_clock};
    _cache_bytes += bytes;
    trim_cache();
    mclog::tagInfo(_tag, "decoded {} ({}x{})", name, image->image_dsc()->header.w, image->image_dsc()->header.h);
    return "";
}

SpriteLayer::Image SpriteLayer::cached(const std::string& name)
{
    auto it = _cache.find(name);
    if (it == _cache.end()) {
        return nullptr;
    }
    it->second.last_use = ++_use_clock;
    return it->second.image;
}

void SpriteLayer::forget(const std::string& name)
{
    if (auto it = _cache.find(name); it != _cache.end()) {
        _cache_bytes -= it->second.bytes;
        _cache.erase(it);  // a sprite showing it keeps its own reference until it changes
    }
}

// trim_cache drops the least recently used pictures over the limit (shown ones stay alive
// through their sprite's reference).
void SpriteLayer::trim_cache()
{
    while (_cache_bytes > _cache_limit && _cache.size() > 1) {
        auto oldest = _cache.begin();
        for (auto it = _cache.begin(); it != _cache.end(); ++it) {
            if (it->second.last_use < oldest->second.last_use) {
                oldest = it;
            }
        }
        _cache_bytes -= oldest->second.bytes;
        _cache.erase(oldest);
    }
}

void SpriteLayer::place(Sprite& s, int ms)
{
    const auto* dsc = s.image ? s.image->image_dsc() : nullptr;
    const int w = dsc ? dsc->header.w : 0, h = dsc ? dsc->header.h : 0;
    const int tx = s.x - w / 2, ty = s.y - h / 2;  // the center is at (x, y); scale and rotation keep it
    lv_anim_delete(s.obj, nullptr);
    if (ms <= 0) {
        lv_obj_set_pos(s.obj, tx, ty);
        return;
    }
    auto animate = [&](int from, int to, lv_anim_exec_xcb_t exec) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s.obj);
        lv_anim_set_values(&a, from, to);
        lv_anim_set_duration(&a, ms);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, exec);
        lv_anim_start(&a);
    };
    // Coordinates are only updated by a layout pass: without it a sprite placed in the same
    // batch (new, then glide) would start from where LVGL last drew it, e.g. (0, 0).
    lv_obj_update_layout(s.obj);
    animate(lv_obj_get_x(s.obj), tx, (lv_anim_exec_xcb_t)lv_obj_set_x);
    animate(lv_obj_get_y(s.obj), ty, (lv_anim_exec_xcb_t)lv_obj_set_y);
}

void SpriteLayer::restack()
{
    std::vector<Sprite*> order;
    for (auto& [id, s] : _sprites) {
        order.push_back(&s);
    }
    std::sort(order.begin(), order.end(),
              [](const Sprite* a, const Sprite* b) { return a->z != b->z ? a->z < b->z : a->order < b->order; });
    for (size_t i = 0; i < order.size(); i++) {
        lv_obj_move_to_index(order[i]->obj, (int32_t)i);
    }
}

std::string SpriteLayer::set(const ArduinoJson::JsonDocument& args)
{
    if (!_layer) {
        return "no screen";
    }
    const std::string id = args["id"] | "";
    if (id.empty() || id.size() > _max_id) {
        return "id: 1-16 characters";
    }
    auto it        = _sprites.find(id);
    const bool fresh = it == _sprites.end();
    if (fresh && _sprites.size() >= _max_sprites) {
        return "too many sprites (24)";
    }
    const std::string asset = args["asset"] | "";
    if (fresh && asset.empty()) {
        return "a new sprite needs an asset";
    }
    Image image;
    if (!asset.empty()) {
        image = cached(asset);
        if (!image) {
            return "cannot show " + asset;  // load() reported why
        }
    }
    if (fresh) {
        Sprite s;
        s.obj   = lv_image_create(_layer);
        s.order = _next_order++;
        lv_obj_remove_flag(s.obj, LV_OBJ_FLAG_CLICKABLE);
        it = _sprites.emplace(id, std::move(s)).first;
    }
    Sprite& s = it->second;
    if (image && (image != s.image || asset != s.asset)) {
        s.image = image;
        s.asset = asset;
        lv_image_set_src(s.obj, s.image->image_dsc());
        const auto* dsc = s.image->image_dsc();
        lv_obj_set_size(s.obj, dsc->header.w, dsc->header.h);
        lv_image_set_pivot(s.obj, dsc->header.w / 2, dsc->header.h / 2);
    }
    if (args["scale"].is<float>()) {
        const uint32_t scale = (uint32_t)std::clamp(args["scale"].as<float>() * 256.0f, 26.0f, 2048.0f);
        lv_image_set_scale(s.obj, scale);
        s.scale = scale / 256.0f;
    }
    if (args["tap"].is<bool>()) {
        s.tap = args["tap"].as<bool>();
    }
    if (args["angle"].is<float>()) {
        lv_image_set_rotation(s.obj, (int32_t)(args["angle"].as<float>() * 10));
    }
    if (args["opacity"].is<float>()) {
        lv_obj_set_style_opa(s.obj, (lv_opa_t)std::clamp(args["opacity"].as<float>() * 255.0f, 0.0f, 255.0f), 0);
    }
    if (args["hidden"].is<bool>()) {
        args["hidden"].as<bool>() ? lv_obj_add_flag(s.obj, LV_OBJ_FLAG_HIDDEN) : lv_obj_remove_flag(s.obj, LV_OBJ_FLAG_HIDDEN);
    }
    const bool moved = args["x"].is<int>() || args["y"].is<int>();
    s.x              = args["x"] | s.x;
    s.y              = args["y"] | s.y;
    if (moved || fresh || image) {
        place(s, fresh ? 0 : (int)std::clamp(args["ms"] | 0, 0, 60000));
    }
    if (args["z"].is<int>() || fresh) {
        s.z = args["z"] | s.z;
        restack();
    }
    return "";
}

bool SpriteLayer::hit(int x, int y, Hit& out) const
{
    const Sprite* best = nullptr;
    int bx = 0, by = 0;
    for (const auto& [id, s] : _sprites) {
        if (!s.tap || !s.image || lv_obj_has_flag(s.obj, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_style_opa(s.obj, LV_PART_MAIN) == 0) {
            continue;
        }
        const auto* dsc = s.image->image_dsc();
        const int w = dsc->header.w, h = dsc->header.h;
        // The picture is drawn scaled around its center (s.x, s.y).
        const int px = (int)((x - s.x) / s.scale + w / 2.0f), py = (int)((y - s.y) / s.scale + h / 2.0f);
        if (px < 0 || py < 0 || px >= w || py >= h) {
            continue;
        }
        if (dsc->header.cf == LV_COLOR_FORMAT_ARGB8888 && dsc->data[py * dsc->header.stride + px * 4 + 3] < 64) {
            continue;  // a transparent part: not the button
        }
        if (!best || s.z > best->z || (s.z == best->z && s.order > best->order)) {
            best = &s;
            bx = px, by = py;
            out.id = id;
        }
    }
    if (!best) {
        return false;
    }
    out.asset = best->asset;
    out.x     = bx;
    out.y     = by;
    return true;
}

void SpriteLayer::hide(const std::string& id)
{
    auto it = _sprites.find(id);
    if (it == _sprites.end()) {
        return;
    }
    lv_anim_delete(it->second.obj, nullptr);
    lv_obj_delete(it->second.obj);
    _sprites.erase(it);
}

void SpriteLayer::clear()
{
    for (auto& [id, s] : _sprites) {
        lv_anim_delete(s.obj, nullptr);
        lv_obj_delete(s.obj);
    }
    _sprites.clear();
}

}  // namespace embody
