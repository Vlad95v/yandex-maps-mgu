// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* frame_dump.h: the app's one-shot full CarPlay frame. A request waits for
 * the next 1440x636 whole frame (a part, a repeat, another size or a short
 * atlas is not taken and says why), the frame is copied without touching the
 * decode path, and it is written atomically with its metadata; a second
 * request while one waits or is written makes no second frame, and a saved
 * frame is replaced by the next request. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "frame_dump.h"

using namespace woc;

static int failures;
static int checks;

static void ok(const char *name, bool cond)
{
    checks++;
    if (!cond) {
        std::printf("FAIL %s\n", name);
        failures++;
    }
}

static bool read_file(const std::string &path, std::vector<uint8_t> *out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out->clear();
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out->insert(out->end(), buf, buf + n);
    const bool ok_read = std::ferror(f) == 0;
    std::fclose(f);
    return ok_read;
}

int main()
{
    char tmpl[] = "/tmp/woc-frame-dump-XXXXXX";
    const char *root = mkdtemp(tmpl);
    if (!root) {
        std::printf("frame_dump_test: cannot make a folder\n");
        return 1;
    }
    const std::string dir = std::string(root) + "/state";
    const std::vector<uint8_t> atlas(FrameDump::atlas_bytes(), 0x5a);
    const size_t bytes = atlas.size();

    ok("a 1440x636 4:2:0 frame is 1,373,760 bytes", bytes == 1373760 && FrameDump::atlas_bytes() == 1373760);

    FrameDump dump(dir);
    ok("nothing asked: off", dump.state() == FrameDump::State::Off && std::strcmp(dump.state_word(), "off") == 0);

    dump.request(1000);
    ok("asked: waiting", dump.state() == FrameDump::State::Waiting && std::strcmp(dump.state_word(), "waiting") == 0);

    /* The calibration the app sent with the request: the selected and
     * active advertised resolutions and the patch state, plus this process's
     * own SPS, crop and output. */
    FrameDump::Calibration cal;
    cal.selected = "1200x530";
    cal.active = "1440x636";
    cal.patch = "patched";
    cal.sps_w = 1200;
    cal.sps_h = 530;
    cal.crop_x = 176;
    cal.crop_y = 0;
    cal.crop_w = 1088;
    cal.crop_h = 480;
    cal.output_w = 1088;
    cal.output_h = 480;
    cal.zoom = 100.0;
    dump.set_calibration(cal);

    /* A part, another size, a short atlas: none is taken, and the reason says
     * which; the request still stands. */
    dump.offer(atlas.data(), bytes, 1440, 636, /*part=*/true, 1001);
    ok("a part is not taken", dump.state() == FrameDump::State::Waiting && dump.reason().find("part") != std::string::npos);
    dump.offer(atlas.data(), 1088 * 720, 1088, 480, false, 1002);
    ok("another size is not taken, and the reason says what came",
       dump.state() == FrameDump::State::Waiting && dump.reason().find("1088x480") != std::string::npos &&
           dump.reason().find("1440x636") != std::string::npos);
    dump.offer(atlas.data(), 10, 1440, 636, false, 1003);
    ok("a short atlas is not taken", dump.state() == FrameDump::State::Waiting && dump.reason().find("bytes") != std::string::npos);

    dump.offer(atlas.data(), bytes, 1440, 636, false, 1004);
    ok("the whole frame is taken", dump.state() == FrameDump::State::Captured && dump.reason().empty());
    std::vector<uint8_t> got;
    uint32_t gw = 0, gh = 0;
    ok("it is the frame, copied", dump.captured(&got, &gw, &gh) && gw == 1440 && gh == 636 && got == atlas);
    ok("the capture time is UTC", dump.captured_utc().size() == 20 && dump.captured_utc()[10] == 'T' &&
                                     dump.captured_utc()[19] == 'Z');

    /* One request, one frame: a second request while it waits or is written
     * changes nothing. */
    dump.request(1005);
    std::vector<uint8_t> again;
    uint32_t aw = 0, ah = 0;
    ok("a second request during the capture: the same one frame", dump.state() == FrameDump::State::Captured &&
                                                                     dump.captured(&again, &aw, &ah) && again == atlas);

    dump.write_pending(1006);
    ok("written: saved", dump.state() == FrameDump::State::Saved && std::strcmp(dump.state_word(), "saved") == 0);
    std::vector<uint8_t> on_disk;
    ok("the file is there, whole", read_file(dir + "/" + FrameDump::kAtlasName, &on_disk) && on_disk.size() == bytes &&
                                      on_disk == atlas);
    std::vector<uint8_t> json;
    const std::string json_text = read_file(dir + "/" + FrameDump::kJsonName, &json)
                                      ? std::string(json.begin(), json.end())
                                      : std::string();
    ok("the metadata is there", !json_text.empty() && json_text.find("\"width\":1440") != std::string::npos &&
                                    json_text.find("\"height\":636") != std::string::npos &&
                                    json_text.find("\"format\":\"yuv420p_atlas\"") != std::string::npos &&
                                    json_text.find("\"atlas_height\":954") != std::string::npos &&
                                    json_text.find("\"size\":1373760") != std::string::npos &&
                                    json_text.find("\"source\":\"carplay\"") != std::string::npos &&
                                    json_text.find("\"full_frame\":true") != std::string::npos &&
                                    json_text.find("\"captured_utc\":\"") != std::string::npos);
    ok("the calibration metadata is there", json_text.find("\"selected\":\"1200x530\"") != std::string::npos &&
                                               json_text.find("\"active\":\"1440x636\"") != std::string::npos &&
                                               json_text.find("\"patch\":\"patched\"") != std::string::npos &&
                                               json_text.find("\"sps_width\":1200") != std::string::npos &&
                                               json_text.find("\"sps_height\":530") != std::string::npos &&
                                               json_text.find("\"normalized_width\":1440") != std::string::npos &&
                                               json_text.find("\"normalized_height\":636") != std::string::npos &&
                                               json_text.find("\"crop_x\":176") != std::string::npos &&
                                               json_text.find("\"crop_y\":0") != std::string::npos &&
                                               json_text.find("\"crop_w\":1088") != std::string::npos &&
                                               json_text.find("\"crop_h\":480") != std::string::npos &&
                                               json_text.find("\"output_width\":1088") != std::string::npos &&
                                               json_text.find("\"output_height\":480") != std::string::npos &&
                                               json_text.find("\"zoom\":100.0") != std::string::npos);
    struct stat st;
    ok("the file is readable by the logs service (0644)",
       ::stat((dir + "/" + FrameDump::kAtlasName).c_str(), &st) == 0 && (st.st_mode & 0777) == 0644);
    ok("no temporary file is left", ::access((dir + "/" + FrameDump::kAtlasName + ".tmp").c_str(), F_OK) != 0 &&
                                        ::access((dir + "/" + FrameDump::kJsonName + ".tmp").c_str(), F_OK) != 0);
    ok("the path is the file", dump.path() == dir + "/" + FrameDump::kAtlasName);

    /* The frame taken is a copy: the producer's buffer changing afterwards
     * does not change it. */
    std::vector<uint8_t> mine = atlas;
    dump.request(2000);
    dump.offer(mine.data(), bytes, 1440, 636, false, 2001);
    mine.assign(bytes, 0x00);
    std::vector<uint8_t> copy;
    uint32_t cw = 0, ch = 0;
    ok("the captured frame is a copy", dump.captured(&copy, &cw, &ch) && copy == atlas);
    dump.write_pending(2002);
    ok("a new request after a saved one replaces the file", dump.state() == FrameDump::State::Saved &&
                                                                   read_file(dir + "/" + FrameDump::kAtlasName, &on_disk) &&
                                                                   on_disk == atlas);

    /* A folder that is a file: the write fails with the reason, and nothing
     * half written is left. */
    const std::string blocked = std::string(root) + "/blocked";
    FILE *b = std::fopen(blocked.c_str(), "wb");
    std::fclose(b);
    FrameDump bad(blocked);
    bad.request(3000);
    bad.offer(atlas.data(), bytes, 1440, 636, false, 3001);
    bad.write_pending(3002);
    ok("a failed write says why", bad.state() == FrameDump::State::Failed && std::strcmp(bad.state_word(), "failed") == 0 &&
                                      !bad.reason().empty());

    /* A request after a failure asks again. */
    bad.request(3003);
    ok("a request after a failure waits again", bad.state() == FrameDump::State::Waiting);

    if (failures)
        std::printf("%d of %d FAILED\n", failures, checks);
    else
        std::printf("all %d ok\n", checks);
    return failures ? 1 : 0;
}
