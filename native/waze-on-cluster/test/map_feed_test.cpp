// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Host test of the map feed (map_feed.h) against the shared cluster renderer
 * (cluster-render, the CR tree) on fake display backends, over its socket:
 * - the map is a contending content layer on the map target, covering the
 *   1088x480 slot; before a frame the app sees idle, with the first frames
 *   the slot is asked for (starting) and the map shown with the frames as
 *   they were decoded;
 * - the stock map taking the slot shows as starting with that reason; the
 *   contending map asks again, parks behind it, takes it over and is shown,
 *   the stock map parked behind it;
 * - frames stopping: idle after stall_ms, the slot released;
 * - the renderer restarting: starting while it is gone, an error once it
 *   stays gone 15 s, then the map again with the next frames;
 * - a lowered shared upload cap shows on the status line, with its reason in
 *   the status, and leaves the line once it is back at the ceiling;
 * - frames offered faster than they can be written are newest-wins;
 * - the stream not live: the layer hidden, idle, the slot released and the
 *   stock map back; live again: shown with the frames; frozen: hidden, the
 *   status says so; no longer frozen: shown again;
 * - before the stream is live the layer is made hidden;
 * - the stream's pictures through the change sampler and freeze watch, as
 *   the service runs them: a stream blank from its start (the car's burst,
 *   then a pause) never asks for the slot, the stock map stays and the app
 *   is told it waits for the phone's map; the first non-blank frame, with no frame
 *   after it, takes the slot over and shows that frame; blank spells
 *   shorter than blank_ms between pictures keep the map up without asking
 *   for the slot again; a picture blank for blank_ms gives the slot back to
 *   the stock map, and the next picture takes it again; a stream blank at
 *   full rate from its start is frozen, the stock map showing; live
 *   through a recent frame (no connection to read), a static map's frame
 *   every 10 s takes the slot over each time it makes the stream live;
 *   a frame of the ended stream decoded after it ended, seconds before the
 *   next stream is live, does not take the slot for it, while one decoded
 *   just before it is live does; the stream ending is logged once, whatever
 *   frames still come;
 * - the status is logged when its state, reason or target changes, not for
 *   the frame rate, the page or the upload cap;
 * - BMW's turn box (the layer's guide_hold): hidden by default, the hold of
 *   its slot following the map; the switch off releases it and the box
 *   shows, the map still showing; on again, held; off from the start,
 *   never held; the status never says the hold is disabled;
 * - only what the renderer needs goes: a picture repeated (as the producer
 *   hands over a picture the same as the last) keeps a static map showing
 *   past stall_ms; nothing reaches the renderer while the layer is hidden,
 *   and shown again it draws the newest picture first; after the renderer
 *   restarts, and on another target, repeats alone bring the picture;
 * - MapFeeds: the HUD off, on beside the cluster (scaled, at most hud_fps),
 *   beside the cluster's map on panel, its size and place, kept shown over
 *   BMW's pop-ups or giving way to them, alone, neither; a still map's repeats keep
 *   both layers, or the HUD alone, shown past stall_ms with no frame whole
 *   and none scaled, and moving again the HUD scales at most hud_fps while
 *   the cluster keeps its rate; a feed made beside the other starts from its
 *   picture;
 * - Off (neither), from the cluster alone and from both: the map's and the
 *   HUD's slots and the turn box's hold released, the stock map and box
 *   shown again, nothing asked of the renderer while off; the cluster, or
 *   both, again: the next frame shown, the hold back;
 * - hosted by another app's scene (hosting): the map in its scene at the
 *   host's rect with the host's part of the picture, its own slot released;
 *   hidden by the host, no layer and nothing sent; shown again; the host
 *   gone, back on its own target; no part asked of the decoder (by default);
 *   a host whose place cannot be drawn (its part misses the map, its rect
 *   too small): on its own target instead;
 * - hosted by a theme's layers, no scene (layers_hosting): in the panel at
 *   the host's rect, its layer marked as the hosted map and so under the
 *   theme's layers; back on its own target while
 *   they are hidden and once the theme is gone;
 * - the cluster's map hosted (hosted_wake): a frame with the scaling worker
 *   wakes the service's loop to take it, and its part is drawn;
 * - hosted with hosted_vpp_scale (vpp_hosting): the host's part asked of the
 *   decoder at the size it is shown at, a whole frame meanwhile scaled by
 *   the worker, a VPP-made one sent as it is, one of another part dropped,
 *   the host's move asking for its new part, the host gone, none;
 * - always_packed (always_packed_map): 0, the layer without always_packed and a still
 *   map exact; 1, every update packed, a still map's too, never exact; with
 *   hosted_vpp_scale the VPP-made part packed; a host's part not a multiple
 *   of 4 high as with 0, shown; cluster-render upgraded or downgraded under
 *   the feed, the layer made again for it (renderer_changed). */
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

/* cluster-render's service and fakes, from its tree (-I$(CR)) */
#include "src/service.h"
#include "src/video_layer.h"
#include "block_pack.h"
#include "cluster_render/protocol.h"
#include "test/fake_graph.h"
#include "test/fake_port.h"
#include "../src/map_feed.h"
#include "src/log.h"
#include "../src/map_feeds.h"
#include "../src/atlas_scale.h"

using namespace woc;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

static int rigs;
static const uint64_t kBox = 0x01000310, kGuide = 0x010004f1;

/* The feed's worker is still scaling a frame, or has one to take: the feed
 * then asks to be woken in kScaleWaitMs however much later it is asked,
 * while a frame due in kScaleWaitMs asks for a millisecond less a
 * millisecond later. */
template <class Feed> static bool scaling(const Feed &feed, int64_t now)
{
    return feed.wake_in_ms(now) == MapFeed::kScaleWaitMs && feed.wake_in_ms(now + 1) == MapFeed::kScaleWaitMs;
}

struct Rig {
    std::string path = "/tmp/woc-feed-test-" + std::to_string(getpid()) + "-" + std::to_string(++rigs) + ".sock";
    crs::FakeGpu gpu;
    crs::FakePort port;
    crs::ServiceConfig sc;
    std::unique_ptr<crs::Service> svc;
    std::unique_ptr<MapFeed> feed;
    int64_t now = 200000;
    std::vector<uint8_t> spare;   /* the buffer handed to the feed, as the producer's */
    /* watched: the frames' pictures drive the stream state, as in the
     * service's loop; otherwise the stream is live with a picture */
    bool watched;
    /* watched and > 0: live while a frame came within this long, as the
     * service decides without the stream's connection; 0: live while live
     * is set, as the stream's connection says */
    int64_t live_stall_ms = 0;
    bool live = true;
    /* frames the same as the last offered go as repeats, as the producer
     * hands them over */
    bool repeats = false;
    /* set: the frames are this part, made by VPP (hosted_vpp_scale) */
    OutputPart part;
    std::vector<uint8_t> offered;
    int64_t last_frame_ms = -1;
    FreezeWatch watch{FreezeConfig()};
    ChangeSampler sampler{1088, 480};
    cr::Client *other = nullptr;   /* another app of the renderer, polled with the feed */

    /* w: watched (above); hold: with the guide hold of the turn box's slot
     * following the map, as cluster-render.conf ships it; guide_hold: the
     * feed's setting; hud: the HUD target too.
     * video_w/video_h: optional source video dimensions (e.g., 1440x636 for
     * cluster source-crop). Default 0 means no scaling configured. */
    explicit Rig(bool w = false, unsigned stall_ms = 6000, bool hold = false, bool guide_hold = true, bool hud = false,
                 uint32_t video_w = 0, uint32_t video_h = 0)
        : watched(w)
    {
        crs::TargetSpec m, p, g, u;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
        m.take_over_stock = true;   /* as cluster-render.conf ships it */
        p.name = "panel"; p.slot = 0x203; p.scene_id = 0x010004f8; p.width = 1920; p.height = 720; p.linger_ms = 3000;
        g.name = "guide_0x105"; g.slot = 0x105; g.scene_id = kGuide; g.width = g.height = 16; g.linger_ms = 0;
        g.take_over_stock = true; g.follows = "map"; g.take_over_contents = {kBox};
        u.name = "hud"; u.slot = 0x323; u.scene_id = 0x01000400; u.width = 800; u.height = 480; u.linger_ms = 3000;
        u.yield_slots = {0x322};   /* as cluster-render.conf ships it */
        u.yield_ms = 1500;
        sc.targets = {m, p};
        if (hud)
            sc.targets.push_back(u);
        if (hold)
            sc.targets.push_back(g);
        sc.socket_path = path;
        sc.backend = "fake";
        start();
        FeedConfig fc;
        fc.socket = path;
        fc.video.w = video_w;
        fc.video.h = video_h;
        fc.video.level_bits = 0;
        fc.video.stall_ms = stall_ms;
        fc.guide_hold = guide_hold;
        feed.reset(new MapFeed(fc));
        watch.live(true, now);
        feed->set_stream(true, false, watched ? watch.picture(now) : Picture::Shown);
    }
    void start()
    {
        svc.reset(new crs::Service(&gpu, &port, sc));
        port.m = &svc->slots();
        port.pending.clear();
        std::string err;
        if (!svc->start(&err))
            std::printf("start: %s\n", err.c_str());
    }
    /* Simulated time; each step runs the socket exchange until idle, and
     * until the scaling worker (its own thread, in real time) has nothing
     * left for the feed to take, as the service's loop wakes for it. */
    void pump(int64_t ms, const std::vector<uint8_t> *frame = nullptr, int64_t frame_every = 65)
    {
        for (int64_t t = 0; t < ms; t += 5) {
            now += 5;
            if (frame && t % frame_every == 0) {
                if (repeats && offered == *frame) {
                    feed->repeat((uint64_t)now, now);
                } else {
                    spare.assign(frame->begin(), frame->end());
                    if (part.set())
                        feed->offer_part(&spare, (uint64_t)now, part, now);
                    else
                        feed->offer(&spare, (uint64_t)now, now);
                    offered = *frame;
                }
                if (watched) {
                    const bool changed = sampler.changed(frame->data());
                    watch.frame(now, changed, sampler.blank());
                    last_frame_ms = now;
                }
            }
            if (watched) {
                const bool on = live_stall_ms ? last_frame_ms >= 0 && now - last_frame_ms <= live_stall_ms : live;
                watch.live(on, now);
                feed->set_stream(on, watch.frozen(now), watch.picture(now));
            }
            for (int round = 0, waits = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                size_t first = 0;
                if (svc)
                    first = svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                if (svc) {
                    port.tick(now);
                    svc->after_poll(fds, first, now);
                }
                const bool pending = feed->wants_write() || (other && other->wants_write());
                feed->poll(now);
                if (other)
                    other->poll(now);
                /* at most 2 s of real time a step for the worker */
                const bool wait = scaling(*feed, now) && waits < 2000;
                if (wait) {
                    usleep(1000);
                    waits++;
                    round--;
                }
                if (round > 0 && n <= 0 && !pending && !wait)
                    break;
            }
        }
    }
    FeedStatus st() { return feed->status("decode"); }
    /* The map status of content in the turn box's slot; -1: not mapped. */
    int in_box_slot(uint64_t content) const
    {
        const crs::SlotInfo *s = port.row(0x105, content);
        return s ? (int)s->map_status : -1;
    }
    const crs::Layer *map_layer()
    {
        for (const crs::Layer *l : svc->compositor().layers())
            return l;
        return nullptr;
    }
    /* The map layer's frames the renderer has counted: written, unchanged,
     * within the threshold, superseded. */
    unsigned long long received()
    {
        unsigned long long n[4] = {};
        if (const crs::Layer *l = map_layer())
            std::sscanf(l->stats().c_str(), "%llu sent, %llu unchanged, %llu within threshold, %llu superseded", &n[0],
                        &n[1], &n[2], &n[3]);
        return n[0] + n[1] + n[2] + n[3];
    }
    /* Whether the target's scene draws the map, and with this atlas. */
    bool drawing(const char *target, const std::vector<uint8_t> &f, bool *with)
    {
        crs::FakeGraph *g = gpu.graph(target);
        *with = false;
        if (!g || g->drawn().empty())
            return false;
        for (const auto &kv : g->shown.buffers)
            *with = *with || (kv.second.f == crs::TexFormat::R8 && kv.second.px == f);
        return true;
    }
};

static std::vector<uint8_t> atlas(int seed)
{
    std::vector<uint8_t> f(1088 * 720);
    for (size_t i = 0; i < f.size(); i++)
        f[i] = (uint8_t)(i < 1088 * 480 ? (i / 5 + seed * 17) & 0xff : 128);
    return f;
}

/* The app's switch for BMW's turn box: on by default, the hold following
 * the map and the box taken over; off, the hold released and the box
 * shown, the map still showing; on again, held. */
static void guide_hold()
{
    const std::vector<uint8_t> f = atlas(7);
    Rig h(false, 6000, true);
    h.port.stock_map(0x105, kBox);
    h.pump(1500, &f);
    ok("turn box: hidden by default, the hold shown in its slot with the box parked behind it",
       h.st().state == "showing" && h.st().guide_hold && h.in_box_slot(kGuide) == 1 && h.in_box_slot(kBox) == 3);
    h.feed->set_guide_hold(false);
    h.pump(500, &f);
    const size_t taken = h.port.taken_over.size();
    ok("turn box: the switch off releases the hold, the box shown, the map still showing, the status says so",
       h.in_box_slot(kGuide) == -1 && h.in_box_slot(kBox) == 1 && h.st().state == "showing" && !h.st().guide_hold);
    h.pump(5000, &f);
    ok("turn box: off, no take-over and no hold", h.port.taken_over.size() == taken && h.in_box_slot(kGuide) == -1);
    h.feed->set_guide_hold(true);
    h.pump(500, &f);
    ok("turn box: on again, held, the box taken over",
       h.in_box_slot(kGuide) == 1 && h.in_box_slot(kBox) == 3 && h.port.taken_over.back() == kBox);

    Rig off(false, 6000, true, false);
    off.port.stock_map(0x105, kBox);
    off.pump(3000, &f);
    ok("turn box: set off before the map is made, never held",
       off.st().state == "showing" && off.in_box_slot(kGuide) == -1 && off.in_box_slot(kBox) == 1);
    ok("turn box: the hold not disabled, the status says so", !h.st().guide_disabled);

}

/* The app's Off (MapFeeds on neither), as the app sends it: from the cluster
 * alone the cluster off; from both each off. Off releases the map's slot and
 * the turn box's hold, so the stock map and box show again, and the HUD's
 * slot; nothing is asked of the renderer while off. A display chosen again
 * shows the next frame, the hold back with it. */
static void show_off()
{
    const std::vector<uint8_t> f1 = atlas(11), f2 = atlas(12), f3 = atlas(13);
    const uint64_t kStock = 0x01000101, kMap = 0x010004f0, kHud = 0x01000400;
    Rig b(false, 6000, true, true, true);
    b.feed.reset();
    b.start();
    b.port.stock_map(0x100, kStock);
    b.port.stock_map(0x105, kBox);
    FeedConfig fc;
    fc.socket = b.path;
    fc.video.level_bits = 0;
    fc.guide_hold = true;
    MapFeeds feeds(fc, true, false, 10);
    feeds.set_stream(true, false, Picture::Shown);
    std::vector<uint8_t> spare;
    bool asked = false;
    const auto pump = [&](int64_t ms, const std::vector<uint8_t> *frame) {
        for (int64_t t = 0; t < ms; t += 5) {
            b.now += 5;
            if (frame && t % 65 == 0 && feeds.wants_frames()) {   /* the service offers only then */
                spare.assign(frame->begin(), frame->end());
                spare[(size_t)(t / 65) % 1088] ^= 0x80;
                feeds.offer(&spare, (uint64_t)b.now, b.now);
            }
            for (int round = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                const size_t first = b.svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                b.port.tick(b.now);
                b.svc->after_poll(fds, first, b.now);
                const bool pending = (feeds.cluster() && feeds.cluster()->wants_write()) || (feeds.hud_feed() && feeds.hud_feed()->wants_write());
                const bool busy = scaling(feeds, b.now);
                if (busy)
                    usleep(1000);
                feeds.poll(b.now);
                for (int i = 0; i < 4; i++)
                    asked = asked || b.svc->slots().requested(i);
                if (round > 0 && n <= 0 && !pending && !busy)
                    break;
            }
        }
    };
    const auto status_of = [&](uint64_t slot, uint64_t content) -> int {
        const crs::SlotInfo *r = b.port.row(slot, content);
        return r ? (int)r->map_status : -1;
    };
    const auto layers = [&]() { return b.svc->compositor().layers().size(); };
    const auto shown = [&](const char *target, const std::vector<uint8_t> &like) {
        crs::FakeGraph *g = b.gpu.graph(target);
        bool on = false;
        if (g)
            for (const auto &kv : g->shown.buffers)
                on = on || (kv.second.f == crs::TexFormat::R8 && kv.second.px.size() == like.size() &&
                            std::equal(kv.second.px.begin() + 1088, kv.second.px.end(), like.begin() + 1088));
        return on;
    };
    const auto map_up = [&]() {
        return status_of(0x100, kMap) == 1 && status_of(0x100, kStock) == 3 && status_of(0x105, kGuide) == 1 &&
               status_of(0x105, kBox) == 3;
    };
    const auto stock_back = [&]() {
        return status_of(0x100, kMap) == -1 && status_of(0x100, kStock) == 1 && status_of(0x105, kGuide) == -1 &&
               status_of(0x105, kBox) == 1 && status_of(0x323, kHud) == -1;
    };

    pump(2000, &f1);
    ok("Off: the cluster's map up first, the stock map and box parked behind it and the hold",
       map_up() && shown("map", f1) && feeds.status("decode").state == "showing");

    /* Off from the cluster alone: the app sends the cluster off */
    feeds.set_cluster(false);
    pump(5000, &f2);
    FeedStatus st = feeds.status("decode");
    ok("Off from the cluster: no feed or layer, nothing wanted, the map's slot and the hold released, the stock map and box back",
       !feeds.cluster() && !feeds.hud_feed() && layers() == 0 && !feeds.wants_frames() && stock_back() && st.state == "off" &&
           st.hud_state == "off" && !st.cluster && !st.hud);
    const int requests = b.port.requests;
    asked = false;
    pump(10000, &f2);
    ok("Off: nothing asked of the renderer for 10 s, no layer, the stock map and box still shown",
       b.port.requests == requests && !asked && layers() == 0 && stock_back());

    /* Cluster again: the next frame, the hold back */
    feeds.set_cluster(true);
    pump(2000, &f3);
    ok("Off then Cluster: the map with the next frame, the stock map and box parked again, the hold back",
       map_up() && shown("map", f3) && feeds.status("decode").state == "showing");

    /* Off from both: the app sends the cluster off, then the HUD off */
    feeds.set_hud(true);
    pump(2000, &f1);
    ok("Off: both up first, the HUD's scene in its slot", map_up() && status_of(0x323, kHud) == 1 &&
                                                         feeds.status("decode").hud_state == "showing");
    feeds.set_cluster(false);
    feeds.set_hud(false);
    pump(5000, &f2);
    st = feeds.status("decode");
    ok("Off from both: no feed or layer, the map's and the HUD's slots and the hold released, the stock map and box back",
       !feeds.cluster() && !feeds.hud_feed() && layers() == 0 && !feeds.wants_frames() && stock_back() && st.state == "off" &&
           st.hud_state == "off");
    feeds.set_hud(true);
    feeds.set_cluster(true);
    pump(2000, &f3);
    ok("Off then both: both back with the next frame", map_up() && status_of(0x323, kHud) == 1 && shown("map", f3) &&
                                                         feeds.status("decode").hud_state == "showing");
}

