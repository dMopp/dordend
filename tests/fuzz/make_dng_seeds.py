#!/usr/bin/env python3
"""Writes small DNGs for rummage-fuzz-dng (ROADMAP 2d) into the folder given: a 16x16 RGB
thumbnail in IFD0 with the colour tags, the raw image in a SubIFD, in the shapes the reader
knows: 16-bit and packed 12-bit CFA strips, lossless JPEG strips and tiles (from the fixtures in
tests/data/image/dng), linear raw, a linearization table, black level deltas, crops, opcode
lists (gain maps before and after demosaic, a lens warp, bad pixel fixes).

    python3 -I tests/fuzz/make_dng_seeds.py build-fuzz/fuzz/seeds-dng
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "..", "data", "image", "dng")

BYTE, ASCII, SHORT, LONG, RATIONAL, UNDEFINED, SRATIONAL, FLOAT = 1, 2, 3, 4, 5, 7, 10, 11
SIZES = {BYTE: 1, ASCII: 1, SHORT: 2, LONG: 4, RATIONAL: 8, UNDEFINED: 1, SRATIONAL: 8, FLOAT: 4}


def encode(kind, values):
    if kind in (BYTE, UNDEFINED):
        return bytes(values)
    if kind == SHORT:
        return struct.pack("<%dH" % len(values), *values)
    if kind == LONG:
        return struct.pack("<%dI" % len(values), *values)
    if kind == FLOAT:
        return struct.pack("<%df" % len(values), *values)
    out = b""
    for v in values:
        if kind == RATIONAL:
            out += struct.pack("<II", int(round(v * 10000)), 10000)
        else:
            out += struct.pack("<ii", int(round(v * 10000)), 10000)
    return out


class Writer:
    def __init__(self):
        self.data = bytearray(b"II*\0\0\0\0\0")

    def blob(self, raw):
        at = len(self.data)
        self.data += raw
        if len(self.data) % 2:
            self.data += b"\0"
        return at

    def ifd(self, fields):
        fields = sorted(fields, key=lambda f: f[0])
        at = len(self.data)
        self.data += b"\0" * (2 + 12 * len(fields) + 4)
        struct.pack_into("<H", self.data, at, len(fields))
        for i, (tag, kind, values) in enumerate(fields):
            raw = encode(kind, values)
            entry = at + 2 + 12 * i
            struct.pack_into("<HHI", self.data, entry, tag, kind, len(raw) // SIZES[kind])
            if len(raw) <= 4:
                self.data[entry + 8:entry + 8 + len(raw)] = raw
            else:
                struct.pack_into("<I", self.data, entry + 8, self.blob(raw))
        return at

    def finish(self, first):
        struct.pack_into("<I", self.data, 4, first)
        return bytes(self.data)


XYZ_TO_SRGB = [3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560,
               0.0556434, -0.2040259, 1.0572252]


def dng(chunks, raw_fields, tiled=False, more=()):
    w = Writer()
    offsets = [w.blob(c) for c in chunks]
    counts = [len(c) for c in chunks]
    raw_fields = list(raw_fields) + [
        (324 if tiled else 273, LONG, offsets),
        (325 if tiled else 279, LONG, counts),
    ]
    sub = w.ifd(raw_fields)
    thumb = w.blob(bytes(16 * 16 * 3))
    first = w.ifd([
        (254, LONG, [1]), (256, SHORT, [16]), (257, SHORT, [16]), (258, SHORT, [8, 8, 8]),
        (259, SHORT, [1]), (262, SHORT, [2]), (273, LONG, [thumb]), (277, SHORT, [3]),
        (279, LONG, [768]), (330, LONG, [sub]), (50706, BYTE, [1, 4, 0, 0]),
        (50721, SRATIONAL, XYZ_TO_SRGB), (50778, SHORT, [21]),
        (50722, SRATIONAL, [v * 1.5 for v in XYZ_TO_SRGB]), (50779, SHORT, [17]),
        (50728, RATIONAL, [0.5, 1.0, 0.6]), (50730, SRATIONAL, [0.5]), (274, SHORT, [6]),
    ] + list(more))
    return w.finish(first)


def cfa(width, height, bits, compression, extra=()):
    return [
        (254, LONG, [0]), (256, LONG, [width]), (257, LONG, [height]), (258, SHORT, [bits]),
        (259, SHORT, [compression]), (262, SHORT, [32803]), (277, SHORT, [1]),
        (33421, SHORT, [2, 2]), (33422, BYTE, [0, 1, 1, 2]),
    ] + list(extra)


def sixteen(width, height):
    return b"".join(struct.pack("<H", (x * 977 + y * 131) & 0xFFFF)
                    for y in range(height) for x in range(width))


def packed12(width, height):
    out = bytearray()
    for y in range(height):
        bits, count = 0, 0
        for x in range(width):
            bits = (bits << 12) | ((x * 37 + y * 11) & 0xFFF)
            count += 12
            while count >= 8:
                count -= 8
                out.append((bits >> count) & 0xFF)
        if count:
            out.append((bits << (8 - count)) & 0xFF)
    return bytes(out)


def opcode_list(ops):
    """An opcode list (big-endian whatever the file's order) of (id, flags, body)."""
    out = struct.pack(">I", len(ops))
    for op, flags, body in ops:
        out += struct.pack(">IIII", op, 0x01030000, flags, len(body)) + body
    return out


def gain_map(area, plane, planes, pitch, points, map_planes, gains):
    body = struct.pack(">4I", *area) + struct.pack(">6I", plane, planes, pitch, pitch, *points)
    body += struct.pack(">4d", 1.0 / max(1, points[0] - 1), 1.0 / max(1, points[1] - 1), 0.0, 0.0)
    return body + struct.pack(">I", map_planes) + struct.pack(">%df" % len(gains), *gains)


def warp(planes, cx=0.5, cy=0.5):
    body = struct.pack(">I", len(planes))
    for k in planes:
        body += struct.pack(">6d", *k)
    return body + struct.pack(">2d", cx, cy)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    gray = open(os.path.join(FIXTURES, "gray16-p1.ljpg"), "rb").read()
    rgb = open(os.path.join(FIXTURES, "rgb14-p6-restart.ljpg"), "rb").read()
    seeds = {
        "cfa16": dng([sixteen(32, 24)], cfa(32, 24, 16, 1, [(50714, SHORT, [512]),
                                                            (50717, SHORT, [60000])])),
        "cfa12-strips": dng([packed12(32, 8), packed12(32, 8), packed12(32, 8)],
                            cfa(32, 24, 12, 1, [(278, SHORT, [8]), (50829, SHORT, [2, 2, 22, 30]),
                                                (50719, RATIONAL, [1, 1]),
                                                (50720, RATIONAL, [26, 18])])),
        "cfa8-linearized": dng([bytes((x * 7) & 0xFF for x in range(32 * 24))],
                               cfa(32, 24, 8, 1, [(50712, SHORT, [i * 64 for i in range(256)]),
                                                  (50713, SHORT, [2, 2]),
                                                  (50714, SHORT, [10, 11, 12, 13])])),
        "cfa-ljpeg-strip": dng([gray], cfa(37, 29, 16, 7, [
            (50716, SRATIONAL, [i * 0.5 for i in range(29)])])),
        "cfa-ljpeg-tiles": dng([gray] * 4, cfa(60, 50, 16, 7, [(322, SHORT, [37]),
                                                               (323, SHORT, [29])]), tiled=True),
        "linear-ljpeg": dng([rgb], [
            (254, LONG, [0]), (256, LONG, [37]), (257, LONG, [29]), (258, SHORT, [14, 14, 14]),
            (259, SHORT, [7]), (262, SHORT, [34892]), (277, SHORT, [3]),
        ], more=[(50940, FLOAT, [0.0, 0.0, 0.5, 0.625, 1.0, 1.0])]),
    }
    shading = [(9, 0, gain_map([r, c, 24, 32], 0, 1, 2, (2, 2), 1, [1.0, 1.2, 1.4, 1.6]))
               for r in (0, 1) for c in (0, 1)]
    after = [(9, 0, gain_map([0, 0, 24, 32], 0, 3, 1, (3, 3), 3, [1.0 + i / 30 for i in range(27)])),
             (1, 0, warp([[0.98, 0.13, -0.33, 0.17, 0.001, -0.002]] * 3, 0.45, 0.55))]
    bad_pixels = [(5, 0, struct.pack(">5I", 0, 2, 0, 3, 4) + b"\0" * 20)]
    seeds["cfa16-opcodes"] = dng([sixteen(32, 24)], cfa(32, 24, 16, 1, [
        (50714, SHORT, [512]), (50717, SHORT, [60000]),
        (51008, UNDEFINED, opcode_list(bad_pixels)), (51009, UNDEFINED, opcode_list(shading)),
        (51022, UNDEFINED, opcode_list(after))]))
    seeds["linear-opcodes"] = dng([rgb], [
        (254, LONG, [0]), (256, LONG, [37]), (257, LONG, [29]), (258, SHORT, [14, 14, 14]),
        (259, SHORT, [7]), (262, SHORT, [34892]), (277, SHORT, [3]),
        (51009, UNDEFINED, opcode_list([(9, 0, gain_map([0, 0, 29, 37], 0, 3, 1, (2, 1), 1,
                                                        [0.9, 1.1]))])),
        (51022, UNDEFINED, opcode_list([(1, 0, warp([[1.02, -0.05, 0.0, 0.0, 0.0, 0.0]])),
                                        (77, 1, b"")]))])
    for name, data in seeds.items():
        with open(os.path.join(out, name + ".dng"), "wb") as f:
            f.write(data)
    print("%d seeds in %s" % (len(seeds), out))


main()
