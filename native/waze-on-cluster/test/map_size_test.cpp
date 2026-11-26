// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* The tex_size validator: wide CarPlay 1440x636 is a source size that MapFeed
 * crops to 1088x480, so it must pass; any other tex_size is the map texture
 * itself and keeps the cluster renderer's 1088x480 contract. */
#include <cstdio>
#include <cstdint>

#include "map_size.h"

using namespace woc;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

int main()
{
    const struct {
        uint32_t w, h;
        bool valid;
        const char *name;
    } cases[] = {
        {1088, 480, true, "1088x480 is the map texture"},
        {1440, 636, true, "1440x636 wide CarPlay is cropped to 1088x480"},
        {1440, 480, false, "1440x480 is not the wide source and is too wide"},
        {1920, 900, false, "1920x900 stock geometry is not a map texture"},
        {1440, 634, false, "1440x634 is not exactly the wide source"},
        {1440, 638, false, "1440x638 is not exactly the wide source"},
        {1088, 636, false, "1088x636 is too high for the map area"},
        {1090, 480, false, "1090x480 is too wide for the map area"},
        {1087, 480, false, "1087x480 has an odd width"},
        {1088, 481, false, "1088x481 has an odd height"},
        {64, 32, true, "64x32 is the lower bound"},
    };
    for (const auto &c : cases)
        ok(c.name, (map_size_error(c.w, c.h) == nullptr) == c.valid);
    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
