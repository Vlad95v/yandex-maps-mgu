// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
#include "frame_dump.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace woc {

const char *const FrameDump::kAtlasName = "carplay-frame-1440x636.yuva";
const char *const FrameDump::kJsonName = "carplay-frame-1440x636.json";

namespace {

/* The whole buffer, or false with why. */
bool write_all(int fd, const void *data, size_t n, std::string *why)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    size_t left = n;
    while (left) {
        const ssize_t w = ::write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            *why = std::string("write: ") + std::strerror(errno);
            return false;
        }
        p += w;
        left -= (size_t)w;
    }
    return true;
}

/* A file written whole and renamed over its name: the archive never sees a
 * half written one. */
bool write_atomic(const std::string &path, const void *data, size_t n, std::string *why)
{
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        *why = std::string("open: ") + std::strerror(errno);
        return false;
    }
    bool ok = write_all(fd, data, n, why);
    if (ok && ::fsync(fd) != 0) {
        *why = std::string("fsync: ") + std::strerror(errno);
        ok = false;
    }
    if (::close(fd) != 0 && ok) {
        *why = std::string("close: ") + std::strerror(errno);
        ok = false;
    }
    if (!ok) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        *why = std::string("rename: ") + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

/* The folder, made if it is not there (the service's state folder is, as
 * its unit says; the tests' is not). */
bool make_dir(const std::string &dir, std::string *why)
{
    if (dir.empty()) {
        *why = "no frame dump folder";
        return false;
    }
    struct stat st;
    if (::stat(dir.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode) ? true : (*why = "the frame dump folder is not a folder", false);
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        *why = std::string("mkdir ") + dir + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

std::string utc_now(int64_t ms)
{
    const time_t t = (time_t)(ms / 1000);
    struct tm tmv;
    if (!gmtime_r(&t, &tmv))
        return std::string();
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return b;
}

/* A metadata string as JSON holds it: only letters, digits and a few safe
 * punctuation marks, at most 64 of them (the app can send nothing else; this
 * keeps a stray quote out of the file). */
std::string safe(const std::string &s)
{
    std::string o;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-' || c == ':' || c == '/' || c == '+' || c == ' ';
        if (ok)
            o.push_back(c);
        if (o.size() >= 64)
            break;
    }
    return o;
}

}  // namespace

FrameDump::FrameDump(std::string dir) : dir_(std::move(dir)) {}

void FrameDump::request(int64_t now_ms)
{
    (void)now_ms;
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ == State::Waiting || state_ == State::Captured) {
        /* one request, one frame: what waits or writes is the answer */
        return;
    }
    state_ = State::Waiting;
    frame_.clear();
    reason_ = "waiting for a 1440x636 CarPlay frame";
    path_.clear();
    utc_.clear();
    w_ = h_ = 0;
    captured_ms_ = 0;
    captured_cal_ = Calibration();
}

void FrameDump::set_calibration(const Calibration &c)
{
    std::lock_guard<std::mutex> lock(mu_);
    cal_ = c;
}

bool FrameDump::waiting() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return state_ == State::Waiting;
}

void FrameDump::offer(const uint8_t *atlas, size_t bytes, uint32_t w, uint32_t h, bool part, int64_t now_ms)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::Waiting)
        return;
    if (part) {
        reason_ = "the picture is a part of the frame (hosted_vpp_scale), not the whole CarPlay frame";
        return;
    }
    if (w != kW || h != kH) {
        char b[96];
        std::snprintf(b, sizeof b, "waiting for a 1440x636 CarPlay frame; the picture is %ux%u", w, h);
        reason_ = b;
        return;
    }
    if (!atlas || bytes != atlas_bytes(w, h)) {
        char b[128];
        std::snprintf(b, sizeof b, "the picture's atlas is %zu bytes, not the %zu of a %ux%u 4:2:0 frame", bytes,
                      atlas_bytes(w, h), w, h);
        reason_ = b;
        return;
    }
    frame_.assign(atlas, atlas + bytes);
    w_ = w;
    h_ = h;
    captured_cal_ = cal_;
    captured_cal_.normalized_w = w;
    captured_cal_.normalized_h = h;
    captured_ms_ = now_ms;
    utc_ = utc_now(now_ms);
    state_ = State::Captured;
    reason_.clear();
}

