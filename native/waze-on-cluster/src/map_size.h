// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Validate the decoder's tex_size. For wide CarPlay the phone renders
 * 1440x636 and MapFeed crops 1088x480 for the cluster renderer, so the
 * texture-size contract applies to that output, not to the source size.
 * Every other tex_size is the map texture itself and is checked as before. */
#ifndef WOC_MAP_SIZE_H
#define WOC_MAP_SIZE_H

#include <stdint.h>

#include "map_feed.h"
#include "map_geometry.h"

namespace woc {

inline const char *map_size_error(uint32_t w, uint32_t h)
{
    const bool wide = w == kCarplayWideWidth && h == kCarplayWideHeight;
    return crk::texture_size_error(wide ? crk::kMapAreaWidth : w, wide ? crk::kMapAreaHeight : h);
}

}  // namespace woc

#endif
