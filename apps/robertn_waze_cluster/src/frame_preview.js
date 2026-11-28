// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* The full CarPlay source frame as a PNG the HMI can show.
 *
 * waze-on-cluster's frame dump writes the uncropped 1440x636 4:2:0 atlas and
 * its JSON beside it (frame_dump.h). This turns that atlas into an RGB PNG
 * at half size (720x318), using the same full-range BT.601 colours and
 * bilinear chroma sampling as the renderer's atlas_rgb() and the PC
 * converter (native/waze-on-cluster/tools/yuv_atlas_to_png.py), then averages
 * 2x2 blocks so one HMI label carries the whole picture without a 1.4 MB
 * texture. No image library: only zlib, as for the QR code.
 */
'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const SRC_W = 1440, SRC_H = 636;
const OUT_W = 720, OUT_H = 318;
const ATLAS_H = SRC_H + SRC_H / 2;
const ATLAS_BYTES = SRC_W * ATLAS_H;
const FRAME_NAME = 'carplay-frame-1440x636.yuva';
const META_NAME = 'carplay-frame-1440x636.json';

const CRC_TABLE = (() => {
    const t = [];
    for (let n = 0; n < 256; n += 1) {
        let c = n;
        for (let k = 0; k < 8; k += 1) {
            c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
        }
        t.push(c >>> 0);
    }
    return t;
})();

function crc32(buf) {
    let c = 0xffffffff;
    for (let i = 0; i < buf.length; i += 1) {
        c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
    }
    return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length, 0);
    const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body), 0);
    return Buffer.concat([len, body, crc]);
}

function clamp(v, lo, hi) {
    return Math.max(lo, Math.min(hi, v));
}

/* atlas_rgb()'s chroma sample: bilinear inside one part of the atlas,
 * coordinates clamped half a texel in from its edges. */
function chroma(atlas, i, ax, j, ay) {
    const i1 = i + 1 < SRC_W ? i + 1 : SRC_W - 1;
    const j1 = j + 1 < ATLAS_H ? j + 1 : ATLAS_H - 1;
    const top = (1 - ax) * atlas[j * SRC_W + i] + ax * atlas[j * SRC_W + i1];
    const bottom = (1 - ay) * atlas[j1 * SRC_W + i] + ay * atlas[j1 * SRC_W + i1];
    return ((1 - ay) * top + ay * bottom) / 255;
}

function level(v) {
    return Math.round(clamp(v, 0, 1) * 255);
}

/* The atlas as RGB, 1440x636, the converter's colours. */
function atlasRgb(atlas) {
    const half = SRC_W / 2;
    const cx = [];
    for (let x = 0; x < SRC_W; x += 1) {
        const s = clamp((x + 0.5) * 0.5 + 0.25, 0.5, half - 0.5);
        const xc = s - 0.5;
        const i = Math.floor(xc);
        cx.push([clamp(i, 0, half - 1), xc - i]);
    }
    const cy = [];
    for (let y = 0; y < SRC_H; y += 1) {
        const s = clamp((y + 0.5) * 0.5, 0.5, half - 0.5);
        const yc = s - 0.5;
        const j = Math.floor(yc);
        cy.push([clamp(j, 0, half - 1), yc - j]);
    }
    const rgb = Buffer.alloc(SRC_W * SRC_H * 3);
    for (let y = 0; y < SRC_H; y += 1) {
        const [cj, ay] = cy[y];
        const bj = SRC_H + cj;
        for (let x = 0; x < SRC_W; x += 1) {
            const yy = atlas[y * SRC_W + x] / 255;
            const [ci, ax] = cx[x];
            const cu = chroma(atlas, ci, ax, bj, ay) - 128 / 255;
            const cv = chroma(atlas, half + ci, ax, bj, ay) - 128 / 255;
            const o = (y * SRC_W + x) * 3;
            rgb[o] = level(yy + 1.402 * cv);
            rgb[o + 1] = level(yy - 0.344136 * cu - 0.714136 * cv);
            rgb[o + 2] = level(yy + 1.772 * cu);
        }
    }
    return rgb;
}

/* 2x2 block average, RGB8 -> RGBA8 (opaque). */
function toRgba(rgb) {
    const rgba = Buffer.alloc(OUT_W * OUT_H * 4);
    for (let y = 0; y < OUT_H; y += 1) {
        for (let x = 0; x < OUT_W; x += 1) {
            const o = (y * OUT_W + x) * 4;
            for (let c = 0; c < 3; c += 1) {
                const a = rgb[((2 * y) * SRC_W + 2 * x) * 3 + c];
                const b = rgb[((2 * y) * SRC_W + 2 * x + 1) * 3 + c];
                const d = rgb[((2 * y + 1) * SRC_W + 2 * x) * 3 + c];
                const e = rgb[((2 * y + 1) * SRC_W + 2 * x + 1) * 3 + c];
                rgba[o + c] = Math.round((a + b + d + e) / 4);
            }
            rgba[o + 3] = 255;
        }
    }
    return rgba;
}

function pngOf(rgba) {
    const stride = OUT_W * 4 + 1;
    const raw = Buffer.alloc(stride * OUT_H);
    for (let y = 0; y < OUT_H; y += 1) {
        raw[y * stride] = 0;
        rgba.copy(raw, y * stride + 1, y * OUT_W * 4, (y + 1) * OUT_W * 4);
    }
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(OUT_W, 0);
    ihdr.writeUInt32BE(OUT_H, 4);
    ihdr[8] = 8;
    ihdr[9] = 6;
    return Buffer.concat([
        Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
        chunk('IHDR', ihdr),
        chunk('IDAT', zlib.deflateSync(raw, { level: 6 })),
        chunk('IEND', Buffer.alloc(0))
    ]);
}

/* The PNG for an atlas and its metadata, or an Error saying what is wrong. */
function framePng(atlas, meta) {
    if (!Buffer.isBuffer(atlas) || atlas.length !== ATLAS_BYTES) {
        throw new Error(`the frame is ${atlas && atlas.length} bytes, not ${ATLAS_BYTES}`);
    }
    if (!meta || meta.width !== SRC_W || meta.height !== SRC_H || meta.full_frame !== true) {
        throw new Error('the frame metadata is not a full 1440x636 frame');
    }
    if (meta.size !== undefined && meta.size !== atlas.length) {
        throw new Error(`the metadata says ${meta.size} bytes, the frame is ${atlas.length}`);
    }
    return pngOf(toRgba(atlasRgb(atlas)));
}

/* Read the frame dump's two files from dir: { png, meta }. */
function previewFrom(dir) {
    const atlas = fs.readFileSync(path.join(dir, FRAME_NAME));
    const meta = JSON.parse(fs.readFileSync(path.join(dir, META_NAME), 'utf8'));
    return { png: framePng(atlas, meta), meta };
}

module.exports = { framePng, previewFrom, SRC_W, SRC_H, OUT_W, OUT_H, ATLAS_BYTES, FRAME_NAME, META_NAME };
