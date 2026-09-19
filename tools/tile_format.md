# Vector tile format `VT01`

One file per tile: `<region>/<z>/<x>/<y>.bin`, Web-Mercator tiling (same z/x/y as
OpenStreetMap slippy tiles, y grows southward). All integers little-endian.
Coordinates are tile-local: `0..4096` spans the tile; geometry is clipped to a
4 % buffer around the tile (`-164..4260`) so strokes don't show seams.

```
header
  char[4]  magic       "VT01"
  u16      extent      4096
  u16      n_groups
  u32      strings_off offset of the string table from file start

group  (repeated n_groups times; already sorted in draw order)
  u8   layer      see table below
  u8   cls        class within the layer
  u8   geom       0 point, 1 line, 2 polygon
  u8   reserved   0
  u16  n_features

  feature (repeated)
    u16  name_id  index into string table, 0xFFFF = unnamed
    u8   n_rings  polygons: outer ring first, then holes; lines/points: 1
    u8   flags    roads: bit0 tunnel, bit1 bridge, bit2 link, bit3 oneway
    ring (repeated n_rings)
      u16    n_pts
      int16  x, y   x n_pts   (polygon rings are NOT closed: last != first)

string table (at strings_off)
  u16  count
  entry (repeated): u8 len, utf8 bytes
```

Polygon rings: outer rings are wound clockwise on screen (y down), holes
counter-clockwise, so a nonzero-winding fill with |winding| clamped to 1 works
and a single feature's rings can be filled in one call.

## Layers and classes

| layer | name     | geom | classes |
|---|---|---|---|
| 0 | WATER    | poly | 0 water |
| 1 | LANDUSE  | poly | 0 park/grass, 1 forest/wood, 2 residential, 3 industrial/commercial/retail, 4 farmland/meadow, 5 cemetery, 6 school/university/hospital, 7 pitch/playground/golf, 8 airport, 9 parking |
| 2 | BUILDING | poly | 0 building |
| 3 | WATERWAY | line | 0 river, 1 canal, 2 stream/drain/ditch |
| 4 | ROAD     | line | 0 motorway, 1 trunk, 2 primary, 3 secondary, 4 tertiary, 5 residential/unclassified/living_street, 6 service, 7 footway/path/cycleway/steps/track/pedestrian, 8 other |
| 5 | RAIL     | line | 0 rail, 1 metro/light_rail/subway/monorail, 2 tram/other |
| 6 | AEROWAY  | line | 0 runway, 1 taxiway |
| 7 | PLACE    | point| 0 city, 1 town, 2 suburb, 3 neighbourhood/quarter, 4 village/hamlet/locality |
| 8 | POI      | point| 0 transport (station/airport/bus station), 1 hospital, 2 school/college/university, 3 park/attraction, 4 mall/marketplace, 5 place of worship, 6 food (restaurant/cafe), 7 other |

Draw order = group order in the file: WATER, LANDUSE, BUILDING, WATERWAY,
AEROWAY, RAIL, ROAD (class 8 down to 0, each drawn casing-then-fill by the
device), PLACE, POI.

`meta.json` next to the tiles: `{ "name", "bbox": [w,s,e,n], "minzoom",
"maxzoom", "center": [lon,lat,zoom], "extent": 4096 }`.
