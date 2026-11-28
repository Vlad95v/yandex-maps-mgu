// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/**
 * The calibration geometry (src/calibration.js): the width range, the height
 * that keeps the 1088x480 shape, the physical size the patcher advertises,
 * the safe area, and the cluster crop / zoom. It also checks the app's copy of
 * the formulas against the patcher's (native/waze-on-cluster/patch/
 * resolution.js), every even width in each range.
 *
 *   node tools/test_calibration.js
 */
const assert = require('assert');
const path = require('path');
const cal = require('../src/calibration.js');

let checks = 0;
const ok = (cond, msg) => { checks += 1; assert(cond, msg); };
const eq = (got, want, msg) => { checks += 1; assert.deepStrictEqual(got, want, msg); };

/* ---- the width range ---- */
[1088, 1100, 1120, 1200, 1300, 1320, 1400, 1440, 1500, 1600, 1800, 1920].forEach((w) =>
    ok(cal.validWidth(w), `${w} is a calibration width`));
[1086, 1078, 1101, 1922, 2000, 0, -1088, 1200.5, NaN, '1200', null, undefined].forEach((w) =>
    ok(!cal.validWidth(w), `${String(w)} is not a calibration width`));
ok(cal.validWidth(cal.MIN_WIDTH) && cal.validWidth(cal.MAX_WIDTH), 'the ends are inside');
ok(!cal.validWidth(cal.MIN_WIDTH - 2) && !cal.validWidth(cal.MAX_WIDTH + 2), 'and just outside is not');

/* ---- the height: the 1088x480 shape, the expected examples ---- */
eq([1088, 1100, 1200, 1300, 1400, 1440, 1500, 1600, 1800, 1920].map(cal.heightForWidth),
    [480, 486, 530, 574, 618, 636, 662, 706, 794, 848], 'the height examples');
ok(cal.heightForWidth(1088) === 480 && cal.heightForWidth(1440) === 636, 'both known configurations exact');
for (let w = 1088; w <= 1920; w += 2) {
    ok(cal.heightForWidth(w) % 2 === 0, `${w}: an even height`);
}

/* ---- the physical size: linear between 138x61 and 183x81, on above ---- */
eq([1088, 1200, 1300, 1320, 1400, 1440, 1600, 1920].map((w) => {
    const m = cal.mmForWidth(w);
    return [m.w, m.h];
}), [[138, 61], [152, 67], [165, 73], [168, 74], [178, 79], [183, 81], [203, 90], [244, 108]], 'the physical examples');
ok(cal.mmForWidth(1088).w === 138 && cal.mmForWidth(1088).h === 61, '1088 is 138x61 mm');
ok(cal.mmForWidth(1440).w === 183 && cal.mmForWidth(1440).h === 81, '1440 is 183x81 mm');
ok(cal.mmForWidth(1920).w > cal.mmForWidth(1800).w, 'above 1440 the line continues');

/* ---- the safe area: 504x368+286+72 at 1088, scaled ---- */
eq(cal.safeFor(1088, 480), { x: 286, y: 72, w: 504, h: 368 }, 'the 1088 safe area');
eq(cal.safeFor(1440, 636), { x: 379, y: 95, w: 667, h: 488 }, 'the wide safe area');
ok(cal.geometryForWidth(1440).safew === 667 && cal.geometryForWidth(1440).safeh === 488, 'geometry carries it');

/* ---- parseRes, resText ---- */
eq(cal.parseRes('1200x530'), Object.assign({}, cal.geometryForWidth(1200)), 'a computed height parses');
ok(cal.parseRes('1200x531') === null && cal.parseRes('1201x530') === null && cal.parseRes('1440') === null &&
    cal.parseRes('1440x636x1') === null, 'others do not');
eq(cal.resText(1440, 636), '1440x636', 'the text');

/* ---- the crop ---- */
ok(cal.validCropWidth(816) && cal.validCropWidth(1440) && cal.validCropWidth(1088), 'the crop ends and baseline');
[814, 1442, 817, 0, NaN].forEach((w) => ok(!cal.validCropWidth(w), `${String(w)} is not a crop width`));
eq([816, 1088, 1160, 1232, 1360, 1440].map(cal.cropHeightForWidth), [360, 480, 512, 544, 600, 636], 'the crop heights');
ok(cal.validCrop({ x: 176, y: 0, w: 1088, h: 480 }), 'the baseline crop is inside');
ok(!cal.validCrop({ x: 353, y: 0, w: 1088, h: 480 }) && !cal.validCrop({ x: 176, y: 157, w: 1088, h: 480 }),
    'outside is not');
ok(!cal.validCrop({ x: 176, y: 0, w: 1088, h: 478 }) && !cal.validCrop({ x: 177, y: 0, w: 1088, h: 480 }),
    'another shape or odd x is not');

/* ---- zoom and the offset from the centre ---- */
ok(cal.zoomPercent(1088) === 100 && cal.zoomPercent(816) === 133.3 && cal.zoomPercent(1440) === 75.6, 'the zoom');
ok(cal.zoomPercent(0) === 0, 'no crop: no zoom');
eq(cal.maxCropOffset(1088), 176, 'the 1088 crop moves +-176');
eq(cal.maxCropOffset(816), 312, 'a narrower crop moves further');
eq(cal.maxCropOffset(1440), 0, 'the whole source moves nowhere');
eq(cal.cropXForOffset(1088, 0), 176, 'centred: 176');
eq(cal.cropXForOffset(1088, 176), 0, 'right: 0');
eq(cal.cropXForOffset(1088, -176), 352, 'left: 352');
eq(cal.offsetForCropX(1088, 0), 176, 'and back');
eq(cal.offsetForCropX(816, 312), 0, 'the 816 crop centred');
eq(cal.offsetForCropX(816, 0), 312, 'clamped to its range');

/* ---- the app's copy against the patcher's ---- */
const native = require(path.join(__dirname, '..', '..', '..', 'native', 'waze-on-cluster', 'patch', 'resolution.js'));
for (let w = 1088; w <= 1920; w += 2) {
    const a = cal.geometryForWidth(w), b = native.geometryForWidth(w);
    ok(JSON.stringify(a) === JSON.stringify(b), `${w}: the app and the patcher agree`);
}
for (let w = 816; w <= 1440; w += 2) {
    ok(cal.cropHeightForWidth(w) === native.cropHeightForWidth(w), `${w}: the crop height agrees`);
    ok(cal.zoomPercent(w) === native.zoomPercent(w), `${w}: the zoom agrees`);
}

console.log(`calibration: ${checks} checks pass`);
