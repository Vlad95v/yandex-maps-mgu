#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Robert Nisipeanu
"""The full CarPlay frame from a woc-logs archive, as a PNG.

The app's Logs page can ask waze-on-cluster for one full CarPlay frame
("Capture full CarPlay frame", frame_dump.h): a 1440x636 4:2:0 picture as
the atlas the cluster renderer takes (native/cluster-render/kit/yuv_atlas.h),
1,373,760 bytes, beside a small JSON with its size and capture time. Save
logs puts both in the archive; this tool reads them out of it and writes the
PNG, so the picture can be looked at without any other tool.

    py -3 native\\waze-on-cluster\\tools\\yuv_atlas_to_png.py ^
        "C:\\path\\woc-logs-XXXXXXXX.tar.gz" ^
        --output "C:\\path\\carplay-full-frame.png"

The colours are the shader's, coordinate for coordinate (atlas_rgb() in
yuv_atlas.cpp): full-range BT.601, chroma left-sited horizontally and
centred vertically, each chroma sample upsampled bilinearly inside its own
part of the atlas with the coordinates clamped half a texel in from the
edges. Only the standard library is used: no Pillow, no OpenCV.

The archive is never unpacked to disk: only the two expected members are
read, by name, and a member anywhere else is ignored.
"""

import argparse
import json
import math
import os
import struct
import sys
import tarfile
import zlib

FRAME_NAME = 'carplay-frame-1440x636.yuva'
JSON_NAME = 'carplay-frame-1440x636.json'
WIDTH, HEIGHT = 1440, 636


class Error(Exception):
    """Something the user has to fix: the message says what."""


def atlas_height(h):
    return h + h // 2


def atlas_bytes(w=WIDTH, h=HEIGHT):
    return w * atlas_height(h)


def _safe_member_name(name):
    """The member's path has no way out of the archive: no absolute path, no
    '..' component, no drive letter. Nothing is ever unpacked to disk, but a
    frame taken from such a member is refused all the same."""
    parts = name.replace('\\', '/').split('/')
    return bool(name) and not name.startswith('/') and '..' not in parts and ':' not in parts[0]


def _member(archive, name):
    """The archive's member called name, anywhere in it, a regular file."""
    for m in archive.getmembers():
        if not _safe_member_name(m.name):
            continue
        if os.path.basename(m.name) == name:
            if not m.isfile():
                raise Error('%s in the archive is not a regular file' % name)
            return m
    return None


def read_frame(archive_path):
    """(atlas bytes, metadata dict) from the archive; Error when it does not
    hold a full frame."""
    if not os.path.isfile(archive_path):
        raise Error('no such archive: %s' % archive_path)
    try:
        with tarfile.open(archive_path, 'r:gz') as archive:
            frame = _member(archive, FRAME_NAME)
            meta = _member(archive, JSON_NAME)
            if frame is None:
                raise Error('the archive holds no %s: press "Capture full CarPlay frame" before Save logs' % FRAME_NAME)
            if meta is None:
                raise Error('the archive holds no %s: the frame has no metadata' % JSON_NAME)
            atlas = archive.extractfile(frame).read()
            text = archive.extractfile(meta).read().decode('utf-8')
    except (tarfile.TarError, OSError, EOFError) as e:
        raise Error('cannot read %s: %s' % (archive_path, e))
    try:
        info = json.loads(text)
    except ValueError as e:
        raise Error('%s is not JSON: %s' % (JSON_NAME, e))
    if not isinstance(info, dict):
        raise Error('%s is not a JSON object' % JSON_NAME)
    for key, want in (('width', WIDTH), ('height', HEIGHT)):
        if info.get(key) != want:
            raise Error('%s says %s %r, not %d' % (JSON_NAME, key, info.get(key), want))
    if info.get('full_frame') is not True:
        raise Error('%s does not say it is a full frame' % JSON_NAME)
    want = atlas_bytes()
    if len(atlas) != want:
        raise Error('the frame is %d bytes, not the %d of a %dx%d 4:2:0 atlas' % (len(atlas), want, WIDTH, HEIGHT))
    if info.get('size') not in (None, len(atlas)):
        raise Error('%s says %r bytes, the frame is %d' % (JSON_NAME, info.get('size'), len(atlas)))
    return atlas, info


