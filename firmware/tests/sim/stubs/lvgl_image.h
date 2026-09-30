// Host stand-in for xiaozhi's lvgl_image.h (tests/sim): the allocated image class.
#pragma once
#include <lvgl.h>
#include <cstdlib>
#include <cstring>

class LvglImage {
public:
    virtual ~LvglImage() = default;
    virtual const lv_image_dsc_t* image_dsc() const = 0;
};

class LvglAllocatedImage : public LvglImage {
public:
    LvglAllocatedImage(void* data, size_t size, int width, int height, int stride, int color_format)
    {
        std::memset(&dsc_, 0, sizeof(dsc_));
        dsc_.data_size     = size;
        dsc_.data          = static_cast<uint8_t*>(data);
        dsc_.header.magic  = LV_IMAGE_HEADER_MAGIC;
        dsc_.header.cf     = color_format;
        dsc_.header.w      = width;
        dsc_.header.h      = height;
        dsc_.header.stride = stride;
    }
    ~LvglAllocatedImage() override { std::free((void*)dsc_.data); }
    const lv_image_dsc_t* image_dsc() const override { return &dsc_; }

private:
    lv_image_dsc_t dsc_;
};
