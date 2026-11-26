// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* The HUD map's horizontal crop: the part of the decoded picture the HUD
 * shows, and the box it is shown in, from the two settings that shape it.
 *
 * The HUD has always shown a centred part of the picture rather than the
 * whole wide one: from the wide CarPlay frame (1440x636) the 1088x480 in the
 * middle, its own region, independent of the cluster's crop and of
 * cluster_offset_x; from any other frame the whole of it (map_feed.cpp). The
 * Size setting (hud_width_pct) sizes that region on the HUD: a share of the
 * HUD's width, its height from the picture's shape, centred.
 *
 * hud_crop_width_pct takes a centred share of that region's *width*, the
 * height and so the vertical scale kept: at 100 % the region as it always
 * was (no change at all), at 50 % the middle half of it, in a box half as
 * wide and as high as before, with the roads, the route and the labels no
 * smaller than they were. The box narrows, it is not squeezed.
 *
 * The numbers are the scaler's (atlas_scale.h: even values, the crop inside
 * the picture, the box no wider than the crop and no higher than its
 * height) and the renderer's (cluster-render's video_layer.cpp refuses a
 * video narrower than 64 px): a box too narrow for the requested share gets
 * the narrowest crop it can show, never a map scaled unevenly. */
#ifndef WOC_HUD_CROP_H
#define WOC_HUD_CROP_H

#include <stdint.h>

namespace woc {

/* The wide CarPlay frame's size (map_feed.h kCarplayWideWidth/Height). */
const uint32_t kHudWideFrameW = 1440, kHudWideFrameH = 636;

/* The share of the region's width: the setting's range. */
const unsigned kHudCropMinPct = 20, kHudCropMaxPct = 100;

/* The renderer's narrowest video (cluster-render's video_layer.cpp). */
const uint32_t kHudBoxMinW = 64;

/* The HUD's own region of the decoded frame: the wide frame's centred
 * 1088x480, the whole frame otherwise. */
struct HudSource {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};

inline HudSource hud_source(uint32_t frame_w, uint32_t frame_h)
{
    if (frame_w == kHudWideFrameW && frame_h == kHudWideFrameH)
        return HudSource{176, 0, 1088, 480};
    return HudSource{0, 0, frame_w, frame_h};
}

inline unsigned hud_crop_pct(unsigned pct)
{
    return pct < kHudCropMinPct ? kHudCropMinPct : pct > kHudCropMaxPct ? kHudCropMaxPct : pct;
}

struct HudCrop {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};

struct HudBox {
    uint32_t w = 0, h = 0;
};

struct HudGeometry {
    HudSource src;
    HudCrop crop;
    HudBox box;
};

/* What the HUD shows of a frame_w x frame_h picture, in a box the whole
 * region would be shown in of box_w x box_h (the Size setting's box), at
 * pct of the region's width: the box that share as wide at the same height,
 * the crop of the picture that fills it at the same pixel scale as before.
 *
 * 100 % is the region itself in that box, exactly the geometry before
 * hud_crop_width_pct: the old 1088x480 at X 176 of the wide frame, in the
 * box the Size setting makes. */
inline HudGeometry hud_geometry(uint32_t frame_w, uint32_t frame_h, uint32_t box_w, uint32_t box_h, unsigned pct)
{
    HudGeometry g;
    g.src = hud_source(frame_w, frame_h);
    g.box.h = box_h;
    if (!g.src.w || !box_w) {
        g.box.w = box_w & ~3u;
        return g;
    }
    const unsigned share = hud_crop_pct(pct);
    /* the box: that share of the width the Size setting made, to the nearest
     * multiple of 4 (the renderer's videos are, and a truncated box would
     * squeeze the map sideways a little) */
    uint32_t dw = (uint32_t)(((uint64_t)box_w * share + 50) / 100 + 2) & ~3u;
    if (dw > box_w)
        dw = box_w & ~3u;
    /* the crop: the same share of the region, its width even; the pixel scale
     * stays the whole region's (box_w of src.w), so 100 % is the region
     * itself in the box the Size setting made. */
    uint32_t cw = (uint32_t)(((uint64_t)g.src.w * share + 50) / 100 + 1) & ~1u;
    if (dw < kHudBoxMinW && share < kHudCropMaxPct) {
        /* the renderer's narrowest video: the crop the box can show at the
         * region's scale, so that the map is never scaled unevenly */
        dw = kHudBoxMinW;
        cw = (uint32_t)(((uint64_t)g.src.w * dw + box_w / 2) / box_w + 1) & ~1u;
    }
    const uint32_t most = g.src.w & ~1u;
    if (cw > most)
        cw = most;
    if (cw < (dw & ~1u))
        cw = dw & ~1u;
    g.crop.w = cw;
    g.crop.h = g.src.h;
    g.crop.y = g.src.y;
    g.crop.x = (g.src.x + (g.src.w - cw) / 2) & ~1u;
    g.box.w = dw;
    return g;
}

}  // namespace woc

#endif
