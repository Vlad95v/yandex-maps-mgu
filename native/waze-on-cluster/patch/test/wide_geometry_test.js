// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* The wide CarPlay geometry must agree in all four places: the patcher's
 * --res wide, patchd's high-panel plan, the shipped decoder tex_size and the
 * MapFeed constants. One side changing alone fails this test. */
'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');

const HERE = path.join(__dirname, '..', '..');
const P = path.join(HERE, 'patch');

const wide = require(path.join(P, 'carplay_patch.js')).resFor('high', 'wide');
assert.deepStrictEqual(
    { w: wide.w, h: wide.h, mmw: wide.mmw, mmh: wide.mmh, safew: wide.safew, safeh: wide.safeh, safex: wide.safex, safey: wide.safey },
    { w: 1440, h: 636, mmw: 183, mmh: 81, safew: 667, safeh: 488, safex: 379, safey: 95 },
    'carplay_patch RES_WIDE geometry and safe area');

const conf = fs.readFileSync(path.join(HERE, 'waze-on-cluster.conf'), 'utf8');
assert(/^tex_size = 1440x636$/m.test(conf), 'waze-on-cluster.conf tex_size is the wide source');

const patchd = fs.readFileSync(path.join(P, 'patchd.js'), 'utf8');
assert(/if \(panel === 'high'\) args\.push\('--res', res \|\| 'wide'\);/.test(patchd),
    'patchd planArgs passes --res wide by default, the calibration selection otherwise');
assert(/require\('\.\/resolution\.js'\)/.test(patchd), 'patchd uses the shared calibration geometry');

const feed = fs.readFileSync(path.join(HERE, 'src', 'map_feed.h'), 'utf8');
assert(/kCarplayWideWidth = 1440/.test(feed) && /kCarplayWideHeight = 636/.test(feed),
    'MapFeed has the 1440x636 wide constants');

const size = fs.readFileSync(path.join(HERE, 'src', 'map_size.h'), 'utf8');
assert(/kCarplayWideWidth/.test(size) && /kCarplayWideHeight/.test(size) && /kMapAreaWidth/.test(size) &&
    /kMapAreaHeight/.test(size), 'map_size validates the 1088x480 output for the wide source');

console.log('ok   wide CarPlay 1440x636 agrees in patcher, patchd, config and MapFeed');
