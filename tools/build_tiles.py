#!/usr/bin/env python3
"""
build_tiles.py - OpenStreetMap .pbf  ->  lean binary vector tiles (see tile_format.md)

    .venv/bin/python build_tiles.py bengaluru.osm.pbf out/bengaluru \
        --bbox 77.40 12.78 77.85 13.25 --minzoom 10 --maxzoom 16

Pass 1 walks the .pbf once with pyosmium and keeps only the features the map
draws, as shapely geometries in Web-Mercator (normalised 0..1 world coords).
Pass 2, per zoom level: filter by minzoom/size, simplify to ~half a pixel,
index with an STRtree, and for every tile clip + quantise + serialise.

Output: <out>/<z>/<x>/<y>.bin plus <out>/meta.json.
"""
import argparse, json, math, os, struct, sys, time
from collections import defaultdict

import numpy as np
import osmium
import shapely
from shapely import STRtree
from shapely.geometry import box
from shapely.ops import orient

# ---- layer / class enums (must match tile_format.md and maps/style.h) -------
L_WATER, L_LANDUSE, L_BUILDING, L_WATERWAY, L_ROAD, L_RAIL, L_AEROWAY, L_PLACE, L_POI = range(9)
G_POINT, G_LINE, G_POLY = 0, 1, 2
EXTENT = 4096
BUFFER = 0.04            # clip buffer as fraction of tile size
DRAW_ORDER = [L_WATER, L_LANDUSE, L_BUILDING, L_WATERWAY, L_AEROWAY, L_RAIL, L_ROAD, L_PLACE, L_POI]

ROAD_CLASS = {
    'motorway': 0, 'motorway_link': 0, 'trunk': 1, 'trunk_link': 1,
    'primary': 2, 'primary_link': 2, 'secondary': 3, 'secondary_link': 3,
    'tertiary': 4, 'tertiary_link': 4,
    'residential': 5, 'unclassified': 5, 'living_street': 5,
    'service': 6,
    'footway': 7, 'path': 7, 'cycleway': 7, 'steps': 7, 'track': 7, 'pedestrian': 7, 'bridleway': 7,
    'road': 8, 'busway': 8,
}
ROAD_MINZOOM = {0: 10, 1: 10, 2: 11, 3: 12, 4: 13, 5: 14, 6: 15, 7: 15, 8: 15}
RAIL_CLASS = {'rail': 0, 'narrow_gauge': 0, 'subway': 1, 'light_rail': 1, 'monorail': 1, 'tram': 2, 'funicular': 2}
RAIL_MINZOOM = {0: 11, 1: 12, 2: 14}
WATERWAY_CLASS = {'river': 0, 'canal': 1, 'stream': 2, 'drain': 2, 'ditch': 2}
WATERWAY_MINZOOM = {0: 11, 1: 13, 2: 14}
LANDUSE_MINZOOM = {0: 12, 1: 12, 2: 13, 3: 13, 4: 13, 5: 13, 6: 13, 7: 14, 8: 11, 9: 15}
PLACE_CLASS = {'city': 0, 'town': 1, 'suburb': 2, 'neighbourhood': 3, 'quarter': 3, 'village': 4, 'hamlet': 4, 'locality': 4}
PLACE_MINZOOM = {0: 10, 1: 11, 2: 13, 3: 14, 4: 13}
POI_MINZOOM = {0: 13, 1: 14, 2: 15, 3: 14, 4: 14, 5: 15, 6: 16, 7: 16}


