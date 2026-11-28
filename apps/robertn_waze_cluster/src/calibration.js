// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/**
 * The calibration geometry: an arbitrary advertised CarPlay width (1088..1920,
 * even), the height that keeps the 1088x480 shape, the physical size the
 * patcher advertises for it, and the cluster crop / zoom over the fixed
 * 1440x636 decoded source.
 *
 * The formulas are the patcher's (native/waze-on-cluster/patch/resolution.js,
 * which this app cannot require): the app cannot import from the native tree,
 * and the two are checked against each other by
 * native/waze-on-cluster/patch/test/resolution_test.js.
 *
 *   H = 2 * round(W * 15 / 68)        (480/1088 reduced; 0.5 rounds up)
 *   mmw = round(138 + (W - 1088) * 45 / 352)
 *   mmh = round(61 + (W - 1088) * 20 / 352)
 *   zoom = 1088 / crop_w * 100
 *
 * Pure functions: no OAP runtime, no I/O.
 */
'use strict';

const MIN_WIDTH = 1088;
const MAX_WIDTH = 1920;
const WIDTH_STEP = 2;
const CROP_MIN_WIDTH = 816;
const CROP_MAX_WIDTH = 1440;
const SOURCE_WIDTH = 1440;
const SOURCE_HEIGHT = 636;
const MAP_WIDTH = 1088;
const MAP_HEIGHT = 480;

function validWidth(w) {
    return typeof w === 'number' && isFinite(w) && w === Math.floor(w) && w % WIDTH_STEP === 0 && w >= MIN_WIDTH && w <= MAX_WIDTH;
}

function validCropWidth(w) {
    return typeof w === 'number' && isFinite(w) && w === Math.floor(w) && w % WIDTH_STEP === 0 && w >= CROP_MIN_WIDTH && w <= CROP_MAX_WIDTH;
}

function heightForWidth(w) {
    return 2 * Math.round(w * 15 / 68);
}

function mmForWidth(w) {
    return { w: Math.round(138 + (w - 1088) * 45 / 352), h: Math.round(61 + (w - 1088) * 20 / 352) };
}

function safeFor(w, h) {
    const x0 = Math.round(286 * w / 1088), x1 = Math.round(790 * w / 1088);
    const y0 = Math.round(72 * h / 480), y1 = Math.round(440 * h / 480);
    return { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
}

function geometryForWidth(w) {
    if (!validWidth(w)) {
        return null;
    }
    const h = heightForWidth(w), mm = mmForWidth(w), s = safeFor(w, h);
    return { w, h, mmw: mm.w, mmh: mm.h, safex: s.x, safey: s.y, safew: s.w, safeh: s.h };
}

function resText(w, h) {
    return `${w}x${h}`;
}

function parseRes(text) {
    const m = /^(\d+)x(\d+)$/.exec(String(text || ''));
    if (!m) {
        return null;
    }
    const w = Number(m[1]), g = geometryForWidth(w);
    return g && g.h === Number(m[2]) ? g : null;
}

function cropHeightForWidth(w) {
    return validCropWidth(w) ? 2 * Math.round(w * 15 / 68) : 0;
}

function cropForWidth(w) {
    return validCropWidth(w) ? { w, h: cropHeightForWidth(w) } : null;
}

function zoomPercent(cropW) {
    return cropW ? Math.round(1088 * 1000 / cropW) / 10 : 0;
}

function maxCropOffset(cropW) {
    return (SOURCE_WIDTH - (validCropWidth(cropW) ? cropW : MAP_WIDTH)) / 2;
}

function cropXForOffset(cropW, offset) {
    const max = maxCropOffset(cropW);
    return (SOURCE_WIDTH - cropW) / 2 - Math.max(-max, Math.min(max, offset));
}

function offsetForCropX(cropW, x) {
    const max = maxCropOffset(cropW);
    return Math.max(-max, Math.min(max, (SOURCE_WIDTH - cropW) / 2 - x));
}

function validCrop(rect) {
    if (!rect || !validCropWidth(rect.w)) {
        return false;
    }
    const h = cropHeightForWidth(rect.w);
    return rect.h === h && rect.x >= 0 && rect.y >= 0 && rect.x + rect.w <= SOURCE_WIDTH &&
        rect.y + rect.h <= SOURCE_HEIGHT && rect.x % 2 === 0 && rect.y % 2 === 0;
}

module.exports = {
    MIN_WIDTH, MAX_WIDTH, WIDTH_STEP, CROP_MIN_WIDTH, CROP_MAX_WIDTH,
    SOURCE_WIDTH, SOURCE_HEIGHT, MAP_WIDTH, MAP_HEIGHT,
    validWidth, validCropWidth, heightForWidth, mmForWidth, safeFor, geometryForWidth,
    resText, parseRes, cropHeightForWidth, cropForWidth, zoomPercent,
    maxCropOffset, cropXForOffset, offsetForCropX, validCrop
};