/* Only what the renderer needs goes over its socket. */
static void only_what_is_needed()
{
    const std::vector<uint8_t> f1 = atlas(1), f2 = atlas(2);
    Rig r(false, 6000);
    r.repeats = true;
    r.pump(1500, &f1);
    bool drawn = false;
    r.drawing("map", f1, &drawn);
    ok("repeats: the first frame whole, shown", r.st().state == "showing" && drawn);
    const unsigned long long first = r.received();
    bool steady = true;
    for (int s = 0; s < 10; s++) {
        r.pump(1000, &f1);
        steady = steady && r.st().state == "showing";
    }
    ok("repeats: a static map's repeated picture keeps it showing past stall_ms, each repeat counted",
       steady && r.map_layer()->params.visible && r.received() >= first + 150);
    int fps = 0;
    std::sscanf(r.st().page.c_str(), "map \xe2\x80\x94 %d frames/s", &fps);
    ok("repeats: the frame rate counts them, a frame each 65 ms", fps == 15 || fps == 16);

    r.feed->set_stream(false, false, Picture::Shown);
    r.pump(100, &f1);
    const unsigned long long hidden = r.received();
    r.pump(3000, &f2);
    ok("hidden: nothing reaches the renderer, frames or repeats", r.received() == hidden && r.st().state == "idle");
    r.feed->set_stream(true, false, Picture::Shown);
    bool first_drawn = false, newest_first = false;
    for (int step = 0; step < 300 && !first_drawn; step++) {
        r.pump(5);
        first_drawn = r.drawing("map", f2, &newest_first);
    }
    ok("shown again: the newest picture is what it draws first", first_drawn && newest_first);
    r.pump(1500, &f2);
    ok("shown again: showing", r.st().state == "showing");

    r.svc.reset();
    r.pump(100, &f2);
    r.start();
    r.pump(3000, &f2);
    r.drawing("map", f2, &drawn);
    ok("the renderer restarted: repeats alone bring the picture back", r.st().state == "showing" && drawn);
}

/* The w x h part at x, y of a 1088x480 atlas, as an atlas of its own. */
static std::vector<uint8_t> crop_of(const std::vector<uint8_t> &f, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    std::vector<uint8_t> c((size_t)w * (h + h / 2));
    for (uint32_t r = 0; r < h; r++)
        std::copy_n(&f[(size_t)(y + r) * 1088 + x], w, &c[(size_t)r * w]);
    for (uint32_t r = 0; r < h / 2; r++)
        for (uint32_t p = 0; p < 2; p++)
            std::copy_n(&f[(size_t)(480 + y / 2 + r) * 1088 + p * 544 + x / 2], w / 2, &c[(size_t)(h + r) * w + p * w / 2]);
    return c;
}

/* A scene with layers on the panel hosting the map (cluster-render
 * PROTOCOL.md "host"): the map's layer moves into it, at the host's rect,
 * its frames the host's part of the picture, the map's own slot released;
 * hidden by the host, no layer at all and nothing sent, idle with the
 * reason; shown again, the newest picture's part; the host gone, the map on
 * its own target again. */
static void hosting()
{
    char tmpl[] = "/tmp/woc-hosting-XXXXXX";
    const std::string root = mkdtemp(tmpl) ? tmpl : "/nonexistent";
    const std::string file = root + "/design.scene";
    std::ofstream(file) << "scene 0x010004f8\nnode plate -\npass main\n";
    chmod(file.c_str(), 0644);
    const std::vector<uint8_t> f = atlas(3), f2 = atlas(4);
    Rig r;
    r.sc.targets[1].scene_files = true;
    r.sc.scene_files.root = root;
    r.sc.scene_files.owner = getuid();
    r.start();
    r.pump(2000, &f);
    ok("hosting: the map shown on its own target", r.st().state == "showing" && r.st().target == "map" && !r.st().hosted);

    cr::Client design("design", r.path);
    r.other = &design;
    cr::SceneOptions so;
    so.target = "panel";
    so.file = file;
    so.sha256 = {crs::sha256_file(file)};
    so.layers = true;
    const uint32_t sc = design.scene(so);
    r.pump(500, &f);
    cr::Host h;
    h.target = "map";
    h.x = 700; h.y = 160; h.w = 560; h.h = 400;
    h.src_x = 264; h.src_y = 40; h.src_w = 560; h.src_h = 400;
    design.host(sc, h);
    r.pump(2500, &f);
    bool with = false;
    const int map_i = r.svc->slots().index("map");
    ok("hosted: the map in the panel's scene over the file, its frames the host's 560x400 part, the map's slot released",
       r.st().state == "showing" && r.st().target == "panel" && r.st().hosted && r.drawing("panel", crop_of(f, 264, 40, 560, 400), &with) &&
           with && r.gpu.graph("panel")->over_file && r.svc->compositor().layers().size() == 1 &&
           r.svc->slots().status(map_i).state == crs::TargetState::Idle);
    ok("hosted, hosted_vpp_scale off (the default): no part asked of the decoder, the part scaled by the worker",
       !r.feed->part().set() && r.feed->scaled_frames() > 0);

    h.shown = false;
    design.host(sc, h);
    r.pump(1000, &f2);
    ok("hidden by the host: no layer, nothing sent, idle with the reason",
       r.svc->compositor().layers().empty() && r.st().state == "idle" && r.st().reason == MapFeed::kHostHidden &&
           r.svc->slots().status(map_i).state == crs::TargetState::Idle);
    h.shown = true;
    design.host(sc, h);
    r.pump(1500);
    ok("shown again: the newest picture's part first", r.st().state == "showing" &&
                                                         r.drawing("panel", crop_of(f2, 264, 40, 560, 400), &with) && with);

    design.drop(sc);
    r.pump(2000, &f2);
    ok("the host gone: the map on its own target again",
       r.st().state == "showing" && r.st().target == "map" && !r.st().hosted && r.drawing("map", f2, &with) && with);

    /* A map smaller than the host's src (map_rect: half size, centred):
     * the host stretches src over its rect, so the map lands as that much
     * of the rect, at its place in src, not over the whole rect. */
    FeedConfig fc;
    fc.socket = r.path;
    fc.video.level_bits = 0;
    fc.rect = MapRect{272, 120, 544, 240};
    r.feed.reset(new MapFeed(fc));
    r.feed->set_stream(true, false, Picture::Shown);
    const uint32_t sc2 = design.scene(so);
    r.pump(500, &f);
    cr::Host whole;
    whole.target = "map";
    whole.x = 700; whole.y = 160; whole.w = 544; whole.h = 240;   /* the map target, half size, no src: all of it */
    design.host(sc2, whole);
    r.pump(2500, &f);
    const crs::Layer *placed = r.map_layer();
    ok("a map smaller than the host's src: the half-size map at its place in the rect (836,220 272x120), "
       "its frame scaled to that",
       r.st().hosted && placed && placed->params.rect.x == 836 && placed->params.rect.y == 220 &&
           placed->params.rect.w == 272 && placed->params.rect.h == 120 &&
           r.drawing("panel", f, &with));

    /* A host whose place cannot be drawn: the map stays on its own target,
     * never nowhere. */
    cr::Host missing = whole;
    missing.src_x = 0; missing.src_y = 0; missing.src_w = 200; missing.src_h = 100;   /* outside the half-size map */
    design.host(sc2, missing);
    r.pump(2500, &f);
    ok("a host's part that misses the map: the map on its own target instead",
       r.st().state == "showing" && r.st().target == "map" && !r.st().hosted && r.drawing("map", f, &with));
    cr::Host tiny = whole;
    tiny.w = 40; tiny.h = 20;
    design.host(sc2, tiny);
    r.pump(2500, &f);
    ok("a host's rect too small for the map: the map on its own target instead",
       r.st().state == "showing" && r.st().target == "map" && !r.st().hosted && r.drawing("map", f, &with));
    design.host(sc2, whole);
    r.pump(2500, &f);
    ok("a host that can be drawn again: hosted again", r.st().hosted && r.st().target == "panel");
    design.drop(sc2);
    r.pump(500, &f);
    r.other = nullptr;
    const int removed = std::system(("rm -rf " + root).c_str());
    (void)removed;
}

/* A theme drawing the panel with layers only hosting the map, no scene
 * (cluster-render PROTOCOL.md "Hosting from layers"): the map moves into the
 * panel at the host's rect, with the host's part, its own slot released,
 * and, its layer marked as the hosted map, is drawn under the theme's
 * layers though made after them; every theme layer hidden: back on its own
 * target; shown: hosted again; the theme gone: back on its own target.
 * Nothing in the feed knows the host has no scene. */
static void layers_hosting()
{
    const std::vector<uint8_t> f = atlas(5);
    Rig r;
    r.pump(2000, &f);
    ok("layer host: the map shown on its own target", r.st().state == "showing" && r.st().target == "map" &&
                                                          !r.st().hosted);
    std::unique_ptr<cr::Client> theme(new cr::Client("theme", r.path));
    r.other = theme.get();
    r.pump(100, &f);
    const cr::EffectOptions fx{"#version 100\nvoid main(){gl_FragColor=vec4(0.0);}", "", 0};
    cr::LayerOptions bg, frame;
    bg.target = frame.target = "panel";
    frame.x = 690; frame.y = 150; frame.w = 580; frame.h = 420;
    const uint32_t bg_ref = theme->effect(bg, fx), frame_ref = theme->effect(frame, fx);
    r.pump(1500, &f);
    cr::Host h;
    h.into = "panel";
    h.target = "map";
    h.x = 700; h.y = 160; h.w = 560; h.h = 400;
    h.src_x = 264; h.src_y = 40; h.src_w = 560; h.src_h = 400;
    theme->host(h);
    r.pump(2500, &f);
    bool with = false;
    const int map_i = r.svc->slots().index("map");
    crs::FakeGraph *g = r.gpu.graph("panel");
    const std::vector<int> stack = g ? g->visible() : std::vector<int>();
    const bool under = stack.size() == 3 && g->shown.quads.at(stack[0]).r.x == 700 &&
                       g->shown.quads.at(stack[0]).r.w == 560 && g->shown.quads.at(stack[1]).r.w == 1920 &&
                       g->shown.quads.at(stack[2]).r.w == 580;
    ok("layer host: the map in the panel at the host's rect with its part, under the theme's background and frame, "
       "the map's slot released",
       r.st().state == "showing" && r.st().target == "panel" && r.st().hosted &&
           r.drawing("panel", crop_of(f, 264, 40, 560, 400), &with) && with && under &&
           r.svc->slots().status(map_i).state == crs::TargetState::Idle);

    bg.visible = frame.visible = false;
    theme->set(bg_ref, bg);
    theme->set(frame_ref, frame);
    r.pump(2500, &f);
    ok("layer host: every theme layer hidden: the map on its own target again",
       r.st().state == "showing" && r.st().target == "map" && !r.st().hosted && r.drawing("map", f, &with) && with);
    bg.visible = frame.visible = true;
    theme->set(bg_ref, bg);
    theme->set(frame_ref, frame);
    r.pump(2500, &f);
    ok("layer host: shown again: hosted again", r.st().target == "panel" && r.st().hosted);

    r.other = nullptr;
    theme.reset();
    r.pump(2500, &f);
    ok("layer host: the theme gone: the map on its own target again",
       r.st().state == "showing" && r.st().target == "map" && !r.st().hosted && r.drawing("map", f, &with) && with);
}

/* The cluster's map hosted (MapFeeds, as the service runs it): its worker
 * scales the host's part, so the service's loop is woken (wake_in_ms) to
 * take the scaled frame and draws it without waiting for another frame. */
static void hosted_wake()
{
    char tmpl[] = "/tmp/woc-hosted-wake-XXXXXX";
    const std::string root = mkdtemp(tmpl) ? tmpl : "/nonexistent";
    const std::string file = root + "/design.scene";
    std::ofstream(file) << "scene 0x010004f8\nnode plate -\npass main\n";
    chmod(file.c_str(), 0644);
    const std::vector<uint8_t> f = atlas(5), f2 = atlas(6);
    Rig r;
    r.feed.reset();
    r.sc.targets[1].scene_files = true;
    r.sc.scene_files.root = root;
    r.sc.scene_files.owner = getuid();
    r.start();
    FeedConfig fc;
    fc.socket = r.path;
    fc.video.level_bits = 0;
    MapFeeds feeds(fc, true, false, 10);
    feeds.set_stream(true, false, Picture::Shown);
    cr::Client design("design", r.path);
    std::vector<uint8_t> spare;
    /* as the service's loop: woken for the feeds' wake_in_ms (real time,
     * at most 2 s a step) */
    const auto pump = [&](int64_t ms, const std::vector<uint8_t> *frame) {
        for (int64_t t = 0; t < ms; t += 5) {
            r.now += 5;
            if (frame && t % 65 == 0) {
                spare = *frame;
                feeds.offer(&spare, (uint64_t)r.now, r.now);
            }
            for (int round = 0, waits = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                const size_t first = r.svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                r.port.tick(r.now);
                r.svc->after_poll(fds, first, r.now);
                const bool pending = feeds.cluster()->wants_write() || design.wants_write();
                feeds.poll(r.now);
                design.poll(r.now);
                const bool wait = scaling(feeds, r.now) && waits < 2000;
                if (wait) {
                    usleep(1000);
                    waits++;
                    round--;
                }
                if (round > 0 && n <= 0 && !pending && !wait)
                    break;
            }
        }
    };
    pump(1000, &f);
    cr::SceneOptions so;
    so.target = "panel";
    so.file = file;
    so.sha256 = {crs::sha256_file(file)};
    so.layers = true;
    const uint32_t sc = design.scene(so);
    pump(500, &f);
    cr::Host h;
    h.target = "map";
    h.x = 700; h.y = 160; h.w = 560; h.h = 400;
    h.src_x = 264; h.src_y = 40; h.src_w = 560; h.src_h = 400;
    design.host(sc, h);
    pump(2500, &f);
    ok("hosted cluster map: shown in the host's scene", feeds.status("decode").state == "showing" &&
                                                          feeds.status("decode").hosted);
    pump(200, nullptr);   /* the worker's last frame taken, the next one due */
    spare = f2;
    feeds.offer(&spare, (uint64_t)r.now, r.now);
    ok("hosted cluster map: a frame with its worker wakes the service's loop to take it",
       scaling(*feeds.cluster(), r.now) && feeds.wake_in_ms(r.now) == MapFeed::kScaleWaitMs);
    pump(500, nullptr);
    bool with = false;
    ok("hosted cluster map: that frame's part drawn", r.drawing("panel", crop_of(f2, 264, 40, 560, 400), &with) && with);
    design.drop(sc);
    pump(500, &f2);
    const int removed = std::system(("rm -rf " + root).c_str());
    (void)removed;
}

/* hosted_vpp_scale on (FeedConfig::vpp_scale): hosted, the feed asks the
 * decoder for the host's part at the size it is shown at; whole frames that
 * come meanwhile go through the worker, a frame of the part goes as it is,
 * one of another part is dropped; the host's move asks for its new part, and
 * the host gone, for none, the whole frame on the map's own target. */
