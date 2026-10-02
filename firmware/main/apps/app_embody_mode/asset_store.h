/*
 * SPDX-FileCopyrightText: 2026 Michal Jurosz (mj41)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace embody {

// Files a server uploads to the robot (pictures, sounds), kept in the "userdata"
// FAT partition (about 1.9 MB) mounted at /user. They survive reboots and
// firmware updates. Names are relative paths such as "food/cake.png".
//
// Uploads come in chunks (binary 0x11), in order; each is written to a
// temporary file that replaces the old file only when the last chunk arrived.
class AssetStore {
public:
    struct Entry {
        std::string name;
        uint32_t bytes = 0;
        uint32_t crc   = 0;  // CRC-32 (IEEE, as zlib and Go's hash/crc32)
    };
    // What one chunk did: done (the file is saved) or an error, else in progress.
    struct Result {
        bool done = false;
        std::string error;
        std::string name;
        uint32_t bytes = 0;
        uint32_t crc   = 0;
    };

    // Mounts /user once (formats the partition the first time); later calls are no-ops.
    bool mount();
    bool mounted() const;

    // One upload chunk: uint8 name length, name, uint32 LE total size,
    // uint32 LE offset, then the data. Offset 0 starts (or restarts) an upload.
    Result put(const std::string& payload);

    std::vector<Entry> list();
    bool remove(const std::string& name);
    void usage(uint64_t& total, uint64_t& free);

    // The file path for a valid name, "" otherwise.
    std::string path(const std::string& name) const;
    // 1-64 characters from [A-Za-z0-9._-/], no empty, "." or ".." parts, not starting with '.'.
    static bool validName(const std::string& name);

private:
    FILE* _up = nullptr;
    std::string _up_name;
    uint32_t _up_total   = 0;
    uint32_t _up_written = 0;
    uint32_t _up_crc     = 0;

    void abort_upload();
    void list_dir(const std::string& dir, const std::string& prefix, std::vector<Entry>& out);
};

}  // namespace embody
