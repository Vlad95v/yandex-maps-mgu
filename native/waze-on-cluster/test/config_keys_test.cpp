// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Host test of the tool's configuration keys (config_keys.h): the shipped
 * waze-on-cluster.conf, which names cluster_crop_w and cluster_crop_y, loads
 * whole; their defaults are the 1088x480 baseline; a custom crop is accepted
 * through the same keys; and an unknown key is still refused. */
#include <cstdio>
#include <string>

#include "config.h"
#include "config_keys.h"

using namespace woc;

static int failures;

static void ok(const char *name, bool cond)
{
    std::printf("%-4s %s\n", cond ? "ok" : "FAIL", name);
    if (!cond) failures++;
}

int main()
{
    cld::Config defaults;
    define_keys(&defaults);
    ok("cluster_crop_w defaults to 1088", defaults.num("cluster_crop_w") == 1088);
    ok("cluster_crop_y defaults to 0", defaults.num("cluster_crop_y") == 0);

    /* The actual shipped configuration: both keys are in it (waze-on-cluster.conf),
     * so a missing define() makes this load fail, as it did on the car. */
    cld::Config cfg;
    define_keys(&cfg);
    std::string err;
    ok("the shipped waze-on-cluster.conf loads with cluster_crop_w and cluster_crop_y",
       cfg.load("waze-on-cluster.conf", &err));
    if (!err.empty())
        std::printf("     %s\n", err.c_str());
    ok("its crop width is the baseline 1088", cfg.num("cluster_crop_w") == 1088);
    ok("its crop top is 0", cfg.num("cluster_crop_y") == 0);
    const unsigned w = (unsigned)cfg.num("cluster_crop_w");
    const unsigned h = 2 * ((w * 15 + 34) / 68);
    ok("the default crop is 1088x480", w == 1088 && h == 480);
    ok("the default crop starts at x 176 (centred in 1440)", (1440 - (int)w) / 2 == 176);

    /* The calibration page's values, through the same keys. */
    cld::Config custom;
    define_keys(&custom);
    ok("cluster_crop_w 1200 is accepted", custom.set_pair("cluster_crop_w=1200", &err));
    ok("cluster_crop_y 40 is accepted", custom.set_pair("cluster_crop_y=40", &err));
    ok("and reads back", custom.num("cluster_crop_w") == 1200 && custom.num("cluster_crop_y") == 40);

    /* An unregistered key is still an error: the check above is not vacuous. */
    ok("an unknown key is refused", !custom.set_pair("cluster_crop_x=176", &err) && !err.empty());

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