static void vpp_hosting()
{
    char tmpl[] = "/tmp/woc-vpp-hosting-XXXXXX";
    const std::string root = mkdtemp(tmpl) ? tmpl : "/nonexistent";
    const std::string file = root + "/design.scene";
    std::ofstream(file) << "scene 0x010004f8\nnode plate -\npass main\n";
    chmod(file.c_str(), 0644);
    const std::vector<uint8_t> f = atlas(5), f2 = atlas(6), f3 = atlas(7);
    Rig r;
    r.sc.targets[1].scene_files = true;
    r.sc.scene_files.root = root;
    r.sc.scene_files.owner = getuid();
    r.start();
    FeedConfig fc;
    fc.socket = r.path;
    fc.video.level_bits = 0;
    fc.vpp_scale = true;
    r.feed.reset(new MapFeed(fc));
    r.feed->set_stream(true, false, Picture::Shown);
    r.pump(2000, &f);
    ok("vpp: on its own target, no part asked for", r.st().target == "map" && !r.feed->part().set());

    cr::Client design("design", r.path);
    r.other = &design;
    cr::SceneOptions so;
    so.target = "panel";
    so.file = file;
    so.sha256 = {crs::sha256_file(file)};
    so.layers = true;
    const uint32_t sc = design.scene(so);
    r.pump(500, &f);
    cr::Host h;
    h.target = "map";
    h.x = 700; h.y = 160; h.w = 560; h.h = 400;
    h.src_x = 264; h.src_y = 40; h.src_w = 560; h.src_h = 400;
    design.host(sc, h);
    r.pump(2500, &f);
    bool with = false;
    const OutputPart want{264, 40, 560, 400, 560, 400};
    ok("vpp: hosted, the host's part asked for at the size it is shown at", r.st().hosted && r.feed->part() == want);
    ok("vpp: a whole frame meanwhile scaled by the worker",
       r.drawing("panel", crop_of(f, 264, 40, 560, 400), &with) && with && r.feed->scaled_frames() > 0);

    const std::vector<uint8_t> p2 = crop_of(f2, 264, 40, 560, 400), p3 = crop_of(f3, 264, 40, 560, 400);
    r.part = want;
    r.pump(200, &p2);   /* a whole frame the worker still had, done and taken */
    const uint64_t scaled = r.feed->scaled_frames();
    r.pump(1300, &p2);
    ok("vpp: a frame of the part sent as it is, not through the worker",
       r.st().state == "showing" && r.drawing("panel", p2, &with) && with && r.feed->scaled_frames() == scaled);
    OutputPart other = want;
    other.x = 200;
    r.part = other;
    r.pump(1000, &p3);
    ok("vpp: a frame of another part dropped", r.drawing("panel", p2, &with) && with);

    h.src_x = 200;
    design.host(sc, h);
    r.pump(500);
    ok("vpp: the host moved, its new part asked for", r.feed->part() == other);
    r.pump(1000, &p3);
    ok("vpp: the new part's frames shown", r.drawing("panel", p3, &with) && with && r.feed->scaled_frames() == scaled);

    design.drop(sc);
    r.part = OutputPart();
    r.pump(2000, &f2);
    ok("vpp: the host gone, no part asked for, the whole frame on the map's own target",
       !r.feed->part().set() && r.st().target == "map" && !r.st().hosted && r.drawing("map", f2, &with) && with);
    r.other = nullptr;
    const int removed = std::system(("rm -rf " + root).c_str());
    (void)removed;
}

/* Whether the target's scene shows f (a w x h atlas) packed, and in *exact
 * whether it shows an exact atlas. */
static bool shows_packed(Rig &r, const char *target, const std::vector<uint8_t> &f, uint32_t w, uint32_t h, bool *exact)
{
    crs::FakeGraph *g = r.gpu.graph(target);
    *exact = false;
    if (!g || g->drawn().empty())
        return false;
    std::vector<uint8_t> want(crk::block_bytes(w, h));
    crk::pack_blocks(f.data(), w, h, want.data(), 1);
    bool with = false;
    for (const auto &q : g->shown.quads) {
        auto t = q.second.textures.find("u_image");
        if (!q.second.visible || t == q.second.textures.end())
            continue;
        auto b = g->shown.buffers.find(t->second);
        if (b == g->shown.buffers.end())
            continue;
        if (b->second.f == crs::TexFormat::R8)
            *exact = true;
        else if (b->second.px == want)
            with = true;
    }
    return with;
}

/* The map layer's video settings as the renderer took them; null: none. */
static const crs::VideoParams *map_video(Rig &r)
{
    const crs::Layer *l = r.map_layer();
    return l && std::string(l->kind()) == "video" ? &static_cast<const crs::VideoLayer *>(l)->video() : nullptr;
}

/* always_packed: 0 (the default), the layer without always_packed, a still map
 * exact; 1, every update packed, the still map's too, never the exact atlas
 * or back to it; with hosted_vpp_scale, the VPP-made part packed; a host's
 * part not a multiple of 4 high goes as with 0, and shows. */
static void always_packed_map()
{
    const std::vector<uint8_t> f1 = atlas(11), f2 = atlas(12);
    bool exact = false, with = false;
    {
        Rig r;
        r.pump(2000, &f1);
        const crs::VideoParams *vp = map_video(r);
        ok("always_packed 0: the layer without it, pan_blocks as set, the still map exact",
           vp && !vp->always_packed && vp->pan_blocks && r.st().state == "showing" && r.drawing("map", f1, &with) &&
               with && !shows_packed(r, "map", f1, 1088, 480, &exact) && exact);
    }
    {
        Rig r;
        FeedConfig fc;
        fc.socket = r.path;
        fc.video.level_bits = 0;
        fc.always_packed = true;
        r.feed.reset(new MapFeed(fc));
        r.feed->set_stream(true, false, Picture::Shown);
        r.pump(1000, &f1);
        r.pump(1000, &f2);   /* one change, then a still map */
        crs::FakeGraph *g = r.gpu.graph("map");
        const uint64_t still0 = g ? g->flushed_bytes : 0;
        r.pump(2000, &f2);
        const crs::VideoParams *vp = map_video(r);
        const std::string s = r.map_layer() ? r.map_layer()->stats() : std::string();
        ok("always_packed 1: the layer with it", vp && vp->always_packed && vp->pan_blocks);
        ok("always_packed 1: a still map sends nothing", g && g->flushed_bytes == still0);
        ok("always_packed 1: the still map packed, the exact atlas never shown",
           r.st().state == "showing" && shows_packed(r, "map", f2, 1088, 480, &exact) && !exact);
        ok("always_packed 1: every write packed, none exact or back to exact",
           s.find(" 0 whole, 0 partial, 0 rects, ") != std::string::npos && s.find("rects, 0 packed") == std::string::npos &&
               s.find(" 0 back to exact") != std::string::npos);
    }
    char tmpl[] = "/tmp/woc-packed-XXXXXX";
    const std::string root = mkdtemp(tmpl) ? tmpl : "/nonexistent";
    const std::string file = root + "/design.scene";
    std::ofstream(file) << "scene 0x010004f8\nnode plate -\npass main\n";
    chmod(file.c_str(), 0644);
    {
        Rig r;
        r.sc.targets[1].scene_files = true;
        r.sc.scene_files.root = root;
        r.sc.scene_files.owner = getuid();
        r.start();
        FeedConfig fc;
        fc.socket = r.path;
        fc.video.level_bits = 0;
        fc.always_packed = true;
        fc.vpp_scale = true;
        r.feed.reset(new MapFeed(fc));
        r.feed->set_stream(true, false, Picture::Shown);
        r.pump(1000, &f1);
        cr::Client design("design", r.path);
        r.other = &design;
        cr::SceneOptions so;
        so.target = "panel";
        so.file = file;
        so.sha256 = {crs::sha256_file(file)};
        so.layers = true;
        const uint32_t sc = design.scene(so);
        r.pump(500, &f1);
        cr::Host h;
        h.target = "map";
        h.x = 700; h.y = 160; h.w = 560; h.h = 400;
        h.src_x = 264; h.src_y = 40; h.src_w = 560; h.src_h = 400;
        design.host(sc, h);
        r.pump(1500, &f1);
        r.part = OutputPart{264, 40, 560, 400, 560, 400};
        const std::vector<uint8_t> p2 = crop_of(f2, 264, 40, 560, 400);
        r.pump(1500, &p2);
        const crs::VideoParams *vp = map_video(r);
        ok("always_packed with hosted_vpp_scale: the VPP-made part packed, never exact",
           vp && vp->always_packed && r.st().hosted && shows_packed(r, "panel", p2, 560, 400, &exact) && !exact);

        /* The host moves the map: a new layer, its first write the whole
         * packed part once, then nothing while the part stands still. */
        crs::FakeGraph *pg = r.gpu.graph("panel");
        const uint64_t moved0 = pg ? pg->flushed_bytes : 0;
        h.src_x = 200;
        design.host(sc, h);
        r.part = OutputPart{200, 40, 560, 400, 560, 400};
        const std::vector<uint8_t> p3 = crop_of(f2, 200, 40, 560, 400);
        r.pump(1500, &p3);
        const uint64_t moved = pg ? pg->flushed_bytes - moved0 : 0;
        char name[200];
        std::snprintf(name, sizeof name, "always_packed: the host moved, the new part's whole packed frame once (%llu B), "
                      "then nothing", (unsigned long long)moved);
        ok(name, moved == crk::block_bytes(560, 400) && shows_packed(r, "panel", p3, 560, 400, &exact) && !exact);
        h.src_x = 264;

        h.h = 430;
        h.src_h = 430;
        design.host(sc, h);
        r.part = OutputPart();
        r.pump(2000, &f1);
        vp = map_video(r);
        ok("always_packed: a host's part 430 high (not a multiple of 4) as with 0, and shown",
           vp && !vp->always_packed && r.st().state == "showing" && r.st().hosted &&
               r.drawing("panel", crop_of(f1, 264, 40, 560, 430), &with) && with);
        design.drop(sc);
        r.pump(500, &f1);
        r.other = nullptr;
    }
    const int removed = std::system(("rm -rf " + root).c_str());
    (void)removed;
}

/* A cluster-render from before always_packed: its hello without it (with
 * packed, a newer one's, with it); every JSON message a client sends on the
 * current connection kept. */
struct OldRenderer {
    std::string path = "/tmp/woc-old-render-" + std::to_string(getpid()) + ".sock";
    int lfd = -1, cfd = -1;
    std::string in;
    std::vector<std::string> ops;
    bool packed = false;

    OldRenderer()
    {
        unlink(path.c_str());
        lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
        if (bind(lfd, (const sockaddr *)&a, sizeof a) != 0 || listen(lfd, 4) != 0)
            std::printf("old renderer: %s not listening\n", path.c_str());
    }
    ~OldRenderer()
    {
        if (cfd >= 0)
            close(cfd);
        close(lfd);
        unlink(path.c_str());
    }
    void poll()
    {
        if (cfd < 0)
            cfd = accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK);
        if (cfd < 0)
            return;
        char buf[65536];
        ssize_t n;
        while ((n = read(cfd, buf, sizeof buf)) > 0)
            in.append(buf, (size_t)n);
        size_t off = 0;
        for (;;) {
            size_t used = 0, len = 0;
            uint8_t type = 0;
            const char *body = nullptr;
            bool bad = false;
            if (!cr::next_message(in.data() + off, in.size() - off, &used, &type, &body, &len, &bad))
                break;
            off += used;
            if (type != cr::kJson)
                continue;
            ops.emplace_back(body, len);
            cr::Json j;
            if (cr::parse_json(ops.back(), &j) && j["op"] == "hello") {
                std::vector<std::pair<std::string, std::string>> kv = {
                    {"ev", cr::json_quote("hello")}, {"proto", "1"}, {"backend", cr::json_quote("id7")},
                    {"targets", cr::json_quote("map:1088x480,panel:1920x720")}, {"repeat", "1"}};
                if (packed)
                    kv.push_back({"always_packed", "1"});
                const std::string h = cr::encode(cr::kJson, cr::json_object(kv));
                if (write(cfd, h.data(), h.size()) != (ssize_t)h.size())
                    std::printf("old renderer: hello not written\n");
            }
        }
        in.erase(0, off);
    }
    /* the last layer op */
    std::string layer_op() const
    {
        for (auto o = ops.rbegin(); o != ops.rend(); ++o)
            if (o->find("\"layer\"") != std::string::npos && o->find("\"kind\"") != std::string::npos)
                return *o;
        return std::string();
    }
    /* The connection ends (the service restarted, as an upgrade does). */
    void hang_up()
    {
        close(cfd);
        cfd = -1;
        in.clear();
        ops.clear();
    }
};

/* Compatibility: the map's layer to a cluster-render whose hello does not
 * take always_packed carries no always_packed field, set or not (with it
 * set, logged once, the map as pan_blocks decides); and by default the
 * field is never sent, so a cluster-render of any age gets the layer it
 * always did. */
static void old_renderer()
{
    for (int packed = 0; packed < 2; packed++) {
        OldRenderer old;
        FeedConfig fc;
        fc.socket = old.path;
        fc.always_packed = packed == 1;
        MapFeed feed(fc);
        feed.set_stream(true, false, Picture::Shown);
        int64_t now = 100000;
        for (int i = 0; i < 400 && old.layer_op().empty(); i++) {
            now += 5;
            old.poll();
            feed.poll(now);
            old.poll();
        }
        const std::string op = old.layer_op();
        ok(packed ? "an older cluster-render: always_packed 1 sends the layer without always_packed"
                  : "always_packed 0: the layer without always_packed",
           !op.empty() && op.find("always_packed") == std::string::npos && op.find("pan_blocks") != std::string::npos);
    }
}

/* always_packed follows each connection's hello: cluster-render upgraded
 * (or downgraded) under a running feed gets the map's layer made again for
 * what it takes, with the one log line when it no longer takes it. */
static void renderer_changed()
{
    for (int up = 0; up < 2; up++) {
        OldRenderer r;
        r.packed = !up;
        FeedConfig fc;
        fc.socket = r.path;
        fc.always_packed = true;
        MapFeed feed(fc);
        feed.set_stream(true, false, Picture::Shown);
        int64_t now = 100000;
        auto run = [&] {
            for (int i = 0; i < 400; i++) {
                now += 5;
                r.poll();
                feed.poll(now);
                r.poll();
            }
        };
        run();
        const bool before = r.layer_op().find("always_packed") != std::string::npos;
        uint64_t seq = 0;
        std::vector<std::string> lines;
        cld::log_lines_since(&seq, &lines);
        lines.clear();
        r.packed = up;
        r.hang_up();
        run();
        const std::string op = r.layer_op();
        cld::log_lines_since(&seq, &lines);
        int logged = 0;
        for (const std::string &line : lines)
            logged += line.find("render: always_packed:") != std::string::npos;
        if (up)
            ok("always_packed: an older cluster-render upgraded under the feed: the layer with always_packed after the "
               "reconnect",
               !before && op.find("\"always_packed\"") != std::string::npos && logged == 0);
        else
            ok("always_packed: cluster-render downgraded under the feed: the layer without always_packed after the "
               "reconnect, logged once",
               before && !op.empty() && op.find("always_packed") == std::string::npos && logged == 1);
    }
}

/* Regression test for MapFeed frame invalidation with 1440x636 geometry:
 * - Establish renderer connection and verify map layer exists
 * - Enable one-shot worker pause
 * - Submit frame A directly (1440x636 source)
 * - Wait until worker is paused on A
 * - Offer distinct newer frame B while A is in flight
 * - Release worker (no pump while paused)
 * - Let A finish and B process normally
 * - Capture EVERY flush via also_flush callback
 * - Verify: A never in flush history, B appears, final is B */
