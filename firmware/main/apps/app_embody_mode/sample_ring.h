/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <esp_heap_caps.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace embody {

// A ring of audio samples in PSRAM: one allocation, sized once. When full, the oldest
// samples are dropped (the newest audio wins). Not thread-safe: the caller locks.
//
// It replaces a std::deque<int16_t>: a deque allocates 512-byte blocks, and blocks that
// small always come from the scarce internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL),
// tens of KB of it for a second of audio.
class SampleRing {
public:
    SampleRing() = default;
    SampleRing(const SampleRing&) = delete;
    SampleRing& operator=(const SampleRing&) = delete;
    ~SampleRing() { heap_caps_free(_buf); }

    // reserve sets the capacity (samples); a different size drops what is queued.
    bool reserve(size_t capacity)
    {
        if (capacity == _cap && _buf) {
            return true;
        }
        heap_caps_free(_buf);
        _buf  = (int16_t*)heap_caps_malloc(capacity * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        _cap  = _buf ? capacity : 0;
        _head = _size = 0;
        return _buf != nullptr;
    }

    size_t size() const { return _size; }
    size_t capacity() const { return _cap; }
    bool empty() const { return _size == 0; }
    void clear() { _head = _size = 0; }

    // push adds a sample; when full the oldest is dropped.
    void push(int16_t s)
    {
        if (_cap == 0) {
            return;
        }
        _buf[(_head + _size) % _cap] = s;
        if (_size < _cap) {
            _size++;
        } else {
            _head = (_head + 1) % _cap;
        }
    }

    // pop moves up to n of the oldest samples to out; returns how many.
    size_t pop(int16_t* out, size_t n)
    {
        n = std::min(n, _size);
        for (size_t i = 0; i < n; i++) {
            out[i] = _buf[(_head + i) % _cap];
        }
        _head = _cap ? (_head + n) % _cap : 0;
        _size -= n;
        return n;
    }

private:
    int16_t* _buf = nullptr;
    size_t _cap   = 0;
    size_t _head  = 0;  // the oldest sample
    size_t _size  = 0;
};

}  // namespace embody
