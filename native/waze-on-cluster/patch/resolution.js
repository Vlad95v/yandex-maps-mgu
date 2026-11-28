// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* The CarPlay cluster geometry, for the calibration mode: an arbitrary
 * advertised width in 1088..1920 px (even), the height that keeps the
 * 1088x480 shape, and the physical size the patcher advertises for it.
 *
 *   width W -> height H = round(W * 480 / 1088), to the nearest even value
 *              (0.5 rounds up, so 1440 -> 636)
 *   W -> mm  = 138 + (W - 1088) * 45 / 352, rounded
 *       mm  =  61 + (W - 1088) * 20 / 352, rounded
 *              (the two known sizes: 1088x480 at 138x61 mm and 1440x636 at
 *              183x81 mm; above 1440 the same line continues)
 *   safe area = the default 504x368+286+72 scaled to W x H
 *
 * The same functions, copied for the app (apps/robertn_waze_cluster/src/
 * calibration.js, which cannot require this tree), are checked against these
 * by patch/test/resolution_test.js. Node 8 on the unit: no dependencies.
 */
'use strict';

var MIN_WIDTH = 1088;
var MAX_WIDTH = 1920;
var WIDTH_STEP = 2;
var CROP_MIN_WIDTH = 816;
var CROP_MAX_WIDTH = 1440;
var REF_WIDTH = 1088;
var REF_HEIGHT = 480;
/* The decoded atlas the crop is taken from (waze-on-cluster.conf tex_size). */
var SOURCE_WIDTH = 1440;
var SOURCE_HEIGHT = 636;
var MAP_WIDTH = 1088;
var MAP_HEIGHT = 480;

/* A whole, even width in the calibration range (1088..1920). */
function validWidth(w) {
    return typeof w === 'number' && isFinite(w) && w === Math.floor(w) && w % WIDTH_STEP === 0 &&
        w >= MIN_WIDTH && w <= MAX_WIDTH;
}

/* A whole, even width in the crop range (816..1440). */
function validCropWidth(w) {
    return typeof w === 'number' && isFinite(w) && w === Math.floor(w) && w % WIDTH_STEP === 0 &&
        w >= CROP_MIN_WIDTH && w <= CROP_MAX_WIDTH;
}

/* The even height that keeps the 1088x480 shape. */
function heightForWidth(w) {
    return 2 * Math.round(w * 15 / 68);
}

/* The physical size the patcher advertises for width w: linear between
 * 1088x480 at 138x61 mm and 1440x636 at 183x81 mm, and on the same line
 * above 1440. */
function mmForWidth(w) {
    return {
        w: Math.round(138 + (w - 1088) * 45 / 352),
        h: Math.round(61 + (w - 1088) * 20 / 352),
    };
}

/* The default safe area for w x h: 504x368 at 286,72 scaled from 1088x480. */
function safeFor(w, h) {
    var x0 = Math.round(286 * w / REF_WIDTH), x1 = Math.round(790 * w / REF_WIDTH);
    var y0 = Math.round(72 * h / REF_HEIGHT), y1 = Math.round(440 * h / REF_HEIGHT);
    return { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
}

/* The full geometry a width advertises: { w, h, mmw, mmh, safex, safey,
 * safew, safeh }, or null when the width is not a calibration width. */
function geometryForWidth(w) {
    if (!validWidth(w))
        return null;
    var h = heightForWidth(w), mm = mmForWidth(w), s = safeFor(w, h);
    return { w: w, h: h, mmw: mm.w, mmh: mm.h, safex: s.x, safey: s.y, safew: s.w, safeh: s.h };
}

/* A "WxH" the calibration offers (the height exactly the one for its width),
 * as geometryForWidth, or null. */
function parseRes(text) {
    var m = /^([0-9]+)x([0-9]+)$/.exec(String(text || ''));
    if (!m)
        return null;
    var w = parseInt(m[1], 10), h = parseInt(m[2], 10), g = geometryForWidth(w);
    return g && g.h === h ? g : null;
}

function resText(g) {
    return g.w + 'x' + g.h;
}

/* The height of a cluster crop of width w (the 1088x480 shape kept), or 0. */
function cropHeightForWidth(w) {
    return validCropWidth(w) ? 2 * Math.round(w * 15 / 68) : 0;
}

/* The cluster crop a width asks for: { w, h }, h to the 1088x480 shape, or
 * null when the width is outside 816..1440. */
function cropForWidth(w) {
    if (!validCropWidth(w))
        return null;
    return { w: w, h: cropHeightForWidth(w) };
}

/* 1088 / crop_w * 100, one decimal: 1088 -> 100, 816 -> 133.3. */
function zoomPercent(cropW) {
    if (!cropW)
        return 0;
    return Math.round(1088 * 1000 / cropW) / 10;
}

/* The most the crop can be moved horizontally from the centre: the source's
 * width less the crop's, halved. */
function maxCropOffset(cropW) {
    return (SOURCE_WIDTH - (validCropWidth(cropW) ? cropW : MAP_WIDTH)) / 2;
}

/* crop_x for a horizontal offset from the centre (offset 0: the centred
 * crop, 176 at 1088 wide). */
function cropXForOffset(cropW, offset) {
    var max = maxCropOffset(cropW);
    var off = Math.max(-max, Math.min(max, offset));
    return (SOURCE_WIDTH - cropW) / 2 - off;
}

/* The offset from the centre a crop_x is at. */
function offsetForCropX(cropW, x) {
    var max = maxCropOffset(cropW);
    var off = (SOURCE_WIDTH - cropW) / 2 - x;
    return Math.max(-max, Math.min(max, off));
}

/* Whether a crop rectangle is inside the 1440x636 source, even, and has the
 * 1088x480 shape (its height the one for its width). */
function validCrop(rect) {
    if (!rect || !validCropWidth(rect.w))
        return false;
    var h = cropHeightForWidth(rect.w);
    return rect.h === h && rect.x >= 0 && rect.y >= 0 && rect.x + rect.w <= SOURCE_WIDTH &&
        rect.y + rect.h <= SOURCE_HEIGHT && rect.x % 2 === 0 && rect.y % 2 === 0;
}

module.exports = {
    MIN_WIDTH: MIN_WIDTH, MAX_WIDTH: MAX_WIDTH, WIDTH_STEP: WIDTH_STEP,
    CROP_MIN_WIDTH: CROP_MIN_WIDTH, CROP_MAX_WIDTH: CROP_MAX_WIDTH,
    SOURCE_WIDTH: SOURCE_WIDTH, SOURCE_HEIGHT: SOURCE_HEIGHT,
    MAP_WIDTH: MAP_WIDTH, MAP_HEIGHT: MAP_HEIGHT,
    REF_WIDTH: REF_WIDTH, REF_HEIGHT: REF_HEIGHT,
    validWidth: validWidth, validCropWidth: validCropWidth,
    heightForWidth: heightForWidth, mmForWidth: mmForWidth, safeFor: safeFor,
    geometryForWidth: geometryForWidth, parseRes: parseRes, resText: resText,
    cropHeightForWidth: cropHeightForWidth, cropForWidth: cropForWidth, zoomPercent: zoomPercent,
    maxCropOffset: maxCropOffset, cropXForOffset: cropXForOffset, offsetForCropX: offsetForCropX,
    validCrop: validCrop,
};
