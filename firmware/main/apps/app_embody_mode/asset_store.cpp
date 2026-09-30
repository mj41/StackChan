/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "asset_store.h"
#include <dirent.h>
#include <esp_rom_crc.h>
#include <esp_vfs_fat.h>
#include <mooncake_log.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cctype>
#include <cstring>

namespace embody {

static const char* _tag       = "Assets";
static const char* _base      = "/user";
static const char* _partition = "userdata";
static const char* _tmp_path  = "/user/.upload.tmp";
static constexpr size_t _max_name    = 64;
static constexpr size_t _max_entries = 256;

static bool _is_mounted = false;  // the VFS mount outlives the app (it is reopened)
static wl_handle_t _wl  = WL_INVALID_HANDLE;

bool AssetStore::mount()
{
    if (_is_mounted) {
        return true;
    }
    esp_vfs_fat_mount_config_t cfg = {};
    cfg.max_files                  = 4;
    cfg.format_if_mount_failed     = true;  // the first time: an empty partition
    cfg.allocation_unit_size       = 4096;
    const esp_err_t err            = esp_vfs_fat_spiflash_mount_rw_wl(_base, _partition, &cfg, &_wl);
    if (err != ESP_OK) {
        mclog::tagError(_tag, "mount {} failed: {}", _partition, esp_err_to_name(err));
        return false;
    }
    _is_mounted = true;
    uint64_t total = 0, free = 0;
    usage(total, free);
    mclog::tagInfo(_tag, "mounted {}: {} KB free of {} KB", _base, free / 1024, total / 1024);
    return true;
}

bool AssetStore::mounted() const
{
    return _is_mounted;
}

void AssetStore::usage(uint64_t& total, uint64_t& free)
{
    total = free = 0;
    if (_is_mounted) {
        esp_vfs_fat_info(_base, &total, &free);
    }
}

bool AssetStore::validName(const std::string& name)
{
    if (name.empty() || name.size() > _max_name || name.front() == '/' || name.back() == '/') {
        return false;
    }
    size_t part_start = 0;
    for (size_t i = 0; i <= name.size(); i++) {
        if (i == name.size() || name[i] == '/') {
            const std::string part = name.substr(part_start, i - part_start);
            if (part.empty() || part.front() == '.') {  // also rejects "." and ".." and hidden files
                return false;
            }
            part_start = i + 1;
            continue;
        }
        const char c = name[i];
        if (!(std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

std::string AssetStore::path(const std::string& name) const
{
    return validName(name) ? std::string(_base) + "/" + name : std::string();
}

void AssetStore::abort_upload()
{
    if (_up) {
        fclose(_up);
        _up = nullptr;
        unlink(_tmp_path);
    }
    _up_name.clear();
    _up_total = _up_written = _up_crc = 0;
}

static uint32_t read_u32(const std::string& s, size_t at)
{
    return (uint32_t)(uint8_t)s[at] | ((uint32_t)(uint8_t)s[at + 1] << 8) | ((uint32_t)(uint8_t)s[at + 2] << 16) |
           ((uint32_t)(uint8_t)s[at + 3] << 24);
}

// mkdirs creates the folders of a file path under /user.
static void mkdirs(const std::string& file)
{
    for (size_t i = std::strlen(_base) + 1; i < file.size(); i++) {
        if (file[i] == '/') {
            mkdir(file.substr(0, i).c_str(), 0775);  // exists already: fine
        }
    }
}

AssetStore::Result AssetStore::put(const std::string& payload)
{
    Result res;
    if (!_is_mounted) {
        res.error = "no storage";
        return res;
    }
    if (payload.empty() || payload.size() < 1u + (uint8_t)payload[0] + 8) {
        res.error = "short chunk";
        return res;
    }
    const size_t name_len = (uint8_t)payload[0];
    res.name              = payload.substr(1, name_len);
    const uint32_t total  = read_u32(payload, 1 + name_len);
    const uint32_t offset = read_u32(payload, 5 + name_len);
    const char* data      = payload.data() + 9 + name_len;
    const size_t len      = payload.size() - 9 - name_len;
    if (!validName(res.name)) {
        res.error = "bad name";
        return res;
    }

    if (offset == 0) {  // a new upload (an unfinished one is dropped)
        abort_upload();
        uint64_t size = 0, free = 0;
        usage(size, free);
        struct stat st = {};
        const uint64_t replaced = stat(path(res.name).c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
        if ((uint64_t)total > free + replaced) {
            res.error = "no space";
            return res;
        }
        _up = fopen(_tmp_path, "wb");
        if (!_up) {
            res.error = "cannot write";
            return res;
        }
        _up_name  = res.name;
        _up_total = total;
    } else if (!_up || res.name != _up_name || offset != _up_written || total != _up_total) {
        abort_upload();
        res.error = "chunk out of order";
        return res;
    }

    if (_up_written + len > _up_total) {
        abort_upload();
        res.error = "more data than the size";
        return res;
    }
    if (len && fwrite(data, 1, len, _up) != len) {
        abort_upload();
        res.error = "write failed (full?)";
        return res;
    }
    _up_crc = esp_rom_crc32_le(_up_crc, (const uint8_t*)data, len);
    _up_written += len;
    if (_up_written < _up_total) {
        return res;  // more to come
    }

    fclose(_up);
    _up                    = nullptr;
    const std::string dest = path(res.name);
    mkdirs(dest);
    unlink(dest.c_str());
    if (rename(_tmp_path, dest.c_str()) != 0) {
        unlink(_tmp_path);
        res.error = "rename failed";
        _up_name.clear();
        return res;
    }
    res.done  = true;
    res.bytes = _up_total;
    res.crc   = _up_crc;
    mclog::tagInfo(_tag, "saved {} ({} bytes)", res.name, res.bytes);
    _up_name.clear();
    _up_total = _up_written = _up_crc = 0;
    return res;
}

static uint32_t file_crc(const std::string& file)
{
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) {
        return 0;
    }
    uint32_t crc = 0;
    uint8_t buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc = esp_rom_crc32_le(crc, buf, n);
    }
    fclose(f);
    return crc;
}

void AssetStore::list_dir(const std::string& dir, const std::string& prefix, std::vector<Entry>& out)
{
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return;
    }
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' || out.size() >= _max_entries) {
            continue;  // the temporary upload, "." and ".."
        }
        const std::string full = dir + "/" + e->d_name;
        const std::string name = prefix + e->d_name;
        struct stat st         = {};
        if (stat(full.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            list_dir(full, name + "/", out);
        } else {
            out.push_back({name, (uint32_t)st.st_size, file_crc(full)});
        }
    }
    closedir(d);
}

std::vector<AssetStore::Entry> AssetStore::list()
{
    std::vector<Entry> out;
    if (_is_mounted) {
        list_dir(_base, "", out);
    }
    return out;
}

bool AssetStore::remove(const std::string& name)
{
    const std::string file = path(name);
    if (file.empty() || unlink(file.c_str()) != 0) {
        return false;
    }
    // Drop folders left empty (rmdir fails on a folder with files: fine).
    for (size_t slash = file.rfind('/'); slash > std::strlen(_base); slash = file.rfind('/', slash - 1)) {
        if (rmdir(file.substr(0, slash).c_str()) != 0) {
            break;
        }
    }
    return true;
}

}  // namespace embody