def _bilinear(plane, pitch, rows, i, ax, j, ay):
    i1 = i + 1 if i + 1 < pitch else pitch - 1
    j1 = j + 1 if j + 1 < rows else rows - 1
    top = (1 - ax) * plane[j * pitch + i] + ax * plane[j * pitch + i1]
    bottom = (1 - ax) * plane[j1 * pitch + i] + ax * plane[j1 * pitch + i1]
    return ((1 - ay) * top + ay * bottom) / 255.0


def _level(v):
    return int(round(max(0.0, min(1.0, v)) * 255.0))


def to_png(atlas, w=WIDTH, h=HEIGHT):
    """The PNG bytes of the frame: RGB8, the atlas_rgb() colours."""
    th = atlas_height(h)
    half_w = w // 2
    # atlas_rgb's own sampling: luma at the pixel itself, chroma at the
    # half-resolution grid, clamped half a texel in from its part's edges.
    cx = []
    for x in range(w):
        s = min(max((x + 0.5) * 0.5 + 0.25, 0.5), half_w - 0.5)
        xc = s - 0.5
        i = int(math.floor(xc))
        cx.append((max(0, min(i, half_w - 1)), xc - i))
    cy = []
    for y in range(h):
        s = min(max((y + 0.5) * 0.5, 0.5), half_w - 0.5)
        yc = s - 0.5
        j = int(math.floor(yc))
        cy.append((max(0, min(j, half_w - 1)), yc - j))
    rows = []
    for y in range(h):
        row = bytearray(1 + w * 3)
        base = y * w
        cj, ay = cy[y]
        bj = h + cj
        for x in range(w):
            yy = atlas[base + x] / 255.0
            ci, ax = cx[x]
            cu = _bilinear(atlas, w, th, ci, ax, bj, ay) - 128.0 / 255.0
            cv = _bilinear(atlas, w, th, half_w + ci, ax, bj, ay) - 128.0 / 255.0
            o = 1 + x * 3
            row[o] = _level(yy + 1.402 * cv)
            row[o + 1] = _level(yy - 0.344136 * cu - 0.714136 * cv)
            row[o + 2] = _level(yy + 1.772 * cu)
        rows.append(bytes(row))
    raw = b''.join(rows)

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

    header = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(raw, 6)) +
            chunk(b'IEND', b''))


def convert(archive_path, out_path):
    atlas, info = read_frame(archive_path)
    png = to_png(atlas)
    folder = os.path.dirname(os.path.abspath(out_path))
    if folder and not os.path.isdir(folder):
        raise Error('no such folder: %s' % folder)
    try:
        with open(out_path, 'wb') as f:
            f.write(png)
    except OSError as e:
        raise Error('cannot write %s: %s' % (out_path, e))
    return info, png


def main(argv=None):
    ap = argparse.ArgumentParser(description='The full CarPlay frame of a woc-logs archive, as a PNG.')
    ap.add_argument('archive', help='a woc-logs-<UTC>.tar.gz the app\'s Save logs wrote')
    ap.add_argument('--output', '-o', required=True, help='the PNG to write, e.g. carplay-full-frame.png')
    args = ap.parse_args(argv)
    try:
        info, png = convert(args.archive, args.output)
    except Error as e:
        sys.stderr.write('yuv_atlas_to_png: %s\n' % e)
        return 1
    print('%s: %dx%d, %d bytes, captured %s' % (args.output, WIDTH, HEIGHT, len(png), info.get('captured_utc', '?')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