def landuse_class(t):
    lu, le, na, am, ae = t.get('landuse'), t.get('leisure'), t.get('natural'), t.get('amenity'), t.get('aeroway')
    if le in ('park', 'garden', 'nature_reserve', 'dog_park') or lu in ('grass', 'recreation_ground', 'village_green', 'greenfield'): return 0
    if lu == 'forest' or na in ('wood', 'scrub', 'heath'): return 1
    if lu == 'residential': return 2
    if lu in ('industrial', 'commercial', 'retail', 'railway', 'construction', 'brownfield'): return 3
    if lu in ('farmland', 'meadow', 'orchard', 'farmyard', 'allotments', 'plant_nursery', 'vineyard'): return 4
    if lu == 'cemetery' or am == 'grave_yard': return 5
    if am in ('school', 'university', 'college', 'hospital') : return 6
    if le in ('pitch', 'playground', 'golf_course', 'stadium', 'sports_centre', 'track'): return 7
    if ae in ('aerodrome', 'apron', 'runway', 'taxiway') or lu == 'military': return 8
    if am == 'parking': return 9
    return None


def poi_class(t):
    am, sh, to, ra, pt, ae = t.get('amenity'), t.get('shop'), t.get('tourism'), t.get('railway'), t.get('public_transport'), t.get('aeroway')
    if ra == 'station' or pt == 'station' or am == 'bus_station' or ae == 'aerodrome': return 0
    if am in ('hospital', 'clinic'): return 1
    if am in ('school', 'university', 'college'): return 2
    if to in ('attraction', 'museum', 'zoo', 'theme_park', 'viewpoint') or t.get('leisure') == 'park': return 3
    if sh in ('mall', 'department_store', 'supermarket') or am == 'marketplace': return 4
    if am == 'place_of_worship': return 5
    if am in ('restaurant', 'cafe', 'fast_food'): return 6
    if am in ('library', 'theatre', 'cinema', 'townhall', 'police', 'post_office', 'bank', 'fuel', 'pharmacy') or to == 'hotel': return 7
    return None


def merc(lon, lat):
    """lon/lat -> normalised Web-Mercator (0..1, y down)."""
    x = (lon + 180.0) / 360.0
    s = math.sin(math.radians(max(-85.05, min(85.05, lat))))
    y = 0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)
    return x, y


# ---- pass 1: collect features ---------------------------------------------------
class Collector(osmium.SimpleHandler):
    def __init__(self, bbox):
        super().__init__()
        self.wkb = osmium.geom.WKBFactory()
        self.bbox_poly = box(*bbox)
        self.bbox = bbox
        # per layer: lists of (cls, flags, name, geom)
        self.feat = defaultdict(list)
        self.n = 0

    def _in_bbox(self, lon, lat):
        w, s, e, n = self.bbox
        return w <= lon <= e and s <= lat <= n

    def _add(self, layer, cls, flags, name, geom):
        if geom is None or geom.is_empty:
            return
        self.feat[layer].append((cls, flags, name, geom))
        self.n += 1
        if self.n % 100000 == 0:
            print(f"  {self.n} features...", file=sys.stderr)

    def node(self, n):
        t = n.tags
        if not n.location.valid() or not self._in_bbox(n.location.lon, n.location.lat):
            return
        name = t.get('name:en') or t.get('name')
        if not name:
            return
        pt = shapely.Point(n.location.lon, n.location.lat)
        if 'place' in t and t['place'] in PLACE_CLASS:
            self._add(L_PLACE, PLACE_CLASS[t['place']], 0, name, pt)
        else:
            pc = poi_class(t)
            if pc is not None:
                self._add(L_POI, pc, 0, name, pt)

    def way(self, w):
        t = w.tags
        if t.get('area') == 'yes':
            return
        hw = t.get('highway')
        layer = cls = None
        flags = 0
        if hw in ROAD_CLASS:
            layer, cls = L_ROAD, ROAD_CLASS[hw]
            if t.get('tunnel') in ('yes', 'building_passage'): flags |= 1
            if t.get('bridge') in ('yes', 'viaduct'): flags |= 2
            if hw.endswith('_link'): flags |= 4
            if t.get('oneway') == 'yes': flags |= 8
        elif t.get('railway') in RAIL_CLASS and t.get('service') not in ('yard', 'siding', 'spur'):
            layer, cls = L_RAIL, RAIL_CLASS[t['railway']]
        elif t.get('waterway') in WATERWAY_CLASS:
            layer, cls = L_WATERWAY, WATERWAY_CLASS[t['waterway']]
        elif t.get('aeroway') in ('runway', 'taxiway'):
            layer, cls = L_AEROWAY, 0 if t['aeroway'] == 'runway' else 1
        if layer is None:
            return
        try:
            geom = shapely.from_wkb(self.wkb.create_linestring(w))
        except Exception:
            return
        if not geom.intersects(self.bbox_poly):
            return
        self._add(layer, cls, flags, t.get('name:en') or t.get('name'), geom)

    def area(self, a):
        t = a.tags
        layer = cls = None
        if 'building' in t and t['building'] != 'no':
            layer, cls = L_BUILDING, 0
        elif t.get('natural') == 'water' or t.get('waterway') == 'riverbank' or t.get('landuse') in ('reservoir', 'basin') or t.get('water'):
            layer, cls = L_WATER, 0
        else:
            lc = landuse_class(t)
            if lc is not None:
                layer, cls = L_LANDUSE, lc
        if layer is None:
            return
        try:
            geom = shapely.from_wkb(self.wkb.create_multipolygon(a))
        except Exception:
            return
        if not geom.intersects(self.bbox_poly):
            return
        name = t.get('name:en') or t.get('name')
        self._add(layer, cls, 0, name, geom)
        # named parks / campuses / malls also get a label point
        if name and layer == L_LANDUSE:
            pc = 3 if cls in (0, 1, 7) else (2 if cls == 6 else None)
            if pc is not None:
                self._add(L_POI, pc, 0, name, geom.representative_point())
        elif name and layer == L_BUILDING:
            pc = poi_class(t)
            if pc is not None:
                self._add(L_POI, pc, 0, name, geom.representative_point())


