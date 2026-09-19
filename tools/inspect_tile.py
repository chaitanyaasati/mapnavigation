#!/usr/bin/env python3
"""Decode a VT01 tile and print its structure (sanity check for build_tiles.py). Usage: inspect_tile.py z/x/y.bin"""
import struct, sys
LAYERS = ['WATER', 'LANDUSE', 'BUILDING', 'WATERWAY', 'ROAD', 'RAIL', 'AEROWAY', 'PLACE', 'POI']
d = open(sys.argv[1], 'rb').read()
magic, extent, n_groups, strings_off = struct.unpack_from('<4sHHI', d, 0)
assert magic == b'VT01', magic
# strings
cnt, = struct.unpack_from('<H', d, strings_off)
p = strings_off + 2
strings = []
for _ in range(cnt):
    n = d[p]; strings.append(d[p + 1:p + 1 + n].decode('utf-8')); p += 1 + n
print(f"{sys.argv[1]}: {len(d)} bytes, {n_groups} groups, {cnt} strings")
p = 12
tot_pts = 0
for _ in range(n_groups):
    layer, cls, geom, _, nf = struct.unpack_from('<BBBBH', d, p); p += 6
    pts = 0
    named = 0
    for _ in range(nf):
        name_id, n_rings, flags = struct.unpack_from('<HBB', d, p); p += 4
        if name_id != 0xFFFF: named += 1
        for _ in range(n_rings):
            n, = struct.unpack_from('<H', d, p); p += 2 + 4 * n; pts += n
    tot_pts += pts
    print(f"  {LAYERS[layer]:8s} cls {cls} {'PLP'[geom]}  {nf:5d} features {pts:6d} pts  ({named} named)")
assert p == strings_off, (p, strings_off)
print(f"  total {tot_pts} points; sample names: {strings[:8]}")
