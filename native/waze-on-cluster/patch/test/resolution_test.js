// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/**
 * The calibration geometry the patcher and patchd share
 * (patch/resolution.js), and what carplay_patch.js makes of it: an arbitrary
 * even width 1088..1920 gets the 1088x480 shape's height, the physical size
 * the two known configurations interpolate to, and the scaled safe area; the
 * patcher reads such a geometry as its own. The app's copy
 * (apps/robertn_waze_cluster/src/calibration.js) is checked against it.
 *
 *   node patch/test/resolution_test.js
 */
const assert = require('assert');
const path = require('path');
const r = require('../resolution.js');
const patcher = require('../carplay_patch.js');
const app = require(path.join(__dirname, '..', '..', '..', '..', 'apps', 'robertn_waze_cluster', 'src', 'calibration.js'));

let checks = 0;
const ok = (cond, msg) => { checks += 1; assert(cond, msg); };
const eq = (got, want, msg) => { checks += 1; assert.deepStrictEqual(got, want, msg); };

/* ---- the height examples (the task's own list) ---- */
eq([1088, 1100, 1200, 1300, 1400, 1440, 1500, 1600, 1800, 1920].map(r.heightForWidth),
    [480, 486, 530, 574, 618, 636, 662, 706, 794, 848], 'height');
ok(r.heightForWidth(1088) === 480 && r.heightForWidth(1440) === 636, 'both known configurations');

/* ---- the physical size ---- */
eq([[1088], [1200], [1300], [1320], [1400], [1440], [1600], [1920]].map(([w]) => {
    const m = r.mmForWidth(w);
    return [m.w, m.h];
}), [[138, 61], [152, 67], [165, 73], [168, 74], [178, 79], [183, 81], [203, 90], [244, 108]], 'mm');

/* ---- the safe area ---- */
eq(r.safeFor(1440, 636), { x: 379, y: 95, w: 667, h: 488 }, 'the wide safe area');
eq(r.safeFor(1088, 480), { x: 286, y: 72, w: 504, h: 368 }, 'the default safe area');

/* ---- the patcher's geometry: exact at both known sizes, interpolated between and above ---- */
const wide = patcher.resFor('high', 'wide');
eq([wide.w, wide.h, wide.mmw, wide.mmh, wide.safew, wide.safeh, wide.safex, wide.safey],
    [1440, 636, 183, 81, 667, 488, 379, 95], '--res wide unchanged');
const def = patcher.resFor('high', '1088x480');
eq([def.w, def.h, def.mmw, def.mmh, def.safew, def.safeh, def.safex, def.safey],
    [1088, 480, 138, 61, 504, 368, 286, 72], '1088x480 unchanged');
const c1200 = patcher.resFor('high', '1200x530');
eq([c1200.w, c1200.h, c1200.mmw, c1200.mmh], [1200, 530, 152, 67], '1200 -> 1200x530 at 152x67 mm');
const c1920 = patcher.resFor('high', '1920x848');
eq([c1920.w, c1920.h, c1920.mmw, c1920.mmh], [1920, 848, 244, 108], '1920 -> 1920x848 at 244x108 mm');
/* a smaller legacy size keeps the default physical size */
const legacy = patcher.resFor('high', '800x360');
eq([legacy.mmw, legacy.mmh], [138, 61], 'a legacy small --res keeps 138x61');

/* ---- what the patcher reads as its own ---- */
function geom(w, h, mmw, mmh) {
    const s = r.safeFor(w, h);
    return { w, h, mmw, mmh, viewx: 0, viewy: 0, safew: s.w, safex: s.x, safeh: s.h, safey: s.y };
}
ok(patcher.geometryState(geom(1200, 530, 152, 67)).state === 'patched', 'a calibration geometry is patched');
ok(patcher.geometryState(geom(1920, 848, 244, 108)).state === 'patched', 'the top of the range too');
ok(patcher.geometryState(geom(1600, 706, 203, 90)).state === 'patched', 'an intermediate one too');
ok(patcher.geometryState(geom(1200, 530, 138, 61)).state === 'patched', 'a size an earlier release wrote is kept');
const stock = { w: 1920, h: 900, mmw: 261, mmh: 114, viewx: 0, viewy: 0, safew: 1920, safex: 0, safeh: 900, safey: 0 };
ok(patcher.geometryState(stock).state === 'original', 'a stock geometry is original');
const odd = { w: 1200, h: 600, mmw: 0, mmh: 0, viewx: 0, viewy: 0, safew: 1200, safex: 0, safeh: 600, safey: 0 };
ok(patcher.geometryState(odd).state === 'unknown', 'a geometry without physical size is unknown');
const moved = geom(1200, 530, 152, 67);
moved.viewx = 10;
ok(patcher.geometryState(moved).state !== 'patched', 'a view area not at 0,0 is not ours');

/* ---- the app's copy against the patcher's, every even width ---- */
for (let w = 1088; w <= 1920; w += 2) {
    eq(app.geometryForWidth(w), r.geometryForWidth(w), `${w}: app = patcher`);
}
for (let w = 816; w <= 1440; w += 2) {
    ok(app.cropHeightForWidth(w) === r.cropHeightForWidth(w), `${w}: crop height`);
    ok(app.zoomPercent(w) === r.zoomPercent(w), `${w}: zoom`);
}

/* ---- the crop ---- */
eq([816, 1088, 1160, 1232, 1360, 1440].map(r.cropHeightForWidth), [360, 480, 512, 544, 600, 636], 'crop heights');
ok(r.validCrop({ x: 176, y: 0, w: 1088, h: 480 }), 'the baseline crop');
ok(!r.validCrop({ x: 354, y: 0, w: 1088, h: 480 }), 'outside the source');
ok(Number.isInteger(r.zoomPercent(816)) === false && r.zoomPercent(816) === 133.3, 'zoom one decimal');
eq(r.maxCropOffset(816), 312, 'the crop range');

console.log(`resolution: ${checks} checks pass`);
