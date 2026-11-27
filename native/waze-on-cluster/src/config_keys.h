// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* The configuration keys this tool reads beyond the shared Config's own:
 * registered before any configuration file is loaded, so an unknown key in
 * waze-on-cluster.conf is an error, as it should be. */
#ifndef WOC_CONFIG_KEYS_H
#define WOC_CONFIG_KEYS_H

namespace cld {
class Config;
}

namespace woc {

void define_keys(cld::Config *cfg);

}  // namespace woc

#endif
