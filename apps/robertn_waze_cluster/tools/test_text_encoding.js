// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/**
 * Diagnostic text encoding (src/text_encoding.js): the Russian journal lines
 * that were UTF-8 decoded as Windows-1251 made readable again, while correct
 * UTF-8, English, ASCII, numbers, timestamps and paths stay as they are. Also
 * checks that every locale's words are already correct (no mojibake), so the
 * on-screen fix cannot silently come back.
 *
 *   node tools/test_text_encoding.js
 */
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const enc = require('../src/text_encoding.js');

let checks = 0;
const eq = (got, want, msg) => { checks += 1; assert.deepStrictEqual(got, want, msg); };

/* ---- correct text is untouched ---- */
['Привет, мир!', 'Запись', 'Кадр CarPlay: 1440x636', 'Статус: Применено', 'Ёлка, № 5', 'Maps on cluster',
    'Status: Patched', '1234567890', '2026-11-27T18:29:43Z', 'C:\\Users\\varan\\map.png', '/var/opt/waze-on-cluster/state',
    'CarPlay э 100 %', '日本語のテキスト', 'مرحبا بالعالم', 'Καλημέρα', 'עברית']
    .forEach((s) => eq(enc.repairMojibake(s), s, `unchanged: ${s}`));

/* ---- known mojibake, one level (the screenshot's rows) ---- */
eq(enc.repairMojibake('Р—Р°РїРёСЃСЊ'), 'Запись', 'once: Запись');
eq(enc.repairMojibake('РљР°РґСЂ CarPlay: {0}'), 'Кадр CarPlay: {0}', 'once: Кадр CarPlay');
eq(enc.repairMojibake('РћР¶РёРґР°РЅРёРµ РєР°РґСЂР° CarPlay'), 'Ожидание кадра CarPlay', 'waiting');
eq(enc.repairMojibake('Р—Р°С…РІР°С‚РёС‚СЊ РїРѕР»РЅС‹Р№ РєР°РґСЂ CarPlay'),
    'Захватить полный кадр CarPlay', 'the full frame button');
eq(enc.repairMojibake('РљР°РґСЂ РЅРµ РѕР±РµР·Р»РёС‡РёРІР°РµС‚СЃСЏ: РІ Р°СЂС…РёРІРµ РјРѕРіСѓС‚ Р±С‹С‚СЊ РєР°СЂС‚Р°, Р°РґСЂРµСЃР°, РІР°С€Рµ РјРµСЃС‚РѕРїРѕР»РѕР¶РµРЅРёРµ Рё РјР°СЂС€СЂСѓС‚.'),
    'Кадр не обезличивается: в архиве могут быть карта, адреса, ваше местоположение и маршрут.', 'the privacy note');

/* ---- twice-decoded text, and non-Cyrillic ---- */
eq(enc.repairMojibake('Р“РЃ'), 'è', 'twice: è');
eq(enc.repairMojibake('Р“В©'), 'é', 'twice: é');

/* ---- mixed diagnostic lines: only the mojibake changes ---- */
eq(enc.repairMojibake('Status: Р—Р°РїРёСЃСЊ / OK 12:34:56 C:\\tmp\\a.png'),
    'Status: Запись / OK 12:34:56 C:\\tmp\\a.png', 'mixed Russian and data');
eq(enc.repairMojibake('[2026-11-27T18:29:43Z] РћР¶РёРґР°РЅРёРµ РєР°РґСЂР° CarPlay 1440x636'),
    '[2026-11-27T18:29:43Z] Ожидание кадра CarPlay 1440x636', 'a timestamped mixed line');
eq(enc.repairMojibake('DLT Р—Р°РїРёСЃСЊ: 5 Р·Р°РїРёСЃРµР№'),
    'DLT Запись: 5 записей', 'a DLT prefix and numbers');

/* ---- bytes: strict UTF-8, else Windows-1251 ---- */
eq(enc.decodeBytes(Buffer.from('Привет, мир', 'utf8')), 'Привет, мир', 'valid UTF-8 bytes');
eq(enc.decodeBytes(Buffer.from([0xcf, 0xf0, 0xe8, 0xe2, 0xe5, 0xf2, 0x2c, 0x20, 0xec, 0xe8, 0xf0])), 'Привет, мир',
    'CP1251 bytes');
eq(enc.decodeBytes(Buffer.from([0x52, 0x65, 0x61, 0x64, 0x79])), 'Ready', 'ASCII bytes');
eq(enc.decodeBytes(Buffer.alloc(0)), '', 'no bytes');
/* a valid UTF-8 byte string that is already mojibake is repaired */
eq(enc.decodeBytes(Buffer.from('Р—Р°РїРёСЃСЊ', 'utf8')), 'Запись', 'mojibake arriving as UTF-8');
/* U+FFFD in the input is kept: the information was lost before it got here */
eq(enc.decodeBytes(Buffer.from('ab\uFFFDcd', 'utf8')), 'ab\uFFFDcd', 'a replacement character is kept');

/* ---- strict UTF-8: invalid forms are not taken for text ---- */
eq(enc.strictUtf8(Buffer.from([0x80])), null, 'a lone continuation byte');
eq(enc.strictUtf8(Buffer.from([0xc0, 0xaf])), null, 'overlong');
eq(enc.strictUtf8(Buffer.from([0xed, 0xa0, 0x80])), null, 'a surrogate');
eq(enc.strictUtf8(Buffer.from([0xf4, 0x90, 0x80, 0x80])), null, 'out of range');
eq(enc.strictUtf8(Buffer.from([0xe2, 0x82])), null, 'cut short');
eq(enc.strictUtf8(Buffer.from('€', 'utf8')), '€', 'a valid three-byte character');

/* ---- the locales themselves: no mojibake left ---- */
const DIR = path.join(__dirname, '..', 'locales');
let words = 0;
fs.readdirSync(DIR).filter((n) => /\.json$/.test(n)).forEach((n) => {
    const j = JSON.parse(fs.readFileSync(path.join(DIR, n), 'utf8'));
    Object.keys(j.words).forEach((k) => {
        words += 1;
        eq(enc.repairMojibake(j.words[k]), j.words[k], `${n}: "${k}" is not mojibake`);
        assert(!j.words[k].includes('\uFFFD'), `${n}: "${k}" has no replacement character`);
    });
});
const ru = JSON.parse(fs.readFileSync(path.join(DIR, 'ru.json'), 'utf8'));
eq(ru.words['Capture full CarPlay frame'], 'Захватить полный кадр CarPlay', 'the Russian button');
eq(ru.words['Waiting for CarPlay frame'], 'Ожидание кадра CarPlay', 'the Russian waiting line');
eq(ru.words['The frame is not scrubbed: the archive may contain the map, addresses, your location and route.'],
    'Кадр не обезличивается: в архиве могут быть карта, адреса, ваше местоположение и маршрут.', 'the Russian privacy note');

console.log(`text encoding: ${words} locale words, ${checks} checks pass`);