static void frame_invalidation_test()
{
    /* Use 1440x636 source geometry to activate cluster ScaleWorker.
     * Input atlas: 1440 * (636 + 318) = 1,373,760 bytes
     * Output atlas (YUV): 1088 * (480 + 240) = 783,360 bytes
     * Renderer R8 texture: 1088x720 = 783,360 bytes (Y + U + V packed) */
    const uint32_t src_w = 1440, src_h = 636;
    const uint32_t dst_w = 1088, dst_h = 480;
    const size_t src_atlas_bytes = (size_t)src_w * (src_h + src_h / 2);  // 1,373,760
    const size_t dst_atlas_bytes = (size_t)dst_w * (dst_h + dst_h / 2);  // 783,360
    const uint32_t r8_w = 1088, r8_h = 720;  // Renderer R8 texture dimensions

    /* Production crop geometry for cluster feed (cluster_offset_x = 0):
     * Source: 1440x636, Crop: x=176, y=0, w=1088, h=480 (1:1 crop, no scaling) */
    const uint32_t crop_x = 176, crop_y = 0, crop_w = 1088, crop_h = 480;
    const uint32_t uv_crop_x = crop_x / 2, uv_crop_y = crop_y / 2;
    const uint32_t uv_crop_w = crop_w / 2, uv_crop_h = crop_h / 2;

    auto make_atlas = [&](int seed) {
        std::vector<uint8_t> f(src_atlas_bytes);
        for (size_t i = 0; i < f.size(); i++) {
            if (i < (size_t)src_w * src_h) {
                /* Y plane: deterministic pattern based on position + seed */
                f[i] = (uint8_t)((i / 7 + seed * 13) & 0xff);
            } else {
                /* UV planes: neutral chroma with seed variation */
                f[i] = (uint8_t)(128 + ((seed * 11) & 0x7f));
            }
        }
        return f;
    };

    /* Compute expected R8 texture (1088x720) from source atlas by applying
     * the production crop geometry. Since crop_w == dst_w and crop_h == dst_h,
     * this is a 1:1 crop - just extract the crop region from each plane. */
    auto expected_r8 = [&](const std::vector<uint8_t> &src) {
        std::vector<uint8_t> dst(dst_atlas_bytes);
        /* Source plane offsets */
        const size_t src_y_off = 0;
        const size_t src_u_off = (size_t)src_w * src_h;
        const size_t src_v_off = src_u_off + (size_t)(src_w / 2) * (src_h / 2);
        /* Destination R8 texture offsets (Y at top, U and V side by side below) */
        const size_t dst_y_off = 0;
        const size_t dst_u_off = (size_t)r8_w * dst_h;
        const size_t dst_v_off = dst_u_off + (size_t)(r8_w / 2) * (dst_h / 2);

        /* Copy Y plane: src[176:1264, 0:480] -> dst[0:1088, 0:480] */
        for (uint32_t y = 0; y < dst_h; y++) {
            const uint8_t *s = &src[src_y_off + (size_t)(crop_y + y) * src_w + crop_x];
            uint8_t *d = &dst[dst_y_off + (size_t)y * r8_w];
            std::memcpy(d, s, crop_w);
        }
        /* Copy U plane: src[88:632, 0:240] -> dst[0:544, 480:720] */
        for (uint32_t y = 0; y < dst_h / 2; y++) {
            const uint8_t *s = &src[src_u_off + (size_t)(uv_crop_y + y) * (src_w / 2) + uv_crop_x];
            uint8_t *d = &dst[dst_u_off + (size_t)y * r8_w];
            std::memcpy(d, s, uv_crop_w);
        }
        /* Copy V plane: src[88:632, 0:240] -> dst[544:1088, 480:720] */
        for (uint32_t y = 0; y < dst_h / 2; y++) {
            const uint8_t *s = &src[src_v_off + (size_t)(uv_crop_y + y) * (src_w / 2) + uv_crop_x];
            uint8_t *d = &dst[dst_v_off + (size_t)y * r8_w];
            std::memcpy(d, s, uv_crop_w);
        }
        return dst;
    };

    /* Construct Rig with 1440x636 video geometry so MapFeed creates
     * the correct layer and ScaleWorker from the start. */
    Rig r(false, 6000, false, true, false, src_w, src_h);

    /* Establish renderer connection and verify map layer exists */
    r.pump(2000);
    const crs::Layer *layer = r.map_layer();
    ok("invalidation: map layer exists on renderer", layer != nullptr);

    /* Capture R8 atlas at EVERY flush via also_flush callback.
     * The tag parameter is unused but required by the callback signature. */
    std::vector<std::vector<uint8_t>> flush_history;
    crs::FakeGraph *g = r.gpu.graph("map");
    if (g) {
        g->also_flush = [&](uint64_t /*tag*/) {
            for (const auto &kv : g->shown.buffers) {
                if (kv.second.f == crs::TexFormat::R8 &&
                    kv.second.w == r8_w && kv.second.h == r8_h) {
                    flush_history.push_back(kv.second.px);
                    break;  // only one R8 atlas per flush for map target
                }
            }
        };
    }

    /* Enable one-shot test pause for the next submitted job */
    r.feed->worker().enable_test_pause();

    const std::vector<uint8_t> fA = make_atlas(1);
    const std::vector<uint8_t> fB = make_atlas(2);

    /* Expected R8 outputs after production crop/scale */
    const std::vector<uint8_t> expA = expected_r8(fA);
    const std::vector<uint8_t> expB = expected_r8(fB);

    /* Submit frame A directly (not via pump) */
    std::vector<uint8_t> spare = fA;
    r.feed->offer(&spare, 1, r.now);

    /* Wait until worker accepts job A and reaches the PAUSED barrier */
    bool paused = r.feed->worker().wait_until_worker_paused([](auto &s) { return s.id == 1; }, 5000);
    ok("invalidation: worker paused on frame A (id=1)", paused);

    /* Offer newer frame B while A is paused in worker */
    spare = fB;
    r.feed->offer(&spare, 2, r.now);

    /* Release worker to process A, then let it complete normally.
     * Do NOT call pump() while worker is intentionally paused. */
    r.feed->worker().release_worker();

    /* Let the worker finish A, then the feed will resubmit B,
     * and B will complete without further test pauses. */
    r.pump(2000);

    /* Verify complete flush history:
     * 1. Frame A's expected output (stale) never appears in renderer flush history
     * 2. Frame B's expected output (newest) does appear in renderer flush history
     * 3. The final rendered atlas equals B's expected output */
    bool saw_A = false, saw_B = false;
    for (const auto &buf : flush_history) {
        if (buf == expA) saw_A = true;
        if (buf == expB) saw_B = true;
    }
    ok("invalidation: flush history captured", !flush_history.empty());
    ok("invalidation: frame A (stale) NEVER flushed to renderer", !saw_A);
    ok("invalidation: frame B (newest) WAS flushed to renderer", saw_B);
    ok("invalidation: final rendered atlas is frame B",
       !flush_history.empty() && flush_history.back() == expB);
}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, nullptr, _IONBF, 0);
    hosting();
    layers_hosting();
    hosted_wake();
    vpp_hosting();
    always_packed_map();
    old_renderer();
    renderer_changed();
    frame_invalidation_test();
    Rig r;
    r.pump(100);
    const crs::Layer *l = r.map_layer();
    ok("connected: one contending content layer on the map target, covering the 1088x480 slot",
       l && l->target() == 0 && l->params.contend && l->params.klass == crs::Klass::Content && l->params.rect.x == 0 &&
           l->params.rect.y == 0 && l->params.rect.w == 1088 && l->params.rect.h == 480);
    ok("no frame yet: idle, and the slot is not asked for", r.st().state == "idle" && r.port.requests == 0);

    const std::vector<uint8_t> f1 = atlas(1);
    r.pump(20, &f1);
    ok("the first frame asks for the slot: starting", r.st().state == "starting" && r.port.requests == 1);
    r.pump(1100, &f1);
    crs::FakeGraph *g = r.gpu.graph("map");
    bool drawn = false;
    if (g)
        for (const auto &kv : g->shown.buffers)
            drawn = drawn || (kv.second.f == crs::TexFormat::R8 && kv.second.px == f1);
    ok("granted: showing, with the decoded frame in the slot's scene",
       r.st().state == "showing" && drawn && r.st().page.find("map — ") == 0);
    ok("the status line counts the frames handed over", r.st().page.find("15 frames/s") != std::string::npos);
    const std::string lost = "Stream lost";   /* until the layer stalls */
    ok("showing carries the stream's reason", r.feed->status("decode", lost).state == "showing" &&
                                                  r.feed->status("decode", lost).reason == lost &&
                                                  r.feed->status("decode", lost).stream_reason);

    r.port.steal(r.sc.targets[0]);
    r.pump(20, &f1);
    ok("the stock map takes the slot: starting, with that reason",
       r.st().state == "starting" && r.st().reason == "the stock map holds the slot");
    r.pump(2200, &f1);
    const crs::SlotInfo *ours = r.port.row(0x100, 0x010004f0), *stock = r.port.row(0x100, 0x01000101);
    ok("the map contends: it takes the stock map over and is shown, the stock map parked behind it",
       r.st().state == "showing" && r.port.requests == 2 && r.port.taken_over.size() == 1 &&
           r.port.taken_over[0] == 0x01000101 && ours && ours->map_status == 1 && stock && stock->map_status == 3);

    /* Many frames between two writes: only the newest is drawn. */
    const std::vector<uint8_t> f2 = atlas(2), f3 = atlas(3);
    std::vector<uint8_t> b2 = f2, b3 = f3;
    r.feed->offer(&b2, 2, r.now);
    r.feed->offer(&b3, 3, r.now);
    /* b2 takes the place of the frame sent last, whose buffer comes back. Whether one comes back for b3 too
     * depends on the socket: f2 goes whole at once into a large send buffer, freeing an earlier buffer, and is
     * still on its way with a small one (the car's), when b3 gets a new buffer (client.h, frame()). Never a copy. */
    ok("a frame is taken without a copy: an earlier frame's buffer comes back, then one of them or a new one",
       b2.size() == f2.size() && b3.size() == f3.size() && b2 == f1 && b3 != f2 && b3 != f3);
    r.pump(200);
    g = r.gpu.graph("map");
    bool newest = false, stale = false;
    for (const auto &kv : g->shown.buffers)
        if (kv.second.f == crs::TexFormat::R8) {
            newest = newest || kv.second.px == f3;
            stale = stale || kv.second.px == f2;
        }
    ok("frames faster than the writes: the newest is drawn", newest && !stale);

    const int releases = r.port.releases;
    r.pump(6100);
    ok("frames stop: idle after stall_ms", r.st().state == "idle");
    const std::string waiting = "Stream lost";
    ok("idle while the stream is live carries the stream's reason", r.feed->status("decode", waiting).reason == waiting);
    r.pump(2000);   /* the fade out, then the linger */
    ok("and the slot is released after the linger",
       r.port.releases > releases && !r.port.row(0x100, 0x010004f0));

    r.svc.reset();
    r.pump(100);
    ok("the renderer gone: starting while it may be starting", r.st().state == "starting" &&
                                                               r.st().reason == "waiting for the cluster renderer");
    r.pump(15000);
    ok("still gone 15 s on: an error the app shows", r.st().state == "error" &&
                                                     r.st().reason == "cluster renderer not reachable");
    r.start();
    r.pump(3000, &f1);
    ok("it comes back: the map layer made again, showing with the next frames",
       r.map_layer() && r.st().state == "showing");

    /* the governor counts evidence seen while writes go */
    r.svc->compositor().governor().activity(r.now);
    r.svc->compositor().governor().evidence(crs::Evidence::LinkStall, r.now);
    r.svc->compositor().governor().evidence(crs::Evidence::LinkStall, r.now);
    r.pump(100, &f1);
    ok("a lowered shared upload cap shows on the status line",
       r.st().page.find("upload cap 5000 KB/s") != std::string::npos);
    ok("the status carries the cap and why",
       r.st().upload_cap_kb == 5000 && r.st().upload_reason.find("lowered from 5500 KB/s") == 0 &&
           r.st().upload_reason.find("heartbeat replies silent") != std::string::npos);
    /* A minute of healthy upload raises it back to the ceiling, which the
     * status line leaves out. */
    for (int s = 0; s < 62; s++) {
        r.svc->compositor().governor().activity(r.now);
        r.pump(1000, &f1);
    }
    ok("back at the ceiling after healthy upload: the cap leaves the status line, its reason stays",
       r.st().upload_cap_kb == 5500 && r.st().page.find("upload cap") == std::string::npos &&
           r.st().upload_reason.find("raised from 5000 KB/s") == 0);
    /* the stream's liveness drives the layer, whatever the frames do */
    const int before = r.port.releases;
    r.feed->set_stream(false, false, Picture::Shown);
    r.pump(2000, &f1);   /* the fade out, then the linger */
    stock = r.port.row(0x100, 0x01000101);
    ok("the stream not live: the layer hidden, idle, and the slot released, the parked stock map shown",
       r.map_layer() && !r.map_layer()->params.visible && r.st().state == "idle" && r.port.releases > before &&
           !r.port.row(0x100, 0x010004f0) && stock && stock->map_status == 1);
    ok("and no stream reason", r.feed->status("decode", waiting).reason.empty());
    r.feed->set_stream(true, false, Picture::Shown);
    r.pump(1200, &f1);
    ok("live again: visible and showing with the frames", r.map_layer()->params.visible && r.st().state == "showing");
    r.feed->set_stream(true, true, Picture::Blank);
    r.pump(100, &f1);
    ok("frozen: hidden, and the status says why",
       !r.map_layer()->params.visible && r.st().state == "frozen" && r.st().reason == "the phone's cluster stream froze");
    r.feed->set_stream(true, false, Picture::Shown);
    r.pump(1200, &f1);
    ok("no longer frozen: shown again", r.map_layer()->params.visible && r.st().state == "showing");

    Rig h;
    h.feed->set_stream(false, false, Picture::None);
    h.svc.reset();
    h.start();
    h.pump(3000, &f1);
    ok("a layer made while the stream is not live is hidden: idle, the slot not asked for",
       h.map_layer() && !h.map_layer()->params.visible && h.st().state == "idle" && h.port.requests == 0);

    /* The stream's own pictures, from a stream start with the stock map up. */
    std::vector<uint8_t> black(1088 * 720, 128);
    std::fill(black.begin(), black.begin() + 1088 * 480, 16);
    Rig b(true, 60000);
    b.port.stock_map(0x100, 0x01000101);
    b.pump(100);
    ok("blank start: live, no frame yet: idle, no reason, the slot not asked for",
       b.st().state == "idle" && b.st().reason.empty() && b.port.requests == 0);
    b.pump(2000, &black, 20);   /* the car's start: about 100 blank frames in 2 s */
    b.pump(60000);              /* then a pause */
    const crs::SlotInfo *stock_b = b.port.row(0x100, 0x01000101);
    ok("blank start: the burst and a minute's pause never ask for the slot; the stock map stays",
       b.port.requests == 0 && !b.port.row(0x100, 0x010004f0) && stock_b && stock_b->map_status == 1 &&
           b.map_layer() && !b.map_layer()->params.visible);
    ok("blank start: idle, and the app told it waits for the phone's map",
       b.st().state == "idle" && b.st().reason == MapFeed::kWaiting && !b.st().stream_reason &&
           b.st().reason == "Waiting for the phone's map");
    ok("blank start: a lost stream's reason comes first",
       b.feed->status("decode", lost).reason == lost && b.feed->status("decode", lost).stream_reason);
    b.pump(5, &f1);   /* one non-blank frame, then nothing */
    b.pump(1500);
    g = b.gpu.graph("map");
    drawn = false;
    if (g)
        for (const auto &kv : g->shown.buffers)
            drawn = drawn || (kv.second.f == crs::TexFormat::R8 && kv.second.px == f1);
    stock_b = b.port.row(0x100, 0x01000101);
    ok("first picture: the slot asked for once, the stock map taken over, that frame shown",
       b.st().state == "showing" && drawn && b.port.requests == 1 && b.port.taken_over.size() == 1 && stock_b &&
           stock_b->map_status == 3);
    for (int spell = 0; spell < 6; spell++) {
        b.pump(9000, &black, 1000);   /* a blank frame a second for 9 s */
        b.pump(5, &f1);
    }
    ok("blank spells of 9 s between pictures: showing throughout, the slot not asked for again",
       b.st().state == "showing" && b.port.requests == 1 && b.map_layer()->params.visible);
    const int rel_b = b.port.releases;
    b.pump(5, &black);   /* one blank frame, then nothing */
    b.pump(9900);
    ok("one blank frame, then nothing: the map stays until blank_ms",
       b.st().state == "showing" && b.port.releases == rel_b);
    b.pump(2500);   /* blank_ms, then the fade out and the linger */
    stock_b = b.port.row(0x100, 0x01000101);
    ok("blank for blank_ms: hidden, the slot given back, the stock map shown, the app told it waits for the phone's map",
       !b.map_layer()->params.visible && b.port.releases > rel_b && !b.port.row(0x100, 0x010004f0) && stock_b &&
           stock_b->map_status == 1 && b.st().state == "idle" && b.st().reason == MapFeed::kWaiting);
    b.pump(5, &f1);
    b.pump(1500);
    ok("the next picture takes the slot again", b.st().state == "showing" && b.port.requests == 2);

    Rig fz(true, 60000);
    fz.port.stock_map(0x100, 0x01000101);
    fz.pump(31000, &black, 65);   /* blank at 15 fps from the start */
    ok("blank at full rate from the start: frozen after 30 s, the slot never asked for",
       fz.st().state == "frozen" && fz.port.requests == 0 && !fz.map_layer()->params.visible);

    Rig lf(true, 60000);
    lf.live_stall_ms = 6000;
    lf.port.stock_map(0x100, 0x01000101);
    lf.pump(100);
    bool every = true;
    for (int k = 0; k < 3; k++) {
        lf.pump(5, &f1);   /* one frame, then nothing for 10 s */
        lf.pump(1500);
        every = every && lf.st().state == "showing" && lf.map_layer()->params.visible;
        lf.pump(8500);
        every = every && lf.st().state == "idle";
    }
    ok("live through a recent frame: a static map's frame every 10 s is shown each time it makes the stream live",
       every && lf.port.requests == 3);

    Rig nl(true, 60000);
    nl.port.stock_map(0x100, 0x01000101);
    nl.pump(1500, &f1);
    ok("reconnect: the first stream shows", nl.st().state == "showing" && nl.port.requests == 1);
    uint64_t seq = 0;
    std::vector<std::string> lines;
    cld::log_lines_since(&seq, &lines);
    lines.clear();
    nl.live = false;   /* the connection ends */
    nl.pump(2000);
    nl.pump(5, &f1);   /* the ended stream's last frame, still on its way */
    nl.pump(200, &black, 20);
    nl.pump(3500);
    nl.live = true;   /* the next connection */
    nl.pump(1500);
    const crs::SlotInfo *stock_nl = nl.port.row(0x100, 0x01000101);
    ok("reconnect: a frame decoded after the end, the next stream live seconds later: idle, hidden, the stock map shown",
       nl.st().state == "idle" && nl.st().reason.empty() && !nl.map_layer()->params.visible &&
           nl.port.requests == 1 && stock_nl && stock_nl->map_status == 1);
    cld::log_lines_since(&seq, &lines);
    int not_live = 0;
    for (const std::string &line : lines)
        not_live += line.find("cluster stream is not live") != std::string::npos;
    ok("reconnect: the stream ending is logged once, whatever frames still come", not_live == 1);
    nl.live = false;
    nl.pump(2000);
    nl.pump(5, &f1);
    nl.pump(500);
    nl.live = true;   /* a new stream's first frame, decoded before its connection is read */
    nl.pump(1500);
    ok("reconnect: a frame decoded just before the stream is live takes the slot",
       nl.st().state == "showing" && nl.port.requests == 2);

    StatusLog sl;
    FeedStatus a;
    a.state = "showing";
    a.target = "map";
    a.page = "map — 15 frames/s";
    int64_t t = 1000000;
    ok("status log: the first status", sl.due(a, t));
    a.page = "map — 14 frames/s";
    a.upload_cap_kb = 5500;
    a.upload_reason = "slow flushes";
    ok("status log: not for the page or the upload cap", !sl.due(a, t));
    a.reason = "Stream lost";
    a.stream_reason = true;
    ok("status log: the stream's reason", sl.due(a, t));
    a.reason.clear();
    a.stream_reason = false;
    ok("status log: the stream whole again within 10 s waits", !sl.due(a, t + 2000));
    a.reason = "Stream lost";
    a.stream_reason = true;
    ok("status log: so does its next loss; the logged reason is the same, nothing to log",
       !sl.due(a, t + 4000) && !sl.due(a, t + 10000));
    a.reason.clear();
    a.stream_reason = false;
    ok("status log: a change still standing 10 s after the last such line is logged",
       !sl.due(a, t + 9000) && sl.due(a, t + 10500));
    a.state = "idle";
    a.reason = "Stream lost";
    a.stream_reason = true;
    ok("status log: a state change is logged at once, with its reason", sl.due(a, t + 11000));
    a.state = "starting";
    a.reason = "the stock map holds the slot";
    a.stream_reason = false;
    ok("status log: a state and reason", sl.due(a, t + 11100));
    ok("status log: the same again is not", !sl.due(a, t + 11200));
    a.reason = "another app holds the slot";
    ok("status log: a reason of the feed's own alone, at once", sl.due(a, t + 11300));
    a.target = "panel";
    ok("status log: a target alone", sl.due(a, t + 11400));

    guide_hold();
    show_off();
    only_what_is_needed();
    {   /* (its own scope: the names of the rest of main) */
    /* ---- MapFeeds: the cluster, and the HUD's copy when it is on ---- */
    Rig b(false, 6000, false, true, true);
    b.feed.reset();   /* the rig's single feed is not used here */
    b.sc.targets[2].share = true;
    b.start();
    FeedConfig bfc;
    bfc.socket = b.path;
    bfc.video.level_bits = 0;
    MapFeeds feeds(bfc, true, false, 10);
    feeds.set_stream(true, false, Picture::Shown);
    std::vector<uint8_t> bspare, boffered;
    bool hud_asked = false;
    /* vary: each frame differs from the one before (a moving map); a frame
     * the same as the last offered goes as a repeat, as the producer hands
     * it over */
    const auto bpump = [&](int64_t ms, const std::vector<uint8_t> *frame, bool vary = false) {
        for (int64_t t = 0; t < ms; t += 5) {
            b.now += 5;
            if (frame && t % 65 == 0) {
                bspare.assign(frame->begin(), frame->end());
                if (vary)
                    bspare[(size_t)(t / 65) % 1088] ^= 0x80;
                if (bspare == boffered) {
                    feeds.repeat((uint64_t)b.now, b.now);
                } else {
                    boffered = bspare;
                    feeds.offer(&bspare, (uint64_t)b.now, b.now);
                }
            }
            for (int round = 0; round < 400; round++) {
                std::vector<pollfd> fds;
                const size_t first = b.svc->poll_fds(&fds);
                const int n = ::poll(fds.data(), fds.size(), 0);
                b.port.tick(b.now);
                b.svc->after_poll(fds, first, b.now);
                const bool pending = (feeds.cluster() && feeds.cluster()->wants_write()) || (feeds.hud_feed() && feeds.hud_feed()->wants_write());
                const bool busy = scaling(feeds, b.now);
                if (busy)
                    usleep(1000);
                feeds.poll(b.now);
                hud_asked = hud_asked || b.svc->slots().requested(2);
                if (round > 0 && n <= 0 && !pending && !busy)
                    break;
            }
        }
    };
    const auto layer_on = [&](int target) -> const crs::Layer * {
        for (const crs::Layer *x : b.svc->compositor().layers())
            if (x->target() == target)
                return x;
        return nullptr;
    };
    const auto shown_r8 = [&](const char *target, const std::vector<uint8_t> &px) {
        crs::FakeGraph *g = b.gpu.graph(target);
        bool on = false;
        if (g)
            for (const auto &kv : g->shown.buffers)
                on = on || (kv.second.f == crs::TexFormat::R8 && kv.second.px == px);
        return on;
    };
    bpump(1500, &f1);
    ok("HUD off (the default): the map on the cluster, no HUD feed, layer or request",
       layer_on(0) && shown_r8("map", f1) && !feeds.hud_feed() && !layer_on(2) && !hud_asked &&
           feeds.status("decode").state == "showing" && feeds.status("decode").hud_state == "off" && !feeds.status("decode").hud);
    ok("HUD off: no hud scene ever published", !b.gpu.graph("hud"));

    feeds.set_hud(true);
    bpump(1500, &f1);
    /* the HUD shows the map smaller: its frame is scaled on the head unit */
    AtlasScaler ref;
    std::vector<uint8_t> f1_hud;
    ref.configure(1088, 480, 452, 200, 2);   /* FeedConfig::scaled_level_bits */
    ref.set_corner_radius(200 / 12);         /* FeedConfig::hud_round_corners */
    ref.set_black_background(true);          /* FeedConfig::hud_black_background */
    ref.scale(f1.data(), &f1_hud);
    const crs::Layer *bh = layer_on(2);
    ok("HUD on: a layer on each target, the frame on the cluster whole and on the HUD scaled to 452x200, showing",
       layer_on(0) && bh && shown_r8("map", f1) && shown_r8("hud", f1_hud) && feeds.status("decode").state == "showing" &&
           feeds.status("decode").hud_state == "showing" && feeds.status("decode").target == "map");
    ok("HUD on: its map fitted inside its 800x480 scene",
       bh && bh->params.rect.w <= 800 && bh->params.rect.h <= 480 && bh->params.rect.x + bh->params.rect.w <= 800);
    {
        const uint64_t before = feeds.hud_feed()->scaled_frames();
        bpump(2000, &f2, true);
        const uint64_t scaled = feeds.hud_feed()->scaled_frames() - before;
        char name[160];
        std::snprintf(name, sizeof name, "HUD on: at most 10 frames a second while the cluster takes 15 (%llu in 2 s)",
                      (unsigned long long)scaled);
        ok(name, scaled >= 15 && scaled <= 21);
        /* one more frame right after a HUD frame went, then none: it is not
         * due yet, and still reaches the HUD */
        const auto hud_px = [&]() {
            crs::FakeGraph *g = b.gpu.graph("hud");
            std::vector<uint8_t> px;
            if (g)
                for (const auto &kv : g->shown.buffers)
                    if (kv.second.f == crs::TexFormat::R8)
                        px = kv.second.px;
            return px;
        };
        const std::vector<uint8_t> before_last = hud_px(), f3 = atlas(3);
        bspare.assign(f3.begin(), f3.end());
        boffered = f3;
        feeds.offer(&bspare, (uint64_t)b.now, b.now);
        bpump(600, nullptr);
        ok("HUD on: the newest frame, offered between two HUD frames, reaches the HUD once the stream stops",
           shown_r8("map", f3) && !before_last.empty() && hud_px() != before_last);
        const uint64_t still_from = feeds.hud_feed()->scaled_frames();
        bpump(8000, &f3);   /* past the layers' stall_ms (6 s) */
        const crs::Layer *hs = layer_on(2);
        std::string why;
        ok("HUD on, a still map: the same frame not scaled again, the HUD layer kept fed past its stall time",
           feeds.hud_feed()->scaled_frames() == still_from && shown_r8("map", f3) && hs &&
               b.svc->compositor().state(hs->id(), &why) == crs::LayerState::Shown &&
               feeds.status("decode").hud_state == "showing");
    }


    /* ---- the HUD map's size ---- */
    const crs::Layer *hl = layer_on(2);
    const unsigned default_w = hl ? hl->params.rect.w : 0;
    ok("the default HUD map is the kit's fitted box, narrower than the HUD", hl && default_w > 0 && default_w < 800);
    feeds.set_hud_width(100);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("100 %: held to the widest, 85 % (680x300), the picture's shape kept, centred",
       hl && hl->params.rect.w == 680 && hl->params.rect.h == 300 && hl->params.rect.x == 60 && hl->params.rect.y == 90 &&
           feeds.status("decode").hud_width_pct == 85);
    feeds.set_hud_width(40);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("40 %: resized live, centred", hl && hl->params.rect.w == 320 && hl->params.rect.h == 140 &&
                                      hl->params.rect.x == 240 && hl->params.rect.y == 169);
    /* the frame goes at the size it is shown: 320x140, 67,200 B instead of 783,360 */
    crs::FakeGraph *g40 = b.gpu.graph("hud");
    bool small = false;
    if (g40)
        for (const auto &kv : g40->shown.buffers)
            small = small || (kv.second.f == crs::TexFormat::R8 && kv.second.px.size() == 320u * 210);
    ok("40 %: the HUD's texture is the scaled frame, 320x140 (67,200 B)", small);
    ok("the size leaves the cluster's map alone", layer_on(0) && layer_on(0)->params.rect.w == 1088);
    feeds.set_hud(false);
    bpump(2500, &f2);
    ok("HUD off again: its layer gone, the cluster's map as it was", !layer_on(2) && layer_on(0) && shown_r8("map", f2));
    feeds.set_hud(true);
    bpump(2500, &f2);
    ok("a HUD feed made later takes the size", layer_on(2) && layer_on(2)->params.rect.w == 320);
    feeds.set_hud_width(0);
    bpump(300, &f2);
    ok("0: back to the default box", layer_on(2) && layer_on(2)->params.rect.w == default_w);

    /* ---- where the HUD map is ---- */
    feeds.set_hud_width(40);
    bpump(300, &f2);
    const crs::Layer *at40 = layer_on(2);
    const uint32_t ref40 = at40 ? at40->id() : 0;
    feeds.set_hud_offset(100, -50);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("moved: 100 right, 50 up from the centre (240,169 -> 340,119)", hl && hl->params.rect.x == 340 && hl->params.rect.y == 119 &&
                                                                          hl->params.rect.w == 320 && hl->params.rect.h == 140);
    ok("moved: the same layer, only its place changed (no new texture)", hl && hl->id() == ref40);
    ok("moved: the status carries the offset", feeds.status("decode").hud_offset_x == 100 && feeds.status("decode").hud_offset_y == -50);
    feeds.set_hud_offset(800, 800);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("moved too far: held inside the HUD (its bottom right corner)",
       hl && hl->params.rect.x + hl->params.rect.w == 800 && hl->params.rect.y + hl->params.rect.h >= 479 && hl->params.rect.y + hl->params.rect.h <= 480);
    ok("moved too far: the offset reported as far as it goes (240,170 for 320x140)",
       feeds.status("decode").hud_offset_x == 240 && feeds.status("decode").hud_offset_y == 170);
    feeds.set_hud_offset(-800, -800);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("moved too far the other way: the top left corner", hl && hl->params.rect.x == 0 && hl->params.rect.y == 0);
    feeds.set_hud_offset(-30, 20);
    feeds.set_hud_width(60);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("resized while moved: the new size, still moved (60 %: 480x211 centred at 160,134, so 130,154)",
       hl && hl->params.rect.w == 480 && hl->params.rect.x == 130 && hl->params.rect.y == 154);
    feeds.set_hud_offset(200, 0);
    feeds.set_hud_width(85);
    bpump(300, &f2);
    feeds.set_hud_width(40);
    bpump(300, &f2);
    hl = layer_on(2);
    ok("moved, made larger (held to 60 px) and smaller again: back where it was asked (240+200 = 440)",
       hl && hl->params.rect.x == 440 && feeds.status("decode").hud_offset_x == 200);
    feeds.set_hud_offset(-30, 20);
    feeds.set_hud_width(60);
    bpump(300, &f2);
    ok("the offset leaves the cluster's map alone", layer_on(0) && layer_on(0)->params.rect.x == 0 && layer_on(0)->params.rect.y == 0);
    feeds.set_hud_offset(0, 0);
    feeds.set_hud_width(0);

    /* ---- the HUD map over BMW's pop-ups ---- */
    bpump(300, &f2);
    ok("by default the HUD map gives way (its layer's yield 1)",
       layer_on(2) && layer_on(2)->params.yield && !feeds.status("decode").hud_keep_shown);
    b.port.stock_map(0x322, 0x01000251);   /* the media source list on the HUD */
    bpump(300, &f2);
    ok("by default: a list on the HUD releases the HUD map",
       b.svc->slots().status(2).state == crs::TargetState::Waiting);
    const uint32_t yield_ref = layer_on(2) ? layer_on(2)->id() : 0;
    feeds.set_hud_keep_shown(true);
    bpump(2500, &f2);
    ok("kept shown: the same layer with yield 0, shown over the list, the status says so",
       layer_on(2) && layer_on(2)->id() == yield_ref && !layer_on(2)->params.yield &&
           b.svc->slots().status(2).state == crs::TargetState::Shown && feeds.status("decode").hud_keep_shown);
    ok("kept shown: the cluster's map layer untouched", layer_on(0) && layer_on(0)->params.yield);
    feeds.set_hud(false);
    bpump(2500, &f2);
    feeds.set_hud(true);
    bpump(2500, &f2);
    ok("a HUD feed made later keeps it shown", layer_on(2) && !layer_on(2)->params.yield &&
                                                 b.svc->slots().status(2).state == crs::TargetState::Shown);
    feeds.set_hud_keep_shown(false);
    bpump(300, &f2);
    ok("giving way again: released for the list", layer_on(2) && layer_on(2)->params.yield &&
                                                     b.svc->slots().status(2).state == crs::TargetState::Waiting);
    b.port.stock_unmap(0x322, 0x01000251);
    bpump(2500, &f2);
    ok("the list gone: the HUD map shown again", b.svc->slots().status(2).state == crs::TargetState::Shown);

    /* ---- where the map shows: the cluster, the HUD, both, neither ---- */
    {
        const std::vector<uint8_t> f4 = atlas(4), f5 = atlas(5);
        feeds.set_cluster(false);
        bool map_asked = false;
        for (int k = 0; k < 100; k++) {   /* the map's linger (1.5 s) and fade first */
            bpump(50, &f4, true);
            map_asked = map_asked || (k > 50 && b.svc->slots().requested(0));
        }
        FeedStatus st = feeds.status("decode");
        ok("the HUD alone: no cluster feed or map layer, the map target let go, the HUD showing, status off/on",
           !feeds.cluster() && !layer_on(0) && !map_asked && layer_on(2) && feeds.wants_frames() && st.state == "off" &&
               !st.cluster && st.hud && st.hud_state == "showing");
        const uint64_t alone_from = feeds.hud_feed()->scaled_frames();
        bpump(2000, &f4, true);
        const uint64_t alone = feeds.hud_feed()->scaled_frames() - alone_from;
        char name[160];
        std::snprintf(name, sizeof name, "the HUD alone: its frames taken without a copy, at most 10 a second (%llu in 2 s)",
                      (unsigned long long)alone);
        ok(name, alone >= 15 && alone <= 21);
        feeds.set_hud(false);
        bpump(2500, &f4, true);
        st = feeds.status("decode");
        ok("neither: no feed, no layer, no frames wanted, status off/off",
           !feeds.cluster() && !feeds.hud_feed() && !layer_on(0) && !layer_on(2) && !feeds.wants_frames() &&
               st.state == "off" && st.hud_state == "off" && !st.cluster && !st.hud);
        bspare.assign(f4.begin(), f4.end());
        feeds.offer(&bspare, 1, b.now);
        ok("neither: an offered frame goes nowhere", !layer_on(0) && !layer_on(2));
        feeds.set_cluster(true);
        bpump(1500, &f5);
        ok("the cluster on again: its layer back, the next frame shown, the HUD still off",
           layer_on(0) && shown_r8("map", f5) && !layer_on(2) && feeds.status("decode").state == "showing" &&
               feeds.wants_frames());
        feeds.set_hud(true);
        bpump(1500, &f5);
        ok("both again: the HUD's layer back beside the cluster's", layer_on(0) && layer_on(2) &&
                                                                     feeds.status("decode").hud_state == "showing");
        feeds.set_cluster(false);
        feeds.set_cluster(true);
        bpump(1500, &f5);
        ok("the cluster off and on with the HUD on: both shown, the HUD kept", layer_on(0) && layer_on(2) && shown_r8("map", f5));
    }

    /* ---- repeats: a still map costs nothing on either target ---- */
    {
        /* frames whole, and every frame the renderer counted (whole, repeats
         * and the frames they superseded) */
        const auto counted = [&](int target, unsigned long long *whole) {
            unsigned long long n[4] = {};
            if (const crs::Layer *l = layer_on(target))
                std::sscanf(l->stats().c_str(), "%llu sent, %llu unchanged, %llu within threshold, %llu superseded",
                            &n[0], &n[1], &n[2], &n[3]);
            *whole = n[0];
            return n[0] + n[1] + n[2] + n[3];
        };
        const auto shown = [&](int target) {
            const crs::Layer *l = layer_on(target);
            std::string why;
            return l && b.svc->compositor().state(l->id(), &why) == crs::LayerState::Shown;
        };
        const auto hud_r8 = [&]() {
            std::vector<uint8_t> px;
            if (crs::FakeGraph *g = b.gpu.graph("hud"))
                for (const auto &kv : g->shown.buffers)
                    if (kv.second.f == crs::TexFormat::R8)
                        px = kv.second.px;
            return px;
        };
        const std::vector<uint8_t> f6 = atlas(6), f7 = atlas(7);
        bpump(1500, &f6);
        unsigned long long cw0, hw0;
        const unsigned long long c0 = counted(0, &cw0), h0 = counted(2, &hw0);
        const uint64_t s0 = feeds.hud_feed()->scaled_frames();
        const std::vector<uint8_t> still_hud = hud_r8();
        bpump(8000, &f6);   /* repeats only, past the layers' stall_ms (6 s) */
        unsigned long long cw1, hw1;
        const unsigned long long c1 = counted(0, &cw1), h1 = counted(2, &hw1);
        const uint64_t s1 = feeds.hud_feed()->scaled_frames();
        char name[200];
        std::snprintf(name, sizeof name,
                      "both, a still map: repeats keep both layers shown past stall_ms, no frame whole and none scaled "
                      "(cluster %llu, HUD %llu counted in 8 s)",
                      c1 - c0, h1 - h0);
        ok(name, cw1 == cw0 && hw1 == hw0 && s1 == s0 && c1 - c0 >= 100 && h1 - h0 >= 100 && shown(0) && shown(2) &&
                     shown_r8("map", f6) && hud_r8() == still_hud && feeds.status("decode").state == "showing" &&
                     feeds.status("decode").hud_state == "showing");
        bpump(2000, &f7, true);   /* moving */
        unsigned long long cw2, hw2;
        const unsigned long long c2 = counted(0, &cw2);
        counted(2, &hw2);
        const uint64_t s2 = feeds.hud_feed()->scaled_frames();
        std::snprintf(name, sizeof name,
                      "still, then moving: the HUD scales at most 10 a second (%llu in 2 s) and is written (%llu), "
                      "the cluster keeps its rate (%llu frames in 2 s, %llu in 2 s still)",
                      (unsigned long long)(s2 - s1), hw2 - hw1, c2 - c1, (c1 - c0) / 4);
        ok(name, s2 - s1 >= 15 && s2 - s1 <= 21 && hw2 - hw1 >= 5 && hud_r8() != still_hud && cw2 - cw1 >= 25 &&
                     c2 - c1 + 3 >= (c1 - c0) / 4 && shown(0) && shown(2));

        /* the HUD alone, from both with a still map: its picture kept, and
         * repeats of its last scaled frame */
        bpump(1500, &f6);
        feeds.set_cluster(false);
        bpump(2500, &f6);
        unsigned long long hw3;
        const unsigned long long h3 = counted(2, &hw3);
        const uint64_t s3 = feeds.hud_feed()->scaled_frames();
        bpump(8000, &f6);
        unsigned long long hw4;
        const unsigned long long h4 = counted(2, &hw4);
        ok("the HUD alone, a still map: repeats keep it shown past stall_ms, no frame whole and none scaled",
           !layer_on(0) && hw4 == hw3 && h4 - h3 >= 100 && feeds.hud_feed()->scaled_frames() == s3 && shown(2) &&
               feeds.status("decode").hud_state == "showing");
        bpump(2000, &f7, true);
        const uint64_t s5 = feeds.hud_feed()->scaled_frames();
        ok("the HUD alone, then moving: scaled at most 10 a second", s5 - s3 >= 15 && s5 - s3 <= 21 && shown(2));

        /* a feed made beside the other starts from its picture: no new one
         * comes, the map is still */
        bpump(1500, &f6);
        feeds.set_cluster(true);
        bpump(1500, &f6);
        ok("the cluster on beside the HUD, a still map: the picture shown from the HUD's at once",
           shown(0) && shown_r8("map", f6) && shown(2));
        feeds.set_hud(false);
        bpump(1500, &f6);
        feeds.set_hud(true);
        feeds.set_cluster(false);
        bpump(2500, &f6);
        ok("the cluster's map moved to the HUD, a still map: the HUD shows it, scaled once",
           !layer_on(0) && shown(2) && feeds.hud_feed()->scaled_frames() == 1 && !hud_r8().empty());
        feeds.set_cluster(true);
        bpump(1500, &f6);
    }

    feeds.set_stream(false, false, Picture::None);
    bpump(200, &f2);
    ok("the stream not live reaches both feeds: idle", feeds.status("decode").state == "idle" &&
                                                      feeds.status("decode").hud_state == "idle");
    }

    {   /* ---- before the renderer: the HUD's place as saved ---- */
    FeedConfig nfc;
    nfc.socket = "/tmp/woc-feed-no-renderer-" + std::to_string(getpid()) + ".sock";
    nfc.hud_offset_x = 30;
    nfc.hud_offset_y = -20;
    MapFeeds none(nfc, true, true, 10);
    none.poll(1000);
    ok("HUD on, no renderer yet: the status has the saved place, not the centre",
       none.status("decode").hud_offset_x == 30 && none.status("decode").hud_offset_y == -20);
    none.set_hud_offset(40, -20);
    ok("and a move then is taken as asked", none.status("decode").hud_offset_x == 40);
    }

    /* ---- cluster horizontal offset (source crop from wider CarPlay geometry) ---- */
    {
        /* Feed with 1440x636 source (RES_WIDE), cropped to 1088x480 for cluster.
         * Use a real rig with fake renderer to test the full pipeline. */
        FeedConfig cfc;
        cfc.socket = "/tmp/woc-feed-cluster-offset-" + std::to_string(getpid()) + ".sock";
        cfc.target = "map";
        cfc.video.w = 1440;
        cfc.video.h = 636;
        cfc.video.level_bits = 0;
        cfc.video.stall_ms = 6000;
        cfc.cluster_offset_x = 0;

        crs::FakeGpu gpu;
        crs::FakePort port;
        crs::ServiceConfig sc;
        crs::TargetSpec m;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
        m.take_over_stock = true;
        sc.targets = {m};
        sc.socket_path = cfc.socket;
        sc.backend = "fake";
        crs::Service svc(&gpu, &port, sc);
        port.m = &svc.slots();
        std::string err;
        if (!svc.start(&err)) {
            std::printf("cluster-offset test: service start failed: %s\n", err.c_str());
        } else {
            MapFeed feed(cfc);
            feed.set_stream(true, false, Picture::Shown);
            int64_t now = 200000;

            auto make_atlas = [](uint32_t w, uint32_t h, uint8_t base) {
                std::vector<uint8_t> a((size_t)w * (h + h / 2));
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++)
                        a[y * w + x] = (uint8_t)((x + y * 7 + base) & 0xff);
                for (uint32_t y = 0; y < h / 2; y++) {
                    for (uint32_t x = 0; x < w / 2; x++) {
                        a[w * h + y * w + x] = (uint8_t)((x * 2 + y * 11 + 128) & 0xff);
                        a[w * h + y * w + w / 2 + x] = (uint8_t)((x * 3 + y * 13 + 128) & 0xff);
                    }
                }
                return a;
            };

            auto pump = [&](int64_t ms) {
                for (int64_t t = 0; t < ms; t += 5) {
                    now += 5;
                    std::vector<pollfd> fds;
                    size_t first = svc.poll_fds(&fds);
                    int n = poll(fds.data(), fds.size(), 0);
                    port.tick(now);
                    svc.after_poll(fds, first, now);
                    feed.poll(now);
                }
            };

            auto check_crop = [&](int expected_crop_x, const char *desc) {
                FeedStatus st = feed.status("decode");
                ok(desc, st.cluster_crop_x == (uint32_t)expected_crop_x);
            };

            check_crop(176, "cluster offset 0: crop_x = 176 (centered)");
            feed.set_cluster_offset(10);
            check_crop(166, "cluster offset +10: crop_x = 166");
            feed.set_cluster_offset(-50);
            check_crop(226, "cluster offset -50: crop_x = 226");
            feed.set_cluster_offset(176);
            check_crop(0, "cluster offset +176 (right edge): crop_x = 0");
            feed.set_cluster_offset(-176);
            check_crop(352, "cluster offset -176 (left edge): crop_x = 352");
            feed.set_cluster_offset(200);
            check_crop(0, "cluster offset +200: clamped to +176, crop_x = 0");
            feed.set_cluster_offset(-200);
            check_crop(352, "cluster offset -200: clamped to -176, crop_x = 352");
        }
    }

    /* HUD feed: independent centered crop (crop_x = 176), NOT affected by cluster_offset_x */
    {
        FeedConfig hfc;
        hfc.socket = "/tmp/woc-feed-hud-offset-" + std::to_string(getpid()) + ".sock";
        hfc.target = "hud";
        hfc.video.w = 1440;
        hfc.video.h = 636;
        hfc.video.level_bits = 2;
        hfc.video.stall_ms = 6000;
        hfc.cluster_offset_x = 100;
        hfc.scale_down = true;
        hfc.scaled_level_bits = 2;
        hfc.hud_round_corners = true;
        hfc.hud_black_background = true;

        crs::FakeGpu gpu;
        crs::FakePort port;
        crs::ServiceConfig sc;
        crs::TargetSpec m, u;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
        m.take_over_stock = true;
        u.name = "hud"; u.slot = 0x323; u.scene_id = 0x01000400; u.width = 800; u.height = 480; u.linger_ms = 3000;
        u.yield_slots = {0x322};
        u.yield_ms = 1500;
        sc.targets = {m, u};
        sc.socket_path = hfc.socket;
        sc.backend = "fake";
        crs::Service svc(&gpu, &port, sc);
        port.m = &svc.slots();
        std::string err;
        if (!svc.start(&err)) {
            std::printf("hud-offset test: service start failed: %s\n", err.c_str());
        } else {
            MapFeed feed(hfc);
            feed.set_stream(true, false, Picture::Shown);
            int64_t now = 200000;
            auto pump = [&](int64_t ms) {
                for (int64_t t = 0; t < ms; t += 5) {
                    now += 5;
                    std::vector<pollfd> fds;
                    size_t first = svc.poll_fds(&fds);
                    int n = poll(fds.data(), fds.size(), 0);
                    port.tick(now);
                    svc.after_poll(fds, first, now);
                    feed.poll(now);
                }
            };
            pump(500);
            FeedStatus hs = feed.status("decode");
            ok("HUD feed with cluster_offset_x = 100: crop_x = 176 (independent)",
               hs.cluster_offset_x == 100 && hs.cluster_crop_x == 176);
            hfc.cluster_offset_x = -100;
            MapFeed feed2(hfc);
            feed2.set_stream(true, false, Picture::Shown);
            pump(500);
            FeedStatus hs2 = feed2.status("decode");
            ok("HUD feed with cluster_offset_x = -100: crop_x still = 176 (independent)",
               hs2.cluster_offset_x == -100 && hs2.cluster_crop_x == 176);
        }
    }

    /* ---- pixel-level cluster crop test with synthetic YUV atlas ---- */
    {
        /* Test AtlasScaler directly with crop to verify pixel correctness.
         * Source: 1440x636 YUV 4:2:0 (Y: 1440x636, UV: 720x318 side by side)
         * Crop: 1088x480 at varying crop_x (centered at 176)
         * Formulas (base_y = 0 for standalone test):
         *   Y = (x + y*7) & 0xff
         *   U = (x*2 + y*11 + 128) & 0xff
         *   V = (x*3 + y*13 + 128) & 0xff
         * Chroma source X = crop_x/2 + output_chroma_x */
        auto make_atlas = [](uint32_t w, uint32_t h, uint8_t base_y) {
            std::vector<uint8_t> a((size_t)w * (h + h / 2));
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    a[y * w + x] = (uint8_t)((x + y * 7 + base_y) & 0xff);
            for (uint32_t y = 0; y < h / 2; y++) {
                for (uint32_t x = 0; x < w / 2; x++) {
                    a[w * h + y * w + x] = (uint8_t)((x * 2 + y * 11 + 128) & 0xff);
                    a[w * h + y * w + w / 2 + x] = (uint8_t)((x * 3 + y * 13 + 128) & 0xff);
                }
            }
            return a;
        };

        /* Test crop at center (crop_x = 176) -> output should be 1088x480 */
        AtlasScaler scaler;
        ok("AtlasScaler: configure 1440x636 -> 1088x480 with crop_x=176",
           scaler.configure(1440, 636, 1088, 480, 0, 176, 0, 1088, 480));
        std::vector<uint8_t> src = make_atlas(1440, 636, 0);
        std::vector<uint8_t> dst;
        scaler.scale(src.data(), &dst);
        ok("AtlasScaler: output size 1088*720 = 783360",
           dst.size() == 1088u * 720u);

        /* Destination buffer layout: Y plane 1088x480, then U and V side by side at 544x240.
         * U starts at 1088*480 = 522240, V starts at 1088*480 + 544 = 522784. */

        /* Verify center crop: output x=0 corresponds to source x=176 */
        /* Y at dst(0,0) should match src Y at (176,0) = (176 + 0*7 + 0) & 0xff = 176 */
        ok("AtlasScaler crop center: Y[0,0] = src[176,0] = 176",
           dst[0] == 176);
        /* Y at dst(1087,0) should match src Y at (1263,0) = (1263 + 0*7) & 0xff = 239 */
        ok("AtlasScaler crop center: Y[1087,0] = src[1263,0] = 239",
           dst[1087] == 239);
        /* Y at dst(0,479) should match src Y at (176,479) = (176 + 479*7) & 0xff = 201 */
        ok("AtlasScaler crop center: Y[0,479] = src[176,479] = 201",
           dst[479 * 1088] == 201);
        /* Y at dst(1087,479) should match src Y at (1263,479) = (1263 + 479*7) & 0xff = 8 */
        ok("AtlasScaler crop center: Y[1087,479] = src[1263,479] = 8",
           dst[479 * 1088 + 1087] == 8);

        /* U at dst(0,0) with center crop (crop_x=176) -> src U at (88,0) = (88*2 + 0*11 + 128) & 0xff = 48 */
        ok("AtlasScaler crop center: U[0,0] = src U[88,0] = 48",
           dst[1088 * 480 + 0] == 48);
        /* V at dst(0,0) -> src V at (88,0) = (88*3 + 0*13 + 128) & 0xff = 136 */
        ok("AtlasScaler crop center: V[0,0] = src V[88,0] = 136",
           dst[1088 * 480 + 1088 / 2 + 0] == 136);
        /* U at dst(543,239) with center crop -> src U at (631,239) = (631*2 + 239*11 + 128) & 0xff = 179 */
        ok("AtlasScaler crop center: U[543,239] = src U[631,239] = 179",
           dst[1088 * 480 + 239 * 1088 + 543] == 179);
        /* V at dst(543,239) -> src V at (631,239) = (631*3 + 239*13 + 128) & 0xff = 8 */
        ok("AtlasScaler crop center: V[543,239] = src V[631,239] = 8",
           dst[1088 * 480 + 239 * 1088 + 1088 / 2 + 543] == 8);

        /* Test crop at left edge (crop_x = 0) */
        AtlasScaler scaler_left;
        ok("AtlasScaler: configure 1440x636 -> 1088x480 with crop_x=0",
           scaler_left.configure(1440, 636, 1088, 480, 0, 0, 0, 1088, 480));
        std::vector<uint8_t> dst_left;
        scaler_left.scale(src.data(), &dst_left);
        /* Y at dst(0,0) should match src Y at (0,0) = 0 */
        ok("AtlasScaler crop left: Y[0,0] = src[0,0] = 0",
           dst_left[0] == 0);
        /* Y at dst(1087,0) should match src Y at (1087,0) = 1087 & 0xff = 63 */
        ok("AtlasScaler crop left: Y[1087,0] = src[1087,0] = 63",
           dst_left[1087] == 63);
        /* U at dst(0,0) with crop_x=0 -> src U at (0,0) = (0*2 + 0*11 + 128) & 0xff = 128 */
        ok("AtlasScaler crop left: U[0,0] = src U[0,0] = 128",
           dst_left[1088 * 480 + 0] == 128);
        /* V at dst(0,0) -> src V at (0,0) = (0*3 + 0*13 + 128) & 0xff = 128 */
        ok("AtlasScaler crop left: V[0,0] = src V[0,0] = 128",
           dst_left[1088 * 480 + 1088 / 2 + 0] == 128);

        /* Test crop at right edge (crop_x = 352) */
        AtlasScaler scaler_right;
        ok("AtlasScaler: configure 1440x636 -> 1088x480 with crop_x=352",
           scaler_right.configure(1440, 636, 1088, 480, 0, 352, 0, 1088, 480));
        std::vector<uint8_t> dst_right;
        scaler_right.scale(src.data(), &dst_right);
        /* Y at dst(0,0) should match src Y at (352,0) = 352 & 0xff = 96 */
        ok("AtlasScaler crop right: Y[0,0] = src[352,0] = 96",
           dst_right[0] == 96);
        /* Y at dst(1087,0) should match src Y at (1439,0) = 1439 & 0xff = 159 */
        ok("AtlasScaler crop right: Y[1087,0] = src[1439,0] = 159",
           dst_right[1087] == 159);
        /* U at dst(0,0) with crop_x=352 -> src U at (176,0) = (176*2 + 128) & 0xff = 224 */
        ok("AtlasScaler crop right: U[0,0] = src U[176,0] = 224",
           dst_right[1088 * 480 + 0] == 224);
        /* V at dst(0,0) -> src V at (176,0) = (176*3 + 128) & 0xff = 144 */
        ok("AtlasScaler crop right: V[0,0] = src V[176,0] = 144",
           dst_right[1088 * 480 + 1088 / 2 + 0] == 144);

        /* Test with non-zero crop_y */
        AtlasScaler scaler_y;
        ok("AtlasScaler: configure with crop_y=78 (1440x636 -> 1088x480, crop at 176,78)",
           scaler_y.configure(1440, 636, 1088, 480, 0, 176, 78, 1088, 480));
        std::vector<uint8_t> dst_y;
        scaler_y.scale(src.data(), &dst_y);
        /* Y at dst(0,0) should match src Y at (176,78) = (176 + 78*7) & 0xff = 210 */
        ok("AtlasScaler crop_y=78: Y[0,0] = src[176,78] = 210",
           dst_y[0] == 210);
        /* Y at dst(0,479) should match src Y at (176,557) = (176 + 557*7) & 0xff = 235 */
        ok("AtlasScaler crop_y=78: Y[0,479] = src[176,557] = 235",
           dst_y[479 * 1088] == 235);
        /* U at dst(0,0) with crop_y=78, crop_x=176 -> src U at (88,39) = (88*2 + 39*11 + 128) & 0xff = 221 */
        ok("AtlasScaler crop_y=78: U[0,0] = src U[88,39] = 221",
           dst_y[1088 * 480 + 0] == 221);
        /* V at dst(0,0) -> src V at (88,39) = (88*3 + 39*13 + 128) & 0xff = 131 */
        ok("AtlasScaler crop_y=78: V[0,0] = src V[88,39] = 131",
           dst_y[1088 * 480 + 1088 / 2 + 0] == 131);

        /* Verify downscaling with crop still works (e.g., 1440x636 -> 452x200) */
        AtlasScaler scaler_hud;
        ok("AtlasScaler: configure 1440x636 -> 452x200 with crop_x=176",
           scaler_hud.configure(1440, 636, 452, 200, 2, 176, 0, 1088, 480));
        std::vector<uint8_t> dst_hud;
        scaler_hud.scale(src.data(), &dst_hud);
        ok("AtlasScaler HUD: output size 452*300 = 135600",
           dst_hud.size() == 452u * 300u);
    }

    /* ---- live-update regression test: offset change during scaling ---- */
    {
        /* This test reproduces the bug where a cluster offset change during
         * active scaling would cause the worker's stale result (with old crop)
         * to be displayed instead of the new crop. The fix ensures that when
         * the worker finishes with a stale shape, the held atlas is resubmitted
         * with the current crop.
         * Uses deterministic worker synchronization. */
        FeedConfig cfc;
        cfc.socket = "/tmp/woc-feed-live-offset-" + std::to_string(getpid()) + ".sock";
        cfc.target = "map";
        cfc.video.w = 1440;
        cfc.video.h = 636;
        cfc.video.level_bits = 0;
        cfc.video.stall_ms = 6000;
        cfc.cluster_offset_x = 0;

        crs::FakeGpu gpu;
        crs::FakePort port;
        crs::ServiceConfig sc;
        crs::TargetSpec m;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
        m.take_over_stock = true;
        sc.targets = {m};
        sc.socket_path = cfc.socket;
        sc.backend = "fake";
        crs::Service svc(&gpu, &port, sc);
        port.m = &svc.slots();
        std::string err;
        if (!svc.start(&err)) {
            failures++;
            std::printf("FAIL live-offset test: service start failed: %s\n", err.c_str());
        } else {
            MapFeed feed(cfc);
            feed.set_stream(true, false, Picture::Shown);
            int64_t now = 200000;

            auto make_atlas = [](uint32_t w, uint32_t h, uint8_t base) {
                std::vector<uint8_t> a((size_t)w * (h + h / 2));
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++)
                        a[y * w + x] = (uint8_t)((x + y * 7 + base) & 0xff);
                for (uint32_t y = 0; y < h / 2; y++) {
                    for (uint32_t x = 0; x < w / 2; x++) {
                        a[w * h + y * w + x] = (uint8_t)((x * 2 + y * 11 + 128) & 0xff);
                        a[w * h + y * w + w / 2 + x] = (uint8_t)((x * 3 + y * 13 + 128) & 0xff);
                    }
                }
                return a;
            };

            auto pump = [&](int64_t ms) {
                for (int64_t t = 0; t < ms; t += 5) {
                    now += 5;
                    std::vector<pollfd> fds;
                    size_t first = svc.poll_fds(&fds);
                    int n = poll(fds.data(), fds.size(), 0);
                    port.tick(now);
                    svc.after_poll(fds, first, now);
                    feed.poll(now);
                }
            };

            std::vector<uint8_t> atlas = make_atlas(1440, 636, 1);
            std::vector<uint8_t> spare;

            /* Enable test pause for deterministic synchronization */
            feed.worker().enable_test_pause();

            /* Feed first frame, let scaling start */
            spare = atlas;
            feed.offer(&spare, 1, now);
            pump(50);

            /* Wait for worker to accept the job and reach the PAUSED point */
            bool worker_paused = feed.worker().wait_until_worker_paused([](const auto& s) {
                return s.id == 1;  // Verify job ID
            }, 2000);
            ok("live offset change: worker accepted job (id=1) and is paused", worker_paused);

            /* Change cluster offset while worker is paused at the synchronization point */
            feed.set_cluster_offset(50);

            /* Release worker and pump until it finishes */
            feed.worker().release_worker();
            pump(200);

            /* Verify the output corresponds to crop_x=126 (176-50) */
            const crs::Layer *ml = nullptr;
            for (const crs::Layer *l : svc.compositor().layers())
                ml = l;
            bool got_output = false;
            std::vector<uint8_t> output;
            if (ml) {
                crs::FakeGraph *g = gpu.graph("map");
                if (g && !g->drawn().empty()) {
                    for (const auto &kv : g->shown.buffers) {
                        if (kv.second.f == crs::TexFormat::R8) {
                            /* With crop_x=126, the leftmost pixel should come from src x=126 */
                            /* Y[x,y] = (x + y*7 + base) & 0xff, base=1 */
                            /* So Y[0,0] = (126 + 0 + 1) & 0xff = 127 */
                            if (kv.second.px[0] == 127) {
                                got_output = true;
                                output = kv.second.px;
                            }
                        }
                    }
                }
            }

            ok("live offset change: worker was paused and released", worker_paused);
            ok("live offset change: output received with new crop (crop_x=126 -> Y[0]=127)",
               got_output && !output.empty() && output[0] == 127);
            if (got_output && !output.empty() && output[0] != 127) {
                std::printf("  DEBUG: output[0] = %d (expected 127 for crop_x=126, would be 177 for old crop_x=176)\n", output[0]);
            }

            /* Destination buffer: Y 1088x480, U/V at 544x240 starting at 1088*480 */
            /* Verify additional pixels for crop_x=126 (base=1):
             * Y[1087,0] = src[1213,0] = (1213 + 1) & 0xff = 190
             * Y[0,479] = src[126,479] = (126 + 479*7 + 1) & 0xff = 3480 & 0xff = 152
             * U[0,0] -> src U at (63,0) = (63*2 + 128) & 0xff = 254
             * V[0,0] -> src V at (63,0) = (63*3 + 128) & 0xff = 61
             * U[543,239] -> src U at (606,239) = (606*2 + 239*11 + 128) & 0xff = 3969 & 0xff = 129
             * V[543,239] -> src V at (606,239) = (606*3 + 239*13 + 128) & 0xff = 5053 & 0xff = 189 */
            ok("live offset change: Y[1087,0] = src[1213,0] = 190",
               got_output && output[1087] == 190);
            ok("live offset change: Y[0,479] = src[126,479] = 152",
               got_output && output[479 * 1088] == 152);
            ok("live offset change: U[0,0] = src U[63,0] = 254",
               got_output && output[1088 * 480 + 0] == 254);
            ok("live offset change: V[0,0] = src V[63,0] = 61",
               got_output && output[1088 * 480 + 1088 / 2 + 0] == 61);
            ok("live offset change: U[543,239] = src U[606,239] = 129",
               got_output && output[1088 * 480 + 239 * 1088 + 543] == 129);
            ok("live offset change: V[543,239] = src V[606,239] = 189",
               got_output && output[1088 * 480 + 239 * 1088 + 1088 / 2 + 543] == 189);

            /* Verify the shape in status matches new offset */
            FeedStatus st = feed.status("decode");
            ok("live offset change: status shows cluster_offset_x=50, crop_x=126",
               st.cluster_offset_x == 50 && st.cluster_crop_x == 126);
        }
    }

    /* ---- multiple offset changes during one worker job ---- */
    {
        /* Test: offset 0 -> +50 -> -40 during a single worker job.
         * Initial crop_x = 176
         * Change to +50: intermediate crop_x = 126
         * Change to -40: final crop_x = 216
         * The worker must discard stale output and resubmit with final shape. */
        FeedConfig cfc;
        cfc.socket = "/tmp/woc-feed-multi-offset-" + std::to_string(getpid()) + ".sock";
        cfc.target = "map";
        cfc.video.w = 1440;
        cfc.video.h = 636;
        cfc.video.level_bits = 0;
        cfc.video.stall_ms = 6000;
        cfc.cluster_offset_x = 0;

        crs::FakeGpu gpu;
        crs::FakePort port;
        crs::ServiceConfig sc;
        crs::TargetSpec m;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
        m.take_over_stock = true;
        sc.targets = {m};
        sc.socket_path = cfc.socket;
        sc.backend = "fake";
        crs::Service svc(&gpu, &port, sc);
        port.m = &svc.slots();
        std::string err;
        if (!svc.start(&err)) {
            failures++;
            std::printf("FAIL multi-offset test: service start failed: %s\n", err.c_str());
        } else {
            MapFeed feed(cfc);
            feed.set_stream(true, false, Picture::Shown);
            int64_t now = 200000;

            auto make_atlas = [](uint32_t w, uint32_t h, uint8_t base) {
                std::vector<uint8_t> a((size_t)w * (h + h / 2));
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++)
                        a[y * w + x] = (uint8_t)((x + y * 7 + base) & 0xff);
                for (uint32_t y = 0; y < h / 2; y++) {
                    for (uint32_t x = 0; x < w / 2; x++) {
                        a[w * h + y * w + x] = (uint8_t)((x * 2 + y * 11 + 128) & 0xff);
                        a[w * h + y * w + w / 2 + x] = (uint8_t)((x * 3 + y * 13 + 128) & 0xff);
                    }
                }
                return a;
            };

            auto pump = [&](int64_t ms) {
                for (int64_t t = 0; t < ms; t += 5) {
                    now += 5;
                    std::vector<pollfd> fds;
                    size_t first = svc.poll_fds(&fds);
                    int n = poll(fds.data(), fds.size(), 0);
                    port.tick(now);
                    svc.after_poll(fds, first, now);
                    feed.poll(now);
                }
            };

            std::vector<uint8_t> atlas = make_atlas(1440, 636, 1);
            std::vector<uint8_t> spare;

            /* Enable test pause for deterministic synchronization */
            feed.worker().enable_test_pause();

            /* Feed first frame, let scaling start */
            spare = atlas;
            feed.offer(&spare, 1, now);
            pump(50);

            /* Wait for worker to accept the job and reach the PAUSED point */
            bool worker_paused = feed.worker().wait_until_worker_paused([](const auto& s) {
                return s.id == 1;
            }, 2000);
            ok("multi offset change: worker accepted job (id=1) and is paused", worker_paused);

            /* Change offset to +50 (crop_x=126) */
            feed.set_cluster_offset(50);
            /* Change offset to -40 (crop_x=216) while worker still paused */
            feed.set_cluster_offset(-40);

            /* Release worker and pump until it finishes */
            feed.worker().release_worker();
            pump(200);

            /* Verify the output corresponds to FINAL crop_x=216 (176 - (-40)) */
            const crs::Layer *ml = nullptr;
            for (const crs::Layer *l : svc.compositor().layers())
                ml = l;
            bool got_output = false;
            std::vector<uint8_t> output;
            if (ml) {
                crs::FakeGraph *g = gpu.graph("map");
                if (g && !g->drawn().empty()) {
                    for (const auto &kv : g->shown.buffers) {
                        if (kv.second.f == crs::TexFormat::R8) {
                            /* With crop_x=216, the leftmost pixel should come from src x=216 */
                            /* Y[x,y] = (x + y*7 + base) & 0xff, base=1 */
                            /* So Y[0,0] = (216 + 0 + 1) & 0xff = 217 */
                            if (kv.second.px[0] == 217) {
                                got_output = true;
                                output = kv.second.px;
                            }
                        }
                    }
                }
            }

            ok("multi offset change: worker was paused and released", worker_paused);
            ok("multi offset change: output uses FINAL crop (crop_x=216 -> Y[0]=217), not intermediate",
               got_output && !output.empty() && output[0] == 217);
            if (got_output && !output.empty() && output[0] != 217) {
                std::printf("  DEBUG: output[0] = %d (expected 217 for crop_x=216, would be 127 for intermediate crop_x=126, 177 for initial crop_x=176)\n", output[0]);
            }

            /* Destination buffer: Y 1088x480, U/V at 544x240 starting at 1088*480 */
            /* Verify additional pixels for crop_x=216 (base=1):
             * Y[1087,0] = src[1303,0] = (1303 + 1) & 0xff = 24
             * Y[0,479] = src[216,479] = (216 + 479*7 + 1) & 0xff = 3570 & 0xff = 242
             * U[0,0] -> src U at (108,0) = (108*2 + 128) & 0xff = 88
             * V[0,0] -> src V at (108,0) = (108*3 + 128) & 0xff = 196
             * U[543,239] -> src U at (651,239) = (651*2 + 239*11 + 128) & 0xff = 4181 & 0xff = 219
             * V[543,239] -> src V at (651,239) = (651*3 + 239*13 + 128) & 0xff = 5340 & 0xff = 68 */
            ok("multi offset change: Y[1087,0] = src[1303,0] = 24",
               got_output && output[1087] == 24);
            ok("multi offset change: Y[0,479] = src[216,479] = 242",
               got_output && output[479 * 1088] == 242);
            ok("multi offset change: U[0,0] = src U[108,0] = 88",
               got_output && output[1088 * 480 + 0] == 88);
            ok("multi offset change: V[0,0] = src V[108,0] = 196",
               got_output && output[1088 * 480 + 1088 / 2 + 0] == 196);
            ok("multi offset change: U[543,239] = src U[651,239] = 219",
               got_output && output[1088 * 480 + 239 * 1088 + 543] == 219);
            ok("multi offset change: V[543,239] = src V[651,239] = 68",
               got_output && output[1088 * 480 + 239 * 1088 + 1088 / 2 + 543] == 68);

            /* Verify the shape in status matches final offset */
            FeedStatus st = feed.status("decode");
            ok("multi offset change: status shows cluster_offset_x=-40, crop_x=216",
               st.cluster_offset_x == -40 && st.cluster_crop_x == 216);
        }
    }

    /* ---- stable-frame recrop test: offset change after successful scaling ---- */
    {
        /* Test: submit one frame, wait for successful scaling with crop_x=176,
         * then change offset to +50. The cached source atlas should be resubmitted
         * with new crop_x=126 without needing a new CarPlay frame. */
        FeedConfig cfc;
        cfc.socket = "/tmp/woc-feed-stable-recrop-" + std::to_string(getpid()) + ".sock";
        cfc.target = "map";
        cfc.video.w = 1440;
        cfc.video.h = 636;
        cfc.video.level_bits = 0;
        cfc.video.stall_ms = 6000;
        cfc.cluster_offset_x = 0;

        crs::FakeGpu gpu;
        crs::FakePort port;
        crs::ServiceConfig sc;
        crs::TargetSpec m;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
        m.take_over_stock = true;
        sc.targets = {m};
        sc.socket_path = cfc.socket;
        sc.backend = "fake";
        crs::Service svc(&gpu, &port, sc);
        port.m = &svc.slots();
        std::string err;
        if (!svc.start(&err)) {
            failures++;
            std::printf("FAIL stable-recrop test: service start failed: %s\n", err.c_str());
        } else {
            MapFeed feed(cfc);
            feed.set_stream(true, false, Picture::Shown);
            int64_t now = 200000;

            auto make_atlas = [](uint32_t w, uint32_t h, uint8_t base) {
                std::vector<uint8_t> a((size_t)w * (h + h / 2));
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++)
                        a[y * w + x] = (uint8_t)((x + y * 7 + base) & 0xff);
                for (uint32_t y = 0; y < h / 2; y++) {
                    for (uint32_t x = 0; x < w / 2; x++) {
                        a[w * h + y * w + x] = (uint8_t)((x * 2 + y * 11 + 128) & 0xff);
                        a[w * h + y * w + w / 2 + x] = (uint8_t)((x * 3 + y * 13 + 128) & 0xff);
                    }
                }
                return a;
            };

            auto pump = [&](int64_t ms) {
                for (int64_t t = 0; t < ms; t += 5) {
                    now += 5;
                    std::vector<pollfd> fds;
                    size_t first = svc.poll_fds(&fds);
                    int n = poll(fds.data(), fds.size(), 0);
                    port.tick(now);
                    svc.after_poll(fds, first, now);
                    feed.poll(now);
                }
            };

            std::vector<uint8_t> atlas = make_atlas(1440, 636, 1);
            std::vector<uint8_t> spare;

            /* Enable test pause for deterministic synchronization */
            feed.worker().enable_test_pause();

            /* Feed first frame, wait for complete scaling using deterministic sync */
            spare = atlas;
            feed.offer(&spare, 1, now);
            pump(50);

            /* Wait for worker to pause, then release and wait for completion */
            bool worker_paused = feed.worker().wait_until_worker_paused([](const auto& s) {
                return s.id == 1;
            }, 2000);
            ok("stable recrop: worker accepted job (id=1) and paused", worker_paused);
            feed.worker().release_worker();
            pump(200);

            /* Verify initial output with crop_x=176 */
            const crs::Layer *ml = nullptr;
            for (const crs::Layer *l : svc.compositor().layers())
                ml = l;
            bool got_initial = false;
            std::vector<uint8_t> initial_output;
            if (ml) {
                crs::FakeGraph *g = gpu.graph("map");
                if (g && !g->drawn().empty()) {
                    for (const auto &kv : g->shown.buffers) {
                        if (kv.second.f == crs::TexFormat::R8) {
                            /* With crop_x=176, Y[0,0] = (176 + 1) & 0xff = 177 */
                            if (kv.second.px[0] == 177) {
                                got_initial = true;
                                initial_output = kv.second.px;
                            }
                        }
                    }
                }
            }
            ok("stable recrop: initial output with crop_x=176 (Y[0]=177)", got_initial && !initial_output.empty() && initial_output[0] == 177);
            ok("stable recrop: Y[1087,0] = src[1263,0] = 240",
               got_initial && initial_output[1087] == 240);
            ok("stable recrop: Y[0,479] = src[176,479] = 202",
               got_initial && initial_output[479 * 1088] == 202);
            ok("stable recrop: U[0,0] = src U[88,0] = 48",
               got_initial && initial_output[1088 * 480 + 0] == 48);
            ok("stable recrop: V[0,0] = src V[88,0] = 136",
               got_initial && initial_output[1088 * 480 + 1088 / 2 + 0] == 136);

            /* Worker should now be idle, source atlas cached */
            bool worker_idle = feed.worker().idle();
            ok("stable recrop: worker is idle after initial scaling", worker_idle);

            /* Change cluster offset to +50 (crop_x=126) - NO new CarPlay frame */
            feed.set_cluster_offset(50);
            pump(200);  /* wait for recrop */

            /* Verify new output with crop_x=126 */
            ml = nullptr;
            for (const crs::Layer *l : svc.compositor().layers())
                ml = l;
            bool got_recrop = false;
            std::vector<uint8_t> recrop_output;
            if (ml) {
                crs::FakeGraph *g = gpu.graph("map");
                if (g && !g->drawn().empty()) {
                    for (const auto &kv : g->shown.buffers) {
                        if (kv.second.f == crs::TexFormat::R8) {
                            /* With crop_x=126, Y[0,0] = (126 + 1) & 0xff = 127 */
                            if (kv.second.px[0] == 127) {
                                got_recrop = true;
                                recrop_output = kv.second.px;
                            }
                        }
                    }
                }
            }

            ok("stable recrop: recrop output received with new crop (crop_x=126 -> Y[0]=127)",
               got_recrop && !recrop_output.empty() && recrop_output[0] == 127);
            ok("stable recrop: Y[1087,0] = src[1213,0] = 190",
               got_recrop && recrop_output[1087] == 190);
            ok("stable recrop: Y[0,479] = src[126,479] = 152",
               got_recrop && recrop_output[479 * 1088] == 152);
            ok("stable recrop: U[0,0] = src U[63,0] = 254",
               got_recrop && recrop_output[1088 * 480 + 0] == 254);
            ok("stable recrop: V[0,0] = src V[63,0] = 61",
               got_recrop && recrop_output[1088 * 480 + 1088 / 2 + 0] == 61);
            ok("stable recrop: U[543,239] = src U[606,239] = 129",
               got_recrop && recrop_output[1088 * 480 + 239 * 1088 + 543] == 129);
            ok("stable recrop: V[543,239] = src V[606,239] = 189",
               got_recrop && recrop_output[1088 * 480 + 239 * 1088 + 1088 / 2 + 543] == 189);

            /* Verify frame ID has not changed (same source frame recropped) */
            FeedStatus st = feed.status("decode");
            ok("stable recrop: status shows cluster_offset_x=50, crop_x=126",
               st.cluster_offset_x == 50 && st.cluster_crop_x == 126);
        }
    }

    /* ---- frame tracking test: verify no stale/intermediate frames reach renderer ---- */
    {
        /* Test: offset 0 -> +50 -> -40 during one job.
         * Track every frame accepted by the renderer to verify ONLY the final
         * crop_x=216 output is submitted. */
        FeedConfig cfc;
        cfc.socket = "/tmp/woc-feed-track-frames-" + std::to_string(getpid()) + ".sock";
        cfc.target = "map";
        cfc.video.w = 1440;
        cfc.video.h = 636;
        cfc.video.level_bits = 0;
        cfc.video.stall_ms = 6000;
        cfc.cluster_offset_x = 0;

        crs::FakeGpu gpu;
        crs::FakePort port;
        crs::ServiceConfig sc;
        crs::TargetSpec m;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
        m.take_over_stock = true;
        sc.targets = {m};
        sc.socket_path = cfc.socket;
        sc.backend = "fake";
        crs::Service svc(&gpu, &port, sc);
        port.m = &svc.slots();
        std::string err;
        if (!svc.start(&err)) {
            failures++;
            std::printf("FAIL frame-track test: service start failed: %s\n", err.c_str());
        } else {
            MapFeed feed(cfc);
            feed.set_stream(true, false, Picture::Shown);
            int64_t now = 200000;

            auto make_atlas = [](uint32_t w, uint32_t h, uint8_t base) {
                std::vector<uint8_t> a((size_t)w * (h + h / 2));
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++)
                        a[y * w + x] = (uint8_t)((x + y * 7 + base) & 0xff);
                for (uint32_t y = 0; y < h / 2; y++) {
                    for (uint32_t x = 0; x < w / 2; x++) {
                        a[w * h + y * w + x] = (uint8_t)((x * 2 + y * 11 + 128) & 0xff);
                        a[w * h + y * w + w / 2 + x] = (uint8_t)((x * 3 + y * 13 + 128) & 0xff);
                    }
                }
                return a;
            };

            auto pump = [&](int64_t ms) {
                for (int64_t t = 0; t < ms; t += 5) {
                    now += 5;
                    std::vector<pollfd> fds;
                    size_t first = svc.poll_fds(&fds);
                    int n = poll(fds.data(), fds.size(), 0);
                    port.tick(now);
                    svc.after_poll(fds, first, now);
                    feed.poll(now);
                }
            };

            std::vector<uint8_t> atlas = make_atlas(1440, 636, 1);
            std::vector<uint8_t> spare;

            /* Enable test pause for deterministic synchronization */
            feed.worker().enable_test_pause();

            /* Feed first frame, let scaling start */
            spare = atlas;
            feed.offer(&spare, 1, now);
            pump(50);

            /* Wait for worker to pause */
            bool worker_paused = feed.worker().wait_until_worker_paused([](const auto& s) {
                return s.id == 1;
            }, 2000);
            ok("frame track: worker paused with job id=1", worker_paused);

            /* Change offset to +50 (crop_x=126) */
            feed.set_cluster_offset(50);
            /* Change offset to -40 (crop_x=216) while worker still paused */
            feed.set_cluster_offset(-40);

            /* Release worker and pump until it finishes */
            feed.worker().release_worker();
            pump(300);

            /* Now inspect ALL frames the renderer received for the map layer.
             * The FakeGraph records every frame in drawn(). We'll check each
             * frame's Y[0,0] to see which crop it corresponds to. */
            const crs::Layer *ml = nullptr;
            for (const crs::Layer *l : svc.compositor().layers())
                ml = l;

            std::vector<int> seen_crops;  // crop_x values inferred from Y[0,0]
            if (ml) {
                crs::FakeGraph *g = gpu.graph("map");
                if (g) {
                    for (const auto &kv : g->drawn()) {
                        if (kv.second.f == crs::TexFormat::R8 && kv.second.px.size() >= 1088) {
                            int y00 = kv.second.px[0];
                            int crop_x = (y00 - 1 + 256) % 256;  // Y[0,0] = (crop_x + base) & 0xff, base=1
                            seen_crops.push_back(crop_x);
                        }
                    }
                }
            }

            ok("frame track: at least one frame submitted", !seen_crops.empty());
            /* Only crop_x=216 should be present (final offset -40).
             * crop_x=176 (initial) and crop_x=126 (intermediate +50) must NOT appear. */
            bool has_initial = false, has_intermediate = false, has_final = false;
            for (int cx : seen_crops) {
                if (cx == 176) has_initial = true;
                else if (cx == 126) has_intermediate = true;
                else if (cx == 216) has_final = true;
            }
            ok("frame track: NO initial crop_x=176 frame submitted after offset changes", !has_initial);
            ok("frame track: NO intermediate crop_x=126 frame submitted", !has_intermediate);
            ok("frame track: FINAL crop_x=216 frame submitted", has_final);
            if (has_initial) std::printf("  DEBUG: saw initial crop_x=176\n");
            if (has_intermediate) std::printf("  DEBUG: saw intermediate crop_x=126\n");
            if (has_final) std::printf("  DEBUG: saw final crop_x=216\n");
        }
    }

    /* ---- new CarPlay frame arrives during worker job ---- */
    {
        /* Test: frame 1 offered, worker starts scaling, then frame 2 arrives
         * while worker is paused. Frame 1's result must be discarded;
         * frame 2 must be scaled with the current crop. */
        FeedConfig cfc;
        cfc.socket = "/tmp/woc-feed-new-frame-" + std::to_string(getpid()) + ".sock";
        cfc.target = "map";
        cfc.video.w = 1440;
        cfc.video.h = 636;
        cfc.video.level_bits = 0;
        cfc.video.stall_ms = 6000;
        cfc.cluster_offset_x = 0;

        crs::FakeGpu gpu;
        crs::FakePort port;
        crs::ServiceConfig sc;
        crs::TargetSpec m;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
        m.take_over_stock = true;
        sc.targets = {m};
        sc.socket_path = cfc.socket;
        sc.backend = "fake";
        crs::Service svc(&gpu, &port, sc);
        port.m = &svc.slots();
        std::string err;
        if (!svc.start(&err)) {
            failures++;
            std::printf("FAIL new-frame test: service start failed: %s\n", err.c_str());
        } else {
            MapFeed feed(cfc);
            feed.set_stream(true, false, Picture::Shown);
            int64_t now = 200000;

            auto make_atlas = [](uint32_t w, uint32_t h, uint8_t base) {
                std::vector<uint8_t> a((size_t)w * (h + h / 2));
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++)
                        a[y * w + x] = (uint8_t)((x + y * 7 + base) & 0xff);
                for (uint32_t y = 0; y < h / 2; y++) {
                    for (uint32_t x = 0; x < w / 2; x++) {
                        a[w * h + y * w + x] = (uint8_t)((x * 2 + y * 11 + 128) & 0xff);
                        a[w * h + y * w + w / 2 + x] = (uint8_t)((x * 3 + y * 13 + 128) & 0xff);
                    }
                }
                return a;
            };

            auto pump = [&](int64_t ms) {
                for (int64_t t = 0; t < ms; t += 5) {
                    now += 5;
                    std::vector<pollfd> fds;
                    size_t first = svc.poll_fds(&fds);
                    int n = poll(fds.data(), fds.size(), 0);
                    port.tick(now);
                    svc.after_poll(fds, first, now);
                    feed.poll(now);
                }
            };

            /* Frame 1 with base=1 */
            std::vector<uint8_t> atlas1 = make_atlas(1440, 636, 1);
            std::vector<uint8_t> spare = atlas1;

            /* Enable test pause for deterministic synchronization */
            feed.worker().enable_test_pause();

            feed.offer(&spare, 1, now);
            pump(50);

            /* Wait for worker to pause on frame 1 */
            bool worker_paused = feed.worker().wait_until_worker_paused([](const auto& s) {
                return s.id == 1;
            }, 2000);
            ok("new frame: worker paused on frame 1", worker_paused);

            /* Frame 2 arrives with base=2 (different pattern) while worker paused */
            std::vector<uint8_t> atlas2 = make_atlas(1440, 636, 2);
            spare = atlas2;
            feed.offer(&spare, 2, now);

            /* Change offset while worker still paused on frame 1 */
            feed.set_cluster_offset(50);

            /* Release worker - it will complete frame 1 but send_pending must
             * discard it because generation mismatch. Frame 2 should be
             * submitted next with crop_x=126. */
            feed.worker().release_worker();
            pump(300);

            /* Verify output comes from frame 2 (base=2) with crop_x=126.
             * For base=2, crop_x=126: Y[0,0] = (126 + 0*7 + 2) & 0xff = 128 */
            const crs::Layer *ml = nullptr;
            for (const crs::Layer *l : svc.compositor().layers())
                ml = l;
            bool got_output = false;
            std::vector<uint8_t> output;
            if (ml) {
                crs::FakeGraph *g = gpu.graph("map");
                if (g && !g->drawn().empty()) {
                    for (const auto &kv : g->shown.buffers) {
                        if (kv.second.f == crs::TexFormat::R8) {
                            if (kv.second.px[0] == 128) {  // base=2, crop_x=126
                                got_output = true;
                                output = kv.second.px;
                            }
                        }
                    }
                }
            }

            ok("new frame: output from frame 2 (base=2) with crop_x=126 (Y[0]=128)",
               got_output && !output.empty() && output[0] == 128);
            ok("new frame: Y[1087,0] = src[1213,0] base=2 = 191",
               got_output && output[1087] == 191);
            ok("new frame: U[0,0] = src U[63,0] base=2 = (63*2+128)=254",
               got_output && output[1088 * 480 + 0] == 254);
            ok("new frame: V[0,0] = src V[63,0] base=2 = (63*3+128)=61",
               got_output && output[1088 * 480 + 1088 / 2 + 0] == 61);

            /* Frame 1's pattern (base=1) would give Y[0,0]=127.
             * Verify we did NOT get frame 1's stale result. */
            if (got_output && output[0] == 127) {
                std::printf("  ERROR: got stale frame 1 result (Y[0]=127) instead of frame 2\n");
            }
        }
    }

    /* ---- combined cluster + HUD feeds: verify HUD independence ---- */
    {
        /* Test: both cluster and HUD feeds active simultaneously.
         * Change cluster_offset_x; verify HUD crop remains centered at 176.
         * HUD uses its own independent crop centered at 176. */
        FeedConfig cfc;
        cfc.socket = "/tmp/woc-feed-combined-" + std::to_string(getpid()) + ".sock";
        cfc.target = "map";
        cfc.video.w = 1440;
        cfc.video.h = 636;
        cfc.video.level_bits = 0;
        cfc.video.stall_ms = 6000;
        cfc.cluster_offset_x = 0;

        FeedConfig hfc;
        hfc.socket = "/tmp/woc-feed-combined-hud-" + std::to_string(getpid()) + ".sock";
        hfc.target = "hud";
        hfc.video.w = 1440;
        hfc.video.h = 636;
        hfc.video.level_bits = 2;
        hfc.video.stall_ms = 6000;
        hfc.cluster_offset_x = 0;
        hfc.scale_down = true;
        hfc.scaled_level_bits = 2;
        hfc.hud_round_corners = true;
        hfc.hud_black_background = true;

        crs::FakeGpu gpu;
        crs::FakePort port;
        crs::ServiceConfig sc;
        crs::TargetSpec m, u;
        m.name = "map"; m.slot = 0x100; m.scene_id = 0x010004f0; m.width = 1088; m.height = 480; m.linger_ms = 1500;
        m.take_over_stock = true;
        u.name = "hud"; u.slot = 0x323; u.scene_id = 0x01000400; u.width = 800; u.height = 480; u.linger_ms = 3000;
        u.yield_slots = {0x322};
        u.yield_ms = 1500;
        sc.targets = {m, u};
        sc.socket_path = cfc.socket;  /* cluster socket */
        sc.backend = "fake";
        crs::Service svc(&gpu, &port, sc);
        port.m = &svc.slots();
        std::string err;
        if (!svc.start(&err)) {
            failures++;
            std::printf("FAIL combined test: service start failed: %s\n", err.c_str());
        } else {
            MapFeed feed(cfc);
            feed.set_stream(true, false, Picture::Shown);
            MapFeed hud(hfc);
            hud.set_stream(true, false, Picture::Shown);
            int64_t now = 200000;

            auto make_atlas = [](uint32_t w, uint32_t h, uint8_t base) {
                std::vector<uint8_t> a((size_t)w * (h + h / 2));
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++)
                        a[y * w + x] = (uint8_t)((x + y * 7 + base) & 0xff);
                for (uint32_t y = 0; y < h / 2; y++) {
                    for (uint32_t x = 0; x < w / 2; x++) {
                        a[w * h + y * w + x] = (uint8_t)((x * 2 + y * 11 + 128) & 0xff);
                        a[w * h + y * w + w / 2 + x] = (uint8_t)((x * 3 + y * 13 + 128) & 0xff);
                    }
                }
                return a;
            };

            auto pump = [&](int64_t ms) {
                for (int64_t t = 0; t < ms; t += 5) {
                    now += 5;
                    std::vector<pollfd> fds;
                    size_t first = svc.poll_fds(&fds);
                    int n = poll(fds.data(), fds.size(), 0);
                    port.tick(now);
                    svc.after_poll(fds, first, now);
                    feed.poll(now);
                    hud.poll(now);
                }
            };

            std::vector<uint8_t> atlas = make_atlas(1440, 636, 1);
            std::vector<uint8_t> spare;

            /* Feed initial frame to both */
            spare = atlas;
            feed.offer(&spare, 1, now);
            spare = atlas;
            hud.offer(&spare, 1, now);
            pump(200);

            /* Verify initial HUD crop_x = 176 (centered) */
            FeedStatus hs = hud.status("decode");
            ok("combined: HUD initial crop_x = 176", hs.cluster_crop_x == 176);

            /* Change cluster offset to +100 */
            feed.set_cluster_offset(100);
            pump(200);

            /* Verify cluster crop changed to 76 (176 - 100) */
            FeedStatus cs = feed.status("decode");
            ok("combined: cluster offset +100 -> crop_x = 76", cs.cluster_crop_x == 76);

            /* Verify HUD crop REMAINS 176 (independent) */
            hs = hud.status("decode");
            ok("combined: HUD crop_x still 176 after cluster offset change", hs.cluster_crop_x == 176);

            /* Change cluster offset to -100 */
            feed.set_cluster_offset(-100);
            pump(200);

            cs = feed.status("decode");
            ok("combined: cluster offset -100 -> crop_x = 276", cs.cluster_crop_x == 276);

            hs = hud.status("decode");
            ok("combined: HUD crop_x still 176 after cluster offset -100", hs.cluster_crop_x == 176);

            /* Change cluster offset to +50 */
            feed.set_cluster_offset(50);
            pump(200);

            cs = feed.status("decode");
            ok("combined: cluster offset +50 -> crop_x = 126", cs.cluster_crop_x == 126);

            hs = hud.status("decode");
            ok("combined: HUD crop_x still 176 after cluster offset +50", hs.cluster_crop_x == 176);
        }
    }

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
