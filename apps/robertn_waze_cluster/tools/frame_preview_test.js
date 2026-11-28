// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/* frame_preview.js: the full 1440x636 atlas becomes a 720x318 opaque PNG with
 * the atlas_rgb() colours; bad sizes and metadata are refused. */
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const zlib = require('zlib');
const { framePng, previewFrom, ATLAS_BYTES, OUT_W, OUT_H, FRAME_NAME, META_NAME } = require('../src/frame_preview.js');

function chunks(png) {
    const out = [];
    let at = 8;
    while (at < png.length) {
        const n = png.readUInt32BE(at);
        const type = png.toString('ascii', at + 4, at + 8);
        out.push([type, png.subarray(at + 8, at + 8 + n)]);
        at += 12 + n;
    }
    return out;
}

function pixels(png) {
    const cs = chunks(png);
    const ihdr = cs.find(([t]) => t === 'IHDR')[1];
    const raw = zlib.inflateSync(cs.filter(([t]) => t === 'IDAT').map(([, d]) => d).reduce((a, b) => Buffer.concat([a, b])));
    return { w: ihdr.readUInt32BE(0), h: ihdr.readUInt32BE(4), depth: ihdr[8], colour: ihdr[9], raw };
}

const gray = Buffer.alloc(ATLAS_BYTES, 128);
const meta = { width: 1440, height: 636, full_frame: true, size: ATLAS_BYTES };

const png = framePng(gray, meta);
assert.strictEqual(png.subarray(0, 8).toString('hex'), '89504e470d0a1a0a', 'PNG signature');
const p = pixels(png);
assert.deepStrictEqual([p.w, p.h, p.depth, p.colour], [OUT_W, OUT_H, 8, 6], '720x318 RGBA8');
assert.strictEqual(p.raw.length, (OUT_W * 4 + 1) * OUT_H, 'one filter byte a row');
assert.strictEqual(p.raw[0], 0, 'filter none');
assert.deepStrictEqual([...p.raw.subarray(1, 5)], [128, 128, 128, 255], 'neutral grey is opaque grey');

const white = Buffer.alloc(ATLAS_BYTES, 255);
const yBytes = 1440 * 636;
const cBytes = 720 * 318;
white.fill(128, yBytes, yBytes + 2 * cBytes);
const w = pixels(framePng(white, meta));
assert.deepStrictEqual([...w.raw.subarray(1, 5)], [255, 255, 255, 255], 'white stays white');

assert.throws(() => framePng(Buffer.alloc(ATLAS_BYTES - 1, 0), meta), /not 1373760/);
assert.throws(() => framePng(gray, Object.assign({}, meta, { full_frame: false })), /not a full/);
assert.throws(() => framePng(gray, Object.assign({}, meta, { width: 1440, height: 634 })), /not a full/);
assert.throws(() => framePng(gray, Object.assign({}, meta, { size: 1 })), /metadata says/);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-preview-'));
try {
    fs.writeFileSync(path.join(dir, FRAME_NAME), gray);
    fs.writeFileSync(path.join(dir, META_NAME), JSON.stringify(meta));
    const got = previewFrom(dir);
    assert.strictEqual(got.png.length, png.length, 'previewFrom reads the two files');
    assert.strictEqual(got.meta.full_frame, true);
} finally {
    fs.rmSync(dir, { recursive: true, force: true });
}
console.log('frame preview: 11 checks pass');
