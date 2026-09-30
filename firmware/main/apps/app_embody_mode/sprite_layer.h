/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <lvgl.h>
#include <ArduinoJson.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

class LvglAllocatedImage;

namespace embody {

class AssetStore;

// Pictures from the file store placed over the face: transparent PNGs (or
// JPEGs), each with an id, a center position, scale, rotation, opacity and a
// stacking order, moved smoothly by LVGL animations. Taps go through them.
//
// Files are decoded once (PNG to ARGB8888, JPEG to RGB565) into PSRAM and
// cached by name; load() runs outside the LVGL lock, everything else inside.
class SpriteLayer {
public:
    using Image = std::shared_ptr<LvglAllocatedImage>;

    void create(lv_obj_t* parent);
    void destroy();

    // Decodes a stored file into the cache (outside the LVGL lock). "" or an error.
    std::string load(AssetStore& store, const std::string& name);
    Image cached(const std::string& name);
    // A file changed or was deleted: decode it again next time.
    void forget(const std::string& name);

    // "sprite" args: {"id", "asset", "x", "y", "scale", "angle", "opacity", "z", "hidden", "ms", "tap"}.
    // Returns "" or an error. Under the LVGL lock.
    std::string set(const ArduinoJson::JsonDocument& args);
    void hide(const std::string& id);
    void clear();

    // What a tap at screen point (x, y) hit: the topmost visible sprite marked "tap"
    // whose pixel there is not transparent; the point in the picture's own pixels
    // (scale undone, rotation ignored). False if none. Under the LVGL lock.
    struct Hit {
        std::string id, asset;
        int x = 0, y = 0;
    };
    bool hit(int x, int y, Hit& out) const;

private:
    struct Sprite {
        lv_obj_t* obj = nullptr;
        std::string asset;
        Image image;
        int x = 160, y = 120, z = 0;
        float scale = 1;
        bool tap    = false;  // reported by hit()
        uint32_t order = 0;  // creation order: ties in z keep it
    };
    struct Cached {
        Image image;
        size_t bytes     = 0;
        uint32_t last_use = 0;
    };

    lv_obj_t* _layer = nullptr;
    std::map<std::string, Sprite> _sprites;
    std::map<std::string, Cached> _cache;
    size_t _cache_bytes  = 0;
    uint32_t _use_clock  = 0;
    uint32_t _next_order = 0;

    void place(Sprite& s, int ms);
    void restack();
    void trim_cache();
};

}  // namespace embody