# ---- pass 2: tiling -----------------------------------------------------------------
def to_merc_array(geoms):
    """Project an array of lon/lat geometries to normalised mercator in place (vectorised)."""
    def tx(coords):
        lon, lat = coords[:, 0], coords[:, 1]
        x = (lon + 180.0) / 360.0
        s = np.sin(np.radians(np.clip(lat, -85.05, 85.05)))
        y = 0.5 - np.log((1 + s) / (1 - s)) / (4 * np.pi)
        return np.column_stack([x, y])
    return shapely.transform(geoms, tx)


def min_zoom_of(layer, cls):
    return {L_WATER: {0: 10}, L_LANDUSE: LANDUSE_MINZOOM, L_BUILDING: {0: 15}, L_WATERWAY: WATERWAY_MINZOOM,
            L_ROAD: ROAD_MINZOOM, L_RAIL: RAIL_MINZOOM, L_AEROWAY: {0: 12, 1: 12},
            L_PLACE: PLACE_MINZOOM, L_POI: POI_MINZOOM}[layer].get(cls, 99)


def encode_tile(groups, strings):
    """groups: list of (layer, cls, geom_type, [ (name_id, flags, [ring int16 array, ...]) ]) in draw order."""
    out = bytearray()
    body = bytearray()
    for layer, cls, gt, feats in groups:
        body += struct.pack('<BBBBH', layer, cls, gt, 0, len(feats))
        for name_id, flags, rings in feats:
            body += struct.pack('<HBB', name_id, len(rings), flags)
            for r in rings:
                body += struct.pack('<H', len(r))
                body += r.astype('<i2').tobytes()
    header_len = 4 + 2 + 2 + 4
    out += b'VT01' + struct.pack('<HHI', EXTENT, len(groups), header_len + len(body))
    out += body
    out += struct.pack('<H', len(strings))
    for s in strings:
        b = s.encode('utf-8')[:255]
        out += struct.pack('<B', len(b)) + b
    return bytes(out)


