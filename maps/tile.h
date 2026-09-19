#pragma once
// =============================================================================
//  tile - reader for the VT01 vector tile format (tools/tile_format.md)
// =============================================================================
//  Zero-copy: walks the tile bytes in place. All multi-byte fields sit on even
//  offsets, so int16 coordinate arrays can be read directly.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum TileLayer : uint8_t { L_WATER, L_LANDUSE, L_BUILDING, L_WATERWAY, L_ROAD, L_RAIL, L_AEROWAY, L_PLACE, L_POI, L_COUNT };
enum TileGeom  : uint8_t { G_POINT, G_LINE, G_POLY };
enum RoadFlag  : uint8_t { RF_TUNNEL = 1, RF_BRIDGE = 2, RF_LINK = 4, RF_ONEWAY = 8 };
static const int TILE_EXTENT = 4096;

struct TileGroup {
  uint8_t layer, cls, geom;
  uint16_t n_features;
  const uint8_t* first;      // first feature
};
struct TileFeature {
  uint16_t name_id;          // 0xFFFF = none
  uint8_t n_rings, flags;
  const uint8_t* rings;      // first ring: u16 n, then int16 xy[n]
};
struct TileRing {
  uint16_t n;
  const int16_t* xy;
};

class TileReader {
public:
  TileReader(const uint8_t* data, size_t len) : _d(data), _len(len) {
    _ok = len >= 12 && memcmp(data, "VT01", 4) == 0;
    if (_ok) {
      memcpy(&_extent, data + 4, 2);
      memcpy(&_nGroups, data + 6, 2);
      memcpy(&_stringsOff, data + 8, 4);
      _ok = _stringsOff <= len && _extent == TILE_EXTENT;
    }
    _p = data + 12;
    _gi = 0;
  }
  bool valid() const { return _ok; }
  uint16_t groupCount() const { return _nGroups; }

  // Sequential group iteration. Returns false at the end.
  bool nextGroup(TileGroup& g) {
    if (!_ok || _gi >= _nGroups || _p + 6 > _d + _stringsOff) return false;
    g.layer = _p[0]; g.cls = _p[1]; g.geom = _p[2];
    memcpy(&g.n_features, _p + 4, 2);
    g.first = _p + 6;
    // skip to the next group by walking features
    const uint8_t* q = g.first;
    for (uint16_t i = 0; i < g.n_features; i++) {
      TileFeature f;
      if (!readFeature(q, f)) { _ok = false; return false; }
      q = skipFeature(f);
    }
    _p = q;
    _gi++;
    return true;
  }

  // Feature access within a group: pass g.first, then the returned pointer.
  bool readFeature(const uint8_t* p, TileFeature& f) const {
    if (p + 4 > _d + _stringsOff) return false;
    memcpy(&f.name_id, p, 2);
    f.n_rings = p[2]; f.flags = p[3];
    f.rings = p + 4;
    return true;
  }
  const uint8_t* skipFeature(const TileFeature& f) const {
    const uint8_t* q = f.rings;
    for (uint8_t r = 0; r < f.n_rings; r++) {
      uint16_t n; memcpy(&n, q, 2);
      q += 2 + 4 * (size_t)n;
    }
    return q;
  }
  // Ring access: pass f.rings, then the returned pointer.
  static const uint8_t* readRing(const uint8_t* p, TileRing& r) {
    memcpy(&r.n, p, 2);
    r.xy = (const int16_t*)(p + 2);
    return p + 2 + 4 * (size_t)r.n;
  }

  // Name lookup (linear walk of the string table; tiles have few names).
  bool name(uint16_t id, char* out, size_t cap) const {
    if (!_ok || id == 0xFFFF) return false;
    const uint8_t* p = _d + _stringsOff;
    uint16_t cnt; memcpy(&cnt, p, 2); p += 2;
    if (id >= cnt) return false;
    for (uint16_t i = 0; i < id; i++) p += 1 + p[0];
    size_t n = p[0]; if (n >= cap) n = cap - 1;
    memcpy(out, p + 1, n); out[n] = 0;
    return true;
  }

private:
  const uint8_t* _d; size_t _len;
  bool _ok; uint16_t _extent, _nGroups; uint32_t _stringsOff;
  const uint8_t* _p; uint16_t _gi;
};
