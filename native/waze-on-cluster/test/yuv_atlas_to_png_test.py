#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Robert Nisipeanu
"""tools/yuv_atlas_to_png.py: the full CarPlay frame of a woc-logs archive as
a PNG. A 1440x636 4:2:0 atlas is 1,373,760 bytes; the colours are
atlas_rgb()'s (full-range BT.601, chroma upsampled inside its own part, the
edges clamped); a frame of another size, a missing or unsafe member, a
corrupt archive or metadata that does not match are refused with a message.
No dependencies beyond Python 3's standard library."""

import importlib.util
import io
import json
import os
import struct
import sys
import tarfile
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, '..', 'tools', 'yuv_atlas_to_png.py')

spec = importlib.util.spec_from_file_location('yuv_atlas_to_png', TOOL)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

checks = 0
failures = 0


def ok(name, cond):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        sys.stdout.write('FAIL %s\n' % name)


def eq(name, got, want):
    ok('%s (%r)' % (name, got), got == want)


def raises(name, fn, needle):
    try:
        fn()
    except tool.Error as e:
        ok('%s: %s' % (name, e), needle in str(e))
        return
    ok('%s: no error, wanted %r' % (name, needle), False)


def frame(y, u, v, w=tool.WIDTH, h=tool.HEIGHT):
    """A uniform 4:2:0 atlas: y plane, then U and V side by side."""
    th = tool.atlas_height(h)
    a = bytearray(w * th)
    for i in range(w * h):
        a[i] = y
    for j in range(h // 2):
        row = h * w + j * w
        for i in range(w // 2):
            a[row + i] = u
            a[row + w // 2 + i] = v
    return bytes(a)


def metadata(w=tool.WIDTH, h=tool.HEIGHT, size=None, full=True):
    return json.dumps({'width': w, 'height': h, 'format': 'yuv420p_atlas', 'atlas_height': tool.atlas_height(h),
                       'size': tool.atlas_bytes(w, h) if size is None else size, 'captured_utc': '2026-11-27T00:00:00Z',
                       'source': 'carplay', 'full_frame': full}).encode('utf-8')


def archive(path, atlas, meta, member='carplay-frame-1440x636.yuva', names=None):
    """A woc-logs-shaped archive holding the frame and its metadata."""
    names = names or {}
    with tarfile.open(path, 'w:gz') as t:
        for data, name in ((atlas, member), (meta, 'carplay-frame-1440x636.json')):
            if data is None:
                continue
            name = names.get(name, name)
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o600
            t.addfile(info, io.BytesIO(data))
    return path


def png_size(path):
    """(width, height) from the PNG's IHDR, and its pixels as rows of RGB."""
    data = open(path, 'rb').read()
    assert data.startswith(b'\x89PNG\r\n\x1a\n'), 'not a PNG'
    pos, chunks = 8, {}
    while pos < len(data):
        length = struct.unpack('>I', data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        chunks[kind] = data[pos + 8:pos + 8 + length]
        pos += 12 + length
    w, h = struct.unpack('>II', chunks[b'IHDR'][:8])
    raw = zlib.decompress(chunks[b'IDAT'])
    rows, stride = [], 1 + w * 3
    for j in range(h):
        row = raw[j * stride:(j + 1) * stride]
        assert row[0] == 0, 'filter %r' % row[0]
        rows.append(row[1:])
    return w, h, rows


tmp = tempfile.mkdtemp(prefix='woc-yuv-')
try:
    eq('the atlas is 1,373,760 bytes', tool.atlas_bytes(), 1373760)
    eq('its height is the frame plus half', tool.atlas_height(636), 954)

    # A neutral grey frame: Y 128, U and V 128 is exactly grey.
    a = archive(os.path.join(tmp, 'grey.tar.gz'), frame(128, 128, 128), metadata())
    out = os.path.join(tmp, 'grey.png')
    info, png = tool.convert(a, out)
    w, h, rows = png_size(out)
    eq('the PNG is 1440x636', (w, h), (1440, 636))
    eq('the metadata came back', info['source'], 'carplay')
    eq('the middle is grey', tuple(rows[318][720 * 3:720 * 3 + 3]), (128, 128, 128))
    eq('the top left is grey', tuple(rows[0][0:3]), (128, 128, 128))
    eq('the bottom right is grey', tuple(rows[635][1439 * 3:1439 * 3 + 3]), (128, 128, 128))

    # White and black: the ends of the full range.
    tool.convert(archive(os.path.join(tmp, 'white.tar.gz'), frame(255, 128, 128), metadata()), os.path.join(tmp, 'w.png'))
    eq('Y 255 is white', tuple(png_size(os.path.join(tmp, 'w.png'))[2][0][0:3]), (255, 255, 255))
    tool.convert(archive(os.path.join(tmp, 'black.tar.gz'), frame(0, 128, 128), metadata()), os.path.join(tmp, 'b.png'))
    eq('Y 0 is black', tuple(png_size(os.path.join(tmp, 'b.png'))[2][0][0:3]), (0, 0, 0))

    # Full-range BT.601 red: R high, G and B low.
    tool.convert(archive(os.path.join(tmp, 'red.tar.gz'), frame(76, 85, 255), metadata()), os.path.join(tmp, 'r.png'))
    r, g, b = png_size(os.path.join(tmp, 'r.png'))[2][0][0:3]
    ok('Y 76, U 85, V 255 is red (got %d,%d,%d)' % (r, g, b), r > 240 and g < 8 and b < 8)

    # A wrong size is refused, and says what came.
    raises('an 1088x480 frame is refused', lambda: tool.convert(
        archive(os.path.join(tmp, 'small.tar.gz'), frame(128, 128, 128, 1088, 480), metadata()),
        os.path.join(tmp, 'small.png')), 'atlas')

    # A frame with no metadata, or metadata that does not match.
    raises('a frame with no metadata is refused', lambda: tool.convert(
        archive(os.path.join(tmp, 'nometa.tar.gz'), frame(128, 128, 128), None), os.path.join(tmp, 'x.png')),
        'no carplay-frame-1440x636.json')
    raises('an archive with no frame says to capture one', lambda: tool.convert(
        archive(os.path.join(tmp, 'noframe.tar.gz'), None, metadata()), os.path.join(tmp, 'x.png')),
        'Capture full CarPlay frame')
    raises('metadata of another size is refused', lambda: tool.convert(
        archive(os.path.join(tmp, 'badmeta.tar.gz'), frame(128, 128, 128), metadata(1088, 480)),
        os.path.join(tmp, 'x.png')), 'not 1440')

    # A corrupt archive, and a file that is no archive at all.
    bogus = os.path.join(tmp, 'bogus.tar.gz')
    with open(bogus, 'wb') as f:
        f.write(b'not a tar.gz at all')
    raises('a corrupt archive is refused', lambda: tool.convert(bogus, os.path.join(tmp, 'x.png')), 'cannot read')
    raises('a missing file is refused', lambda: tool.convert(os.path.join(tmp, 'nope.tar.gz'), os.path.join(tmp, 'x.png')),
           'no such archive')

    # A member that tries to escape is not taken.
    evil = os.path.join(tmp, 'evil.tar.gz')
    archive(evil, frame(128, 128, 128), None, member='../../carplay-frame-1440x636.yuva')
    raises('a member outside the archive is refused', lambda: tool.convert(evil, os.path.join(tmp, 'x.png')),
           'no carplay-frame-1440x636.yuva')

    # The command line writes the PNG and says what it did.
    cli_out = os.path.join(tmp, 'cli.png')
    argv = ['--output', cli_out, archive(os.path.join(tmp, 'cli.tar.gz'), frame(128, 128, 128), metadata())]
    eq('the command line returns 0', tool.main(argv), 0)
    ok('the command line wrote the PNG', os.path.isfile(cli_out) and png_size(cli_out)[:2] == (1440, 636))
    eq('the command line fails on a bad archive', tool.main(['-o', os.path.join(tmp, 'x.png'), bogus]), 1)
finally:
    for name in os.listdir(tmp):
        try:
            os.unlink(os.path.join(tmp, name))
        except OSError:
            pass
    os.rmdir(tmp)

if failures:
    sys.stdout.write('%d of %d FAILED\n' % (failures, checks))
    sys.exit(1)
sys.stdout.write('all %d ok\n' % checks)