def quantise_rings(geom, tx0, ty0, scale, is_poly):
    """Yield int16 (n,2) arrays for each ring/part of geom in tile-local units. None if degenerate."""
    parts = []
    if is_poly:
        for poly in shapely.get_parts(geom):
            if poly.geom_type != 'Polygon' or poly.is_empty:
                continue
            poly = orient(poly, sign=1.0)              # exterior CCW (math) -> CW on screen after y-down
            rings = []
            for ring in [poly.exterior, *poly.interiors]:
                c = np.asarray(ring.coords)[:-1]
                q = np.rint((c - (tx0, ty0)) * scale)
                q = dedupe(q)
                if len(q) >= 3:
                    rings.append(q)
            if rings:
                parts.append(rings)
    else:
        for line in shapely.get_parts(geom):
            if line.geom_type == 'Point':
                q = np.rint((np.asarray(line.coords) - (tx0, ty0)) * scale)
                parts.append([q])
                continue
            if line.geom_type != 'LineString' or line.is_empty:
                continue
            q = np.rint((np.asarray(line.coords) - (tx0, ty0)) * scale)
            q = dedupe(q)
            if len(q) >= 2:
                parts.append([q])
    return parts


def dedupe(q):
    if len(q) < 2:
        return q
    keep = np.ones(len(q), dtype=bool)
    keep[1:] = np.any(q[1:] != q[:-1], axis=1)
    return q[keep]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pbf')
    ap.add_argument('out')
    ap.add_argument('--bbox', nargs=4, type=float, required=True, metavar=('W', 'S', 'E', 'N'))
    ap.add_argument('--minzoom', type=int, default=10)
    ap.add_argument('--maxzoom', type=int, default=16)
    ap.add_argument('--name', default=None)
    ap.add_argument('--max-buildings', type=int, default=2500, help='per tile, largest first')
    args = ap.parse_args()
    bbox = tuple(args.bbox)

    t0 = time.time()
    print(f"pass 1: reading {args.pbf}", file=sys.stderr)
    col = Collector(bbox)
    col.apply_file(args.pbf, locations=True, idx='flex_mem')
    print(f"  {col.n} features in {time.time() - t0:.0f}s: " +
          ", ".join(f"L{l}={len(v)}" for l, v in sorted(col.feat.items())), file=sys.stderr)

    # per layer: numpy arrays
    layers = {}
    for layer, items in col.feat.items():
        cls = np.array([i[0] for i in items], dtype=np.uint8)
        flags = np.array([i[1] for i in items], dtype=np.uint8)
        names = [i[2] for i in items]
        geoms = to_merc_array(np.array([i[3] for i in items], dtype=object))
        minz = np.array([min_zoom_of(layer, int(c)) for c in cls], dtype=np.int16)
        # size in mercator units (for per-zoom culling): area for polys, length for lines
        if layer in (L_WATER, L_LANDUSE, L_BUILDING):
            size = shapely.area(geoms)
        elif layer in (L_PLACE, L_POI):
            size = np.full(len(geoms), np.inf)
        else:
            size = shapely.length(geoms)
        layers[layer] = dict(cls=cls, flags=flags, names=names, geoms=geoms, minz=minz, size=size)
    del col

    os.makedirs(args.out, exist_ok=True)
    wx0, wy0 = merc(bbox[0], bbox[3])   # NW corner
    wx1, wy1 = merc(bbox[2], bbox[1])   # SE corner
    total_bytes = 0
    total_tiles = 0

    for z in range(args.minzoom, args.maxzoom + 1):
        n = 2 ** z
        tile_size = 1.0 / n                       # in world units
        px = tile_size / 256.0                    # one screen pixel at integer zoom
        tol = 0.5 * px
        tx_min, tx_max = int(wx0 * n), min(n - 1, int(wx1 * n))
        ty_min, ty_max = int(wy0 * n), min(n - 1, int(wy1 * n))
        zt = time.time()

        # per layer: eligible + simplified subset and an index
        prepared = {}
        for layer, L in layers.items():
            elig = L['minz'] <= z
            if layer in (L_WATER, L_LANDUSE):
                elig &= L['size'] >= (px * px) * (16 if z < 14 else 4)
            elif layer == L_BUILDING:
                elig &= L['size'] >= (px * px) * (10 if z == 15 else 2)
            elif layer in (L_ROAD, L_RAIL, L_WATERWAY, L_AEROWAY):
                elig &= L['size'] >= px * 3
            idx = np.nonzero(elig)[0]
            if len(idx) == 0:
                continue
            g = L['geoms'][idx]
            if layer not in (L_PLACE, L_POI):
                g = shapely.simplify(g, tol, preserve_topology=True)
            prepared[layer] = dict(idx=idx, geoms=g, tree=STRtree(g))

        z_tiles = z_bytes = 0
        for tx in range(tx_min, tx_max + 1):
            for ty in range(ty_min, ty_max + 1):
                x0, y0 = tx * tile_size, ty * tile_size
                b = BUFFER * tile_size
                clip_box = box(x0 - b, y0 - b, x0 + tile_size + b, y0 + tile_size + b)
                scale = EXTENT / tile_size
                strings, string_ids = [], {}
                groups = []

                def name_id(nm):
                    if not nm:
                        return 0xFFFF
                    if nm not in string_ids:
                        string_ids[nm] = len(strings)
                        strings.append(nm)
                    return string_ids[nm]

                for layer in DRAW_ORDER:
                    P = prepared.get(layer)
                    if P is None:
                        continue
                    hits = P['tree'].query(clip_box)
                    if len(hits) == 0:
                        continue
                    L = layers[layer]
                    is_poly = layer in (L_WATER, L_LANDUSE, L_BUILDING)
                    is_pt = layer in (L_PLACE, L_POI)
                    if layer == L_BUILDING and len(hits) > args.max_buildings:
                        order = np.argsort(-L['size'][P['idx'][hits]])
                        hits = hits[order[:args.max_buildings]]
                    geoms = P['geoms'][hits] if is_pt else shapely.clip_by_rect(P['geoms'][hits], *clip_box.bounds)
                    per_class = defaultdict(list)
                    for h, g in zip(hits, geoms):
                        if g is None or g.is_empty:
                            continue
                        gi = P['idx'][h]
                        parts = quantise_rings(g, x0, y0, scale, is_poly)
                        for rings in parts:
                            per_class[int(L['cls'][gi])].append((name_id(L['names'][gi]), int(L['flags'][gi]), rings))
                    # roads: minor classes first so major roads draw on top
                    classes = sorted(per_class, reverse=(layer == L_ROAD))
                    for c in classes:
                        groups.append((layer, c, G_POLY if is_poly else (G_POINT if is_pt else G_LINE), per_class[c]))

                if not groups:
                    continue
                data = encode_tile(groups, strings)
                d = os.path.join(args.out, str(z), str(tx))
                os.makedirs(d, exist_ok=True)
                with open(os.path.join(d, f"{ty}.bin"), 'wb') as f:
                    f.write(data)
                z_tiles += 1
                z_bytes += len(data)
        total_tiles += z_tiles
        total_bytes += z_bytes
        print(f"z{z}: {z_tiles} tiles, {z_bytes / 1024:.0f} KB, avg {z_bytes / max(1, z_tiles) / 1024:.1f} KB, {time.time() - zt:.0f}s", file=sys.stderr)

    meta = dict(name=args.name or os.path.basename(args.out.rstrip('/')), bbox=list(bbox), minzoom=args.minzoom,
                maxzoom=args.maxzoom, center=[(bbox[0] + bbox[2]) / 2, (bbox[1] + bbox[3]) / 2, 13], extent=EXTENT)
    with open(os.path.join(args.out, 'meta.json'), 'w') as f:
        json.dump(meta, f, indent=1)
    print(f"done: {total_tiles} tiles, {total_bytes / 1048576:.1f} MB in {time.time() - t0:.0f}s", file=sys.stderr)


if __name__ == '__main__':
    main()
