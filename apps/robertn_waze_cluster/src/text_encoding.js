// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/**
 * Diagnostic text that arrived as bytes, or as characters that were decoded
 * with the wrong code page, made readable again.
 *
 * The journal's Russian lines on the Logs page came from locale data that had
 * been UTF-8 and was decoded as Windows-1251 (once, and in places twice):
 * "Запись" -> "Р—Р°РїРёСЃСЊ". This module reverses that, and reads bytes that
 * are not valid UTF-8 as Windows-1251 (the head unit's Russian journal code
 * page), while never touching text that is already correct:
 *
 *   repairMojibake(s)   UTF-8 text whose bytes were decoded as CP1251, up to
 *                       two levels deep; anything else (valid Cyrillic, Latin,
 *                       CJK, ASCII, numbers, paths) is returned unchanged.
 *   decodeBytes(buf)    a Buffer: strict UTF-8 when valid, else CP1251.
 *   normalizeDiagnostic(s)  repairMojibake, or the string as it is.
 *
 * ASCII is the same in both code pages, so numbers, timestamps and file paths
 * are untouched. A string that cannot be encoded to CP1251 (CJK, Greek, ...)
 * cannot be CP1251 mojibake and is left alone.
 */
'use strict';

/* Windows-1251's 0x80..0xFF half, in byte order. 0x98 is undefined and some
 * decoders give the C1 control for it; it is kept so that a byte decoded that
 * way can still be put back. */
const CP1251_HIGH =
    '\u0402\u0403\u201a\u0453\u201e\u2026\u2020\u2021\u20ac\u2030\u0409\u2039\u040a\u040c\u040b\u040f' +
    '\u0452\u2018\u2019\u201c\u201d\u2022\u2013\u2014\u0098\u2122\u0459\u203a\u045a\u045c\u045b\u045f' +
    '\u00a0\u040e\u045e\u0408\u00a4\u0490\u00a6\u00a7\u0401\u00a9\u0404\u00ab\u00ac\u00ad\u00ae\u0407' +
    '\u00b0\u00b1\u0406\u0456\u0491\u00b5\u00b6\u00b7\u0451\u2116\u0454\u00bb\u0458\u0405\u0455\u0457' +
    '\u0410\u0411\u0412\u0413\u0414\u0415\u0416\u0417\u0418\u0419\u041a\u041b\u041c\u041d\u041e\u041f' +
    '\u0420\u0421\u0422\u0423\u0424\u0425\u0426\u0427\u0428\u0429\u042a\u042b\u042c\u042d\u042e\u042f' +
    '\u0430\u0431\u0432\u0433\u0434\u0435\u0436\u0437\u0438\u0439\u043a\u043b\u043c\u043d\u043e\u043f' +
    '\u0440\u0441\u0442\u0443\u0444\u0445\u0446\u0447\u0448\u0449\u044a\u044b\u044c\u044d\u044e\u044f';

const BYTE_OF = Object.create(null);
for (let i = 0; i < CP1251_HIGH.length; i++) {
    BYTE_OF[CP1251_HIGH[i]] = 0x80 + i;
}

/* The characters a CP1251-decoded UTF-8 byte stream leaves behind: the
 * 0x80..0xBF half (punctuation, NBSP, Ukrainian/Belarusian letters) and the
 * capital Р/С that start most Cyrillic pairs. Used only to prefer the
 * repaired form, never to change a string that did not survive the round
 * trip. */
const MARKERS = new Set(CP1251_HIGH.slice(0, 64).split(''));
MARKERS.add('\u0420');
MARKERS.add('\u0421');

function markerScore(s) {
    let n = 0;
    for (const c of s) {
        if (MARKERS.has(c)) {
            n += 1;
        }
    }
    return n;
}

/* A string as Windows-1251 bytes, or null when a character has no CP1251
 * byte (then it is not CP1251 mojibake). */
function cp1251Bytes(s) {
    const out = Buffer.alloc(s.length);
    for (let i = 0; i < s.length; i++) {
        const code = s.charCodeAt(i);
        if (code < 0x80) {
            out[i] = code;
        } else {
            const b = BYTE_OF[s[i]];
            if (b === undefined) {
                return null;
            }
            out[i] = b;
        }
    }
    return out;
}

/* The string a byte sequence is in UTF-8, or null when it is not valid
 * UTF-8 (Node's toString would silently replace the bad bytes). */
function strictUtf8(bytes) {
    let i = 0;
    while (i < bytes.length) {
        const b = bytes[i];
        let n;
        if (b < 0x80) {
            n = 1;
        } else if (b >= 0xc2 && b <= 0xdf) {
            n = 2;
        } else if (b >= 0xe0 && b <= 0xef) {
            n = 3;
        } else if (b >= 0xf0 && b <= 0xf4) {
            n = 4;
        } else {
            return null;
        }
        if (i + n > bytes.length) {
            return null;
        }
        for (let k = 1; k < n; k++) {
            if ((bytes[i + k] & 0xc0) !== 0x80) {
                return null;
            }
        }
        /* overlong, surrogate and out-of-range forms */
        if (n === 3) {
            const c = ((b & 0x0f) << 12) | ((bytes[i + 1] & 0x3f) << 6) | (bytes[i + 2] & 0x3f);
            if (c < 0x800 || (c >= 0xd800 && c <= 0xdfff)) {
                return null;
            }
        } else if (n === 4) {
            const c = ((b & 0x07) << 18) | ((bytes[i + 1] & 0x3f) << 12) | ((bytes[i + 2] & 0x3f) << 6) |
                (bytes[i + 3] & 0x3f);
            if (c < 0x10000 || c > 0x10ffff) {
                return null;
            }
        }
        i += n;
    }
    return Buffer.from(bytes).toString('utf8');
}

/* UTF-8 text whose bytes were decoded as Windows-1251, one or two levels
 * deep. A string that is already correct, or cannot be CP1251, is returned
 * unchanged. */
function repairMojibake(s) {
    if (typeof s !== 'string') {
        return s;
    }
    let cur = s;
    for (let level = 0; level < 3; level++) {
        const bytes = cp1251Bytes(cur);
        if (!bytes) {
            break;
        }
        const next = strictUtf8(bytes);
        if (next === null || next === cur) {
            break;
        }
        /* Only a form with fewer mojibake markers than this one; a genuine
         * string that happens to round-trip is left alone. */
        if (markerScore(next) >= markerScore(cur)) {
            break;
        }
        cur = next;
    }
    return cur;
}

/* Windows-1251 bytes as text: every byte 0x80..0xFF is its CP1251
 * character, ASCII is ASCII. */
function decodeCp1251(bytes) {
    let out = '';
    for (let i = 0; i < bytes.length; i++) {
        const b = bytes[i];
        out += b < 0x80 ? String.fromCharCode(b) : CP1251_HIGH[b - 0x80];
    }
    return out;
}

/* A diagnostic line's bytes: strict UTF-8 when valid, else Windows-1251
 * (with the mojibake repair in case what arrived was UTF-8 bytes read as
 * CP1251 upstream). */
function decodeBytes(bytes) {
    const buf = Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes || []);
    const utf8 = strictUtf8(buf);
    return repairMojibake(utf8 !== null ? utf8 : decodeCp1251(buf));
}

/* A diagnostic line that may already be a string. */
function normalizeDiagnostic(s) {
    return typeof s === 'string' ? repairMojibake(s) : decodeBytes(s);
}

module.exports = { repairMojibake, decodeBytes, decodeCp1251, strictUtf8, normalizeDiagnostic, markerScore, CP1251_HIGH };
