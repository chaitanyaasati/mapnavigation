#!/usr/bin/env python3
"""
build_places.py - OSM .pbf -> places.bin (offline geocoder index for the device)

    .venv/bin/python build_places.py bengaluru.osm.pbf out/bengaluru/places.bin --bbox 77.40 12.78 77.85 13.25

Entries: places (city..locality), named POIs (stations, malls, hospitals,
parks, schools, temples...), named landuse/buildings, and named roads that are
compact enough to have one meaningful location (a "1st Main Road" that exists
in forty layouts is dropped as ambiguous).

Format (little-endian):
  char[4] "PLC1", u32 count
  entry: f32 lon, f32 lat, u8 rank (0 best), u8 kind, u8 len, char name[len]
"""
import argparse, math, struct, sys
from collections import defaultdict
import osmium, shapely

PLACE_RANK = {'city': 0, 'town': 1, 'suburb': 2, 'quarter': 3, 'neighbourhood': 3, 'village': 3, 'hamlet': 4, 'locality': 4}
K_PLACE, K_POI, K_AREA, K_ROAD = 0, 1, 2, 3

def poi_rank(t):
    if t.get('railway') == 'station' or t.get('public_transport') == 'station' or t.get('aeroway') == 'aerodrome' or t.get('amenity') == 'bus_station': return 3
    if t.get('shop') in ('mall', 'department_store') or t.get('amenity') in ('hospital', 'university', 'college', 'marketplace', 'townhall', 'stadium') or t.get('leisure') in ('park', 'stadium', 'golf_course') or t.get('tourism') in ('attraction', 'museum', 'zoo', 'theme_park'): return 4
    if t.get('amenity') in ('school', 'place_of_worship', 'library', 'theatre', 'cinema', 'police', 'post_office', 'clinic', 'bank', 'fuel') or t.get('tourism') == 'hotel' or t.get('office') or t.get('shop') in ('supermarket',): return 5
    if t.get('amenity') in ('restaurant', 'cafe', 'fast_food', 'pharmacy', 'pub', 'bar') or t.get('shop'): return 6
    return None

class H(osmium.SimpleHandler):
    def __init__(self, bbox):
        super().__init__(); self.bbox = bbox; self.wkb = osmium.geom.WKBFactory()
        self.entries = []; self.roads = defaultdict(list)
    def inb(self, lon, lat):
        w, s, e, n = self.bbox; return w <= lon <= e and s <= lat <= n
    def node(self, n):
        t = n.tags; name = t.get('name:en') or t.get('name')
        if not name or not n.location.valid() or not self.inb(n.location.lon, n.location.lat): return
        if t.get('place') in PLACE_RANK: self.entries.append((n.location.lon, n.location.lat, PLACE_RANK[t['place']], K_PLACE, name))
        else:
            r = poi_rank(t)
            if r is not None: self.entries.append((n.location.lon, n.location.lat, r, K_POI, name))
    def way(self, w):
        t = w.tags; name = t.get('name:en') or t.get('name')
        if not name or 'highway' not in t or t.get('area') == 'yes': return
        try: g = shapely.from_wkb(self.wkb.create_linestring(w))
        except Exception: return
        c = g.centroid
        if self.inb(c.x, c.y): self.roads[name.strip()].append(g)
    def area(self, a):
        t = a.tags; name = t.get('name:en') or t.get('name')
        if not name: return
        r = None
        if t.get('place') in PLACE_RANK: r, k = PLACE_RANK[t['place']], K_PLACE
        else:
            r = poi_rank(t); k = K_AREA
            if r is None and (t.get('landuse') or t.get('leisure') or t.get('amenity') or t.get('building') in ('apartments', 'commercial', 'office', 'hospital', 'school', 'university', 'mall', 'hotel')): r, k = 6, K_AREA
        if r is None: return
        try: g = shapely.from_wkb(self.wkb.create_multipolygon(a))
        except Exception: return
        c = g.representative_point()
        if self.inb(c.x, c.y): self.entries.append((c.x, c.y, r, k, name))

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('pbf'); ap.add_argument('out')
    ap.add_argument('--bbox', nargs=4, type=float, required=True); a = ap.parse_args()
    h = H(tuple(a.bbox)); h.apply_file(a.pbf, locations=True, idx='flex_mem')
    entries = list(h.entries)
    dropped = 0
    for name, segs in h.roads.items():
        u = shapely.union_all(segs); minx, miny, maxx, maxy = u.bounds
        diag_km = math.hypot((maxx - minx) * 111 * math.cos(math.radians(miny)), (maxy - miny) * 111)
        if diag_km > 3.0: dropped += 1; continue
        c = u.centroid; entries.append((c.x, c.y, 7, K_ROAD, name))
    # dedupe identical (name, ~location); prefer better rank
    seen = {}
    for lon, lat, rank, kind, name in entries:
        key = (name.lower(), round(lon, 3), round(lat, 3))
        if key not in seen or rank < seen[key][2]: seen[key] = (lon, lat, rank, kind, name)
    entries = sorted(seen.values(), key=lambda e: (e[2], e[4]))
    with open(a.out, 'wb') as f:
        f.write(b'PLC1' + struct.pack('<I', len(entries)))
        for lon, lat, rank, kind, name in entries:
            b = name.encode('utf-8')[:60]
            f.write(struct.pack('<ffBBB', lon, lat, rank, kind, len(b)) + b)
    kinds = defaultdict(int)
    for e in entries: kinds[e[3]] += 1
    print(f"{len(entries)} entries (places {kinds[0]}, pois {kinds[1]}, areas {kinds[2]}, roads {kinds[3]}; {dropped} ambiguous roads dropped) -> {a.out}", file=sys.stderr)

if __name__ == '__main__': main()
