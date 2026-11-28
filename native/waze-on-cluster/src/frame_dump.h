// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* FrameDump: one full CarPlay frame, saved at the app's request.
 *
 * The producer hands the newest decoded picture to the map feed as a 4:2:0
 * atlas (yuv_atlas.h) before MapFeed crops it for the cluster or the HUD:
 * a 1440x636 CarPlay frame is a 1440x954 atlas, 1,373,760 bytes. The app's
 * Logs page asks for the next such frame once; the frame is copied on the
 * main thread (never on the decode thread) and written to disk right after,
 * atomically: a temporary file, then a rename, and the metadata beside it.
 *
 * One request, one frame: a request while a frame waits to be written, or
 * one already written and not yet archived, is left as it is (the app shows
 * the state it has); the next request after that saves a new frame over the
 * old one. A frame that is a part (hosted_vpp_scale), smaller or larger
 * than the capture size, a repeat of the last one, or absent while the map
 * shows nothing is not taken, and the status says why: the app waits for a
 * full 1440x636 CarPlay picture, never for whatever came.
 *
 * The file lives where the logs service reads it to put it in the archive
 * (collect-logs.sh): the tool's state folder, world-readable, and the
 * archive deletes it once the archive is whole. A failed write keeps the
 * reason for the app and nothing half written. */
#ifndef WOC_FRAME_DUMP_H
#define WOC_FRAME_DUMP_H

#include <stddef.h>
#include <stdint.h>

#include <mutex>
#include <string>
#include <vector>

namespace woc {

class FrameDump {
public:
    /* off: nobody asked; waiting: the request stands, no frame yet;
     * captured: the frame is in memory or being written; saved: on disk,
     * for the archive; failed: the write failed, why in reason(). */
    enum class State { Off, Waiting, Captured, Saved, Failed };

    /* The calibration the frame is taken under, written beside it: what the
     * app selected and what carplay advertises (its patch state), the
     * incoming H.264 SPS's own size, the normalized decoded atlas, the
     * cluster crop and output, and the effective zoom. 0/empty when not
     * known (a capture from an older app). */
    struct Calibration {
        std::string selected, active, patch;
        uint32_t sps_w = 0, sps_h = 0;
        uint32_t normalized_w = 0, normalized_h = 0;
        int crop_x = 0, crop_y = 0;
        uint32_t crop_w = 0, crop_h = 0;
        uint32_t output_w = 0, output_h = 0;
        double zoom = 0.0;
    };

    /* The frame the dump saves: the CarPlay cluster source (1440x636). */
    static const uint32_t kW = 1440, kH = 636;
    /* The atlas the frame is (yuv_atlas.h, 4:2:0: w x (h + h/2)). */
    static size_t atlas_bytes(uint32_t w = kW, uint32_t h = kH) { return (size_t)w * (h + h / 2); }

    /* The file names, beside each other in the folder given. */
    static const char *const kAtlasName;   /* carplay-frame-1440x636.yuva */
    static const char *const kJsonName;    /* carplay-frame-1440x636.json */

    explicit FrameDump(std::string dir);

    /* The next full CarPlay frame, once. Nothing while one waits or one is
     * saved and not yet archived: one file, never two. */
    void request(int64_t now_ms);
    /* The calibration metadata the next captured frame is written with; a
     * request keeps the app's fields as given, offer() fills the frame's. */
    void set_calibration(const Calibration &c);
    /* The request still stands. */
    bool waiting() const;
    /* The newest picture, from the main loop: taken when the request stands
     * and the frame is the whole CarPlay frame (no part), at the capture
     * size, with the atlas exactly its size. A repeat has no atlas and is
     * not taken. */
    void offer(const uint8_t *atlas, size_t bytes, uint32_t w, uint32_t h, bool part, int64_t now_ms);
    /* The captured frame to disk: an atomic write, and the metadata beside
     * it. Called on the main thread, never on the decode thread. */
    void write_pending(int64_t now_ms);

    State state() const;
    /* The state as the status line and the app name it. */
    const char *state_word() const;
    /* Why the frame is not the one asked for, or why the write failed;
     * empty while it is or was saved. */
    std::string reason() const;
    /* The file the frame went to, once saved. */
    std::string path() const;
    /* The capture time, as the metadata has it (empty: no capture). */
    std::string captured_utc() const;

    /* For the tests: the captured frame, copied out. */
    bool captured(std::vector<uint8_t> *out, uint32_t *w, uint32_t *h) const;
    /* For the tests: the file names' folder. */
    const std::string &dir() const { return dir_; }

private:
    /* The frame written: temp, fsync, rename, and the JSON after it. */
    bool write(const std::vector<uint8_t> &atlas, uint32_t w, uint32_t h, int64_t ms, std::string *why);

    const std::string dir_;
    mutable std::mutex mu_;
    State state_ = State::Off;
    std::vector<uint8_t> frame_;   /* the captured atlas, until written */
    uint32_t w_ = 0, h_ = 0;
    int64_t captured_ms_ = 0;
    Calibration cal_;             /* what the app set for the next capture */
    Calibration captured_cal_;    /* the captured frame's, frozen at offer() */
    std::string reason_;
    std::string path_;
    std::string utc_;
};

}  // namespace woc

#endif
