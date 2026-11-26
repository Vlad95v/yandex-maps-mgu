// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* hud_crop.h: the HUD map's centred horizontal crop and the box it is shown
 * in, from the Size setting's box and hud_crop_width_pct.
 *
 * The bounds are the scaler's (atlas_scale.h: even values, the crop inside
 * the picture, the box no wider than the crop) and the renderer's (a video
 * narrower than 64 px is refused). */
#include <cstdio>
#include <cstdint>
#include <initializer_list>

#include "hud_crop.h"

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

/* The box the Size setting makes on the HUD: the fitted 800x352 for the
 * standard size, or 85 % of the HUD's 800x480 at the widest the app asks
 * for (680x300), each rounded down to a multiple of 4 as map_feed.cpp does. */
struct Case {
    uint32_t frame_w, frame_h, box_w, box_h;
    const char *name;
};

int main()
{
    const Case wide{1440, 636, 680, 300, "wide 1440x636, Size 85 % (680x300)"};
    const Case fitted{1440, 636, 800, 352, "wide 1440x636, standard Size (800x352)"};
    const Case plain{1088, 480, 800, 352, "1088x480 source, standard Size"};

    /* ---- the wide frame's own region: the centred 1088x480 at X 176 ---- */
    const HudSource src = hud_source(wide.frame_w, wide.frame_h);
    ok("1440x636: the HUD's own 1088x480", src.x == 176 && src.y == 0 && src.w == 1088 && src.h == 480);
    const HudSource all = hud_source(1088, 480);
    ok("1088x480: the whole frame", all.x == 0 && all.y == 0 && all.w == 1088 && all.h == 480);

    /* 1. 100 %: exactly the geometry before hud_crop_width_pct */
    for (const Case *c : {&wide, &fitted, &plain}) {
        const HudGeometry g = hud_geometry(c->frame_w, c->frame_h, c->box_w, c->box_h, 100);
        char name[160];
        std::snprintf(name, sizeof name, "100 %%: %s, the box unchanged", c->name);
        ok(name, g.box.w == c->box_w && g.box.h == c->box_h);
        std::snprintf(name, sizeof name, "100 %%: %s, the whole region shown", c->name);
        ok(name, g.crop.x == g.src.x && g.crop.y == g.src.y && g.crop.w == g.src.w && g.crop.h == g.src.h);
    }

    /* 2. 20 %: the narrowest the app offers */
    {
        const HudGeometry g = hud_geometry(wide.frame_w, wide.frame_h, wide.box_w, wide.box_h, 20);
        ok("20 %: a fifth as wide a box", g.box.w == 136);
        ok("20 %: the height kept", g.box.h == 300);
        ok("20 %: a fifth of the region's width", g.crop.w >= 208 && g.crop.w <= 218);
        ok("20 %: the whole height of the region", g.crop.h == 480 && g.crop.y == 0);
        ok("20 %: centred (X 176 + (1088-218)/2, even)", g.crop.x == 610);
    }

    /* 3. intermediate values: the share of the width, centred, aligned */
    for (unsigned pct : {25u, 50u, 75u, 95u}) {
        const HudGeometry g = hud_geometry(wide.frame_w, wide.frame_h, wide.box_w, wide.box_h, pct);
        char name[160];
        std::snprintf(name, sizeof name, "%u %%: the box that share of the width", pct);
        const uint32_t want = (uint32_t)(((uint64_t)wide.box_w * pct + 50) / 100);
        ok(name, g.box.w + 3 >= want && g.box.w <= want + 2);
        std::snprintf(name, sizeof name, "%u %%: the height kept", pct);
        ok(name, g.box.h == wide.box_h && g.crop.h == 480);
        std::snprintf(name, sizeof name, "%u %%: the crop centred on the region's centre", pct);
        /* an even X (the scaler's, atlas_scale.h) can leave it a pixel off */
        const long centre = (long)g.crop.x + (long)g.crop.w / 2;
        ok(name, centre >= 719 && centre <= 721);
    }

    /* 4. the scaler's and the renderer's requirements, every step */
    {
        bool aligned = true, inside = true, fits = true, narrow_ok = true;
        for (unsigned pct = 20; pct <= 100; pct += 5) {
            for (const Case *c : {&wide, &fitted, &plain}) {
                const HudGeometry g = hud_geometry(c->frame_w, c->frame_h, c->box_w, c->box_h, pct);
                aligned = aligned && g.crop.x % 2 == 0 && g.crop.w % 2 == 0 && g.box.w % 4 == 0 && g.box.h % 4 == 0;
                inside = inside && (uint64_t)g.crop.x + g.crop.w <= g.src.w + g.src.x && g.crop.y == 0 &&
                         g.crop.h == g.src.h && g.crop.x >= g.src.x;
                fits = fits && g.box.w <= g.crop.w && g.box.h <= g.crop.h;
                narrow_ok = narrow_ok && g.box.w >= 64 && g.box.w <= c->box_w;
            }
        }
        ok("every step: the crop even and inside the picture, the box a multiple of 4", aligned && inside);
        ok("every step: the box no wider than the crop, no higher than its height", fits);
        ok("every step: at least the renderer's 64 px, no wider than the Size box", narrow_ok);
    }

    /* 5. the output height does not change with the width alone */
    {
        bool same = true;
        const HudGeometry first = hud_geometry(wide.frame_w, wide.frame_h, wide.box_w, wide.box_h, 20);
        for (unsigned pct = 25; pct <= 100; pct += 5) {
            const HudGeometry g = hud_geometry(wide.frame_w, wide.frame_h, wide.box_w, wide.box_h, pct);
            same = same && g.box.h == first.box.h;
        }
        ok("20-100 %: the box's height never changes", same);
    }

    /* 6. the crop narrows the box, not squeezes the picture: the pixel scale
     * stays the whole region's (1.5 %, the boxes being multiples of 4; the
     * pre-existing difference between the frame's shape and the region's is
     * not this setting's) */
    {
        bool uniform = true;
        const double base = (double)wide.box_w / 1088;
        for (unsigned pct = 20; pct <= 100; pct += 5) {
            const HudGeometry g = hud_geometry(wide.frame_w, wide.frame_h, wide.box_w, wide.box_h, pct);
            const double scale = (double)g.box.w / g.crop.w;
            uniform = uniform && scale > base * 0.985 && scale < base * 1.015;
        }
        ok("20-100 %: the horizontal pixel scale as the whole region's", uniform);
    }

    /* 7. the small Size boxes: the 64 px minimum, the crop narrowed with it */
    {
        const HudGeometry g = hud_geometry(wide.frame_w, wide.frame_h, 240, 106, 20);
        ok("Size 30 %, Width 20 %: the narrowest video, 64 px", g.box.w == 64);
        ok("Size 30 %, Width 20 %: the crop the box can show", g.crop.w >= 280 && g.crop.w <= 300);
        ok("Size 30 %, Width 20 %: the pixel scale kept", (double)g.box.w / g.crop.w > 0.21 && (double)g.box.w / g.crop.w < 0.23);
    }

    /* 8. 100 % with the standard box: unchanged, as it always was */
    {
        const HudGeometry g = hud_geometry(wide.frame_w, wide.frame_h, 800, 352, 100);
        ok("standard Size, Width 100 %: 800x352 of the 1088x480 region at X 176",
           g.box.w == 800 && g.box.h == 352 && g.crop.x == 176 && g.crop.w == 1088);
    }

    /* 9. out-of-range settings are clamped, never fail */
    {
        const HudGeometry lo = hud_geometry(wide.frame_w, wide.frame_h, wide.box_w, wide.box_h, 0);
        const HudGeometry min = hud_geometry(wide.frame_w, wide.frame_h, wide.box_w, wide.box_h, 20);
        ok("0 %: the narrowest, 20 %", lo.box.w == min.box.w && lo.crop.w == min.crop.w);
        const HudGeometry hi = hud_geometry(wide.frame_w, wide.frame_h, wide.box_w, wide.box_h, 500);
        const HudGeometry full = hud_geometry(wide.frame_w, wide.frame_h, wide.box_w, wide.box_h, 100);
        ok("500 %: the whole region, 100 %", hi.box.w == full.box.w && hi.crop.w == full.crop.w);
    }

    /* 10. the HUD's region is its own: no offset, no cluster crop */
    {
        const HudGeometry a = hud_geometry(wide.frame_w, wide.frame_h, 680, 300, 60);
        ok("the HUD's region starts at X 176 and is 1088 wide at every setting", a.src.x == 176 && a.src.w == 1088);
        ok("the HUD's crop never leaves the region", a.crop.x >= 176 && a.crop.x + a.crop.w <= 1264);
    }

    if (failures)
        std::printf("%d of %d FAILED\n", failures, checks);
    else
        std::printf("all %d ok\n", checks);
    return failures ? 1 : 0;
}