void FrameDump::write_pending(int64_t now_ms)
{
    std::vector<uint8_t> frame;
    uint32_t w = 0, h = 0;
    int64_t ms = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_ != State::Captured || frame_.empty())
            return;
        frame.swap(frame_);
        w = w_;
        h = h_;
        ms = captured_ms_;
    }
    std::string why;
    const bool ok = write(frame, w, h, ms, &why);
    std::lock_guard<std::mutex> lock(mu_);
    if (ok) {
        state_ = State::Saved;
        path_ = dir_ + "/" + kAtlasName;
        reason_.clear();
    } else {
        state_ = State::Failed;
        reason_ = why;
        path_ = dir_ + "/" + kAtlasName;
    }
    (void)now_ms;
}

bool FrameDump::write(const std::vector<uint8_t> &atlas, uint32_t w, uint32_t h, int64_t ms, std::string *why)
{
    if (!make_dir(dir_, why))
        return false;
    const std::string atlas_path = dir_ + "/" + kAtlasName;
    const std::string json_path = dir_ + "/" + kJsonName;
    if (!write_atomic(atlas_path, atlas.data(), atlas.size(), why))
        return false;
    char json[2048];
    const int n = std::snprintf(
        json, sizeof json,
        "{\"width\":%u,\"height\":%u,\"format\":\"yuv420p_atlas\",\"atlas_height\":%u,\"size\":%zu,"
        "\"captured_utc\":\"%s\",\"source\":\"carplay\",\"full_frame\":true,"
        /* the calibration: advertised (selected and active), the incoming
         * SPS, the normalized atlas, the cluster crop and output, the zoom */
        "\"selected\":\"%s\",\"active\":\"%s\",\"patch\":\"%s\","
        "\"sps_width\":%u,\"sps_height\":%u,"
        "\"normalized_width\":%u,\"normalized_height\":%u,"
        "\"crop_x\":%d,\"crop_y\":%d,\"crop_w\":%u,\"crop_h\":%u,"
        "\"output_width\":%u,\"output_height\":%u,\"zoom\":%.1f}\n",
        w, h, h + h / 2, atlas.size(), utc_now(ms).c_str(),
        safe(captured_cal_.selected).c_str(), safe(captured_cal_.active).c_str(), safe(captured_cal_.patch).c_str(),
        captured_cal_.sps_w, captured_cal_.sps_h,
        captured_cal_.normalized_w, captured_cal_.normalized_h,
        captured_cal_.crop_x, captured_cal_.crop_y, captured_cal_.crop_w, captured_cal_.crop_h,
        captured_cal_.output_w, captured_cal_.output_h, captured_cal_.zoom);
    if (n <= 0 || (size_t)n >= sizeof json) {
        *why = "the metadata does not fit";
        ::unlink(atlas_path.c_str());
        return false;
    }
    if (!write_atomic(json_path, json, (size_t)n, why)) {
        ::unlink(atlas_path.c_str());   /* a frame without its metadata is not one */
        return false;
    }
    return true;
}

FrameDump::State FrameDump::state() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

const char *FrameDump::state_word() const
{
    switch (state()) {
    case State::Waiting:  return "waiting";
    case State::Captured: return "captured";
    case State::Saved:    return "saved";
    case State::Failed:   return "failed";
    case State::Off:      break;
    }
    return "off";
}

std::string FrameDump::reason() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return reason_;
}

std::string FrameDump::path() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return path_;
}

std::string FrameDump::captured_utc() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return utc_;
}

bool FrameDump::captured(std::vector<uint8_t> *out, uint32_t *w, uint32_t *h) const
{
    std::lock_guard<std::mutex> lock(mu_);
    if (frame_.empty())
        return false;
    *out = frame_;
    *w = w_;
    *h = h_;
    return true;
}

}  // namespace woc
