#include "renderer.h"
#include "style.h"
#include <math.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#define SCRATCH_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#else
#include <stdlib.h>
#define SCRATCH_ALLOC(n) malloc(n)
#endif

static const int PT_CAP = 16384;

bool MapRenderer::init() {
  if (!_pts) _pts = (RPoint*)SCRATCH_ALLOC(PT_CAP * sizeof(RPoint));
  _cap = PT_CAP;
  return _pts != nullptr;
}

int MapRenderer::transform(const TileRing& ring, const TilePlacement& tp, int base) {
  int n = ring.n;
  if (base + n > _cap) n = _cap - base;
  if (n <= 0) return 0;
  const int16_t* xy = ring.xy;
  RPoint* out = _pts + base;
  for (int i = 0; i < n; i++) {
    out[i].x = tp.ox + xy[2 * i] * tp.scale;
    out[i].y = tp.oy + xy[2 * i + 1] * tp.scale;
  }
  return n;
}

void MapRenderer::drawTile(const uint8_t* data, size_t len, const TilePlacement& tp, float zoom, RenderPass pass,
                           LabelSink sink, void* user) {
  TileReader t(data, len);
  if (!t.valid()) return;
  TileGroup g;
  while (t.nextGroup(g)) {
    switch (pass) {
      case PASS_AREA:
        if (g.geom == G_POLY) drawAreaGroup(t, g, tp, zoom);
        break;
      case PASS_LINE:
        if (g.layer == L_WATERWAY || g.layer == L_AEROWAY || g.layer == L_RAIL) drawLineGroup(t, g, tp, zoom, pass);
        break;
      case PASS_ROAD_CASING:
      case PASS_ROAD_FILL:
        if (g.layer == L_ROAD) drawLineGroup(t, g, tp, zoom, pass);
        break;
      case PASS_LABELS:
        if (sink && g.geom == G_POINT) {
          const uint8_t* p = g.first;
          char name[64];
          for (uint16_t i = 0; i < g.n_features; i++) {
            TileFeature f; if (!t.readFeature(p, f)) return;
            TileRing r; TileReader::readRing(f.rings, r);
            if (r.n >= 1 && t.name(f.name_id, name, sizeof name)) {
              LabelCandidate c;
              c.x = tp.ox + r.xy[0] * tp.scale; c.y = tp.oy + r.xy[1] * tp.scale;
              c.layer = g.layer; c.cls = g.cls; c.name = name;
              sink(c, user);
            }
            p = t.skipFeature(f);
          }
        }
        break;
      default: break;
    }
  }
}

void MapRenderer::drawAreaGroup(TileReader& t, const TileGroup& g, const TilePlacement& tp, float zoom) {
  uint16_t color;
  switch (g.layer) {
    case L_WATER:    color = STYLE_WATER; break;
    case L_LANDUSE:  color = g.cls < sizeof(STYLE_LANDUSE) / sizeof(STYLE_LANDUSE[0]) ? STYLE_LANDUSE[g.cls] : 0xFFFF; break;
    case L_BUILDING: color = STYLE_BUILDING; break;
    default: return;
  }
  if (color == 0xFFFF) return;
  bool outline = g.layer == L_BUILDING && zoom >= STYLE_BUILDING_OUTLINE_FROM_ZOOM;
  const uint8_t* p = g.first;
  uint16_t ringLens[32];
  for (uint16_t i = 0; i < g.n_features; i++) {
    TileFeature f; if (!t.readFeature(p, f)) return;
    const uint8_t* q = f.rings;
    int total = 0, nr = 0;
    for (uint8_t r = 0; r < f.n_rings; r++) {
      TileRing ring; q = TileReader::readRing(q, ring);
      int n = transform(ring, tp, total);
      if (n >= 3 && nr < 32) { ringLens[nr++] = n; total += n; }
    }
    if (nr) {
      _r.fillPath(_pts, ringLens, nr, color);
      if (outline) {
        // close each ring by stroking it as a polyline back to its start
        int base = 0;
        for (int r = 0; r < nr; r++) {
          int n = ringLens[r];
          if (base + n < _cap) { _pts[base + n] = _pts[base]; _r.strokePolyline(_pts + base, n + 1, 0.8f, STYLE_BUILDING_OUTLINE, 255, false); }
          base += n;
        }
      }
    }
    p = t.skipFeature(f);
  }
}

void MapRenderer::strokeFeature(const TileFeature& f, const TilePlacement& tp, float w, uint16_t color, uint8_t alpha, uint8_t dashOn, uint8_t dashOff) {
  const uint8_t* q = f.rings;
  for (uint8_t r = 0; r < f.n_rings; r++) {
    TileRing ring; q = TileReader::readRing(q, ring);
    int n = transform(ring, tp, 0);
    if (n < 2) continue;
    if (dashOn) _r.strokeDashed(_pts, n, w, dashOn, dashOff, color, alpha);
    else        _r.strokePolyline(_pts, n, w, color, alpha, true);
  }
}

void MapRenderer::drawLineGroup(TileReader& t, const TileGroup& g, const TilePlacement& tp, float zoom, RenderPass pass) {
  const uint8_t* p = g.first;
  if (g.layer == L_ROAD) {
    if (g.cls >= sizeof(STYLE_ROAD) / sizeof(STYLE_ROAD[0])) return;
    const RoadStyle& s = STYLE_ROAD[g.cls];
    float w = style_width(s.w, s.n_w, zoom);
    bool casing = zoom >= s.casing_from_zoom && w >= 1.5f;
    if (pass == PASS_ROAD_CASING && !casing) return;
    for (uint16_t i = 0; i < g.n_features; i++) {
      TileFeature f; if (!t.readFeature(p, f)) return;
      uint8_t alpha = (f.flags & RF_TUNNEL) ? 110 : 255;
      if (pass == PASS_ROAD_CASING) strokeFeature(f, tp, w + (w < 4 ? 1.2f : 2.0f), s.casing, alpha, 0, 0);
      else                          strokeFeature(f, tp, w, s.fill, alpha, s.dash_on && w < 3 ? s.dash_on : 0, s.dash_off);
      p = t.skipFeature(f);
    }
    return;
  }
  const LineStyle* tbl; size_t n;
  if (g.layer == L_WATERWAY)     { tbl = STYLE_WATERWAY; n = sizeof(STYLE_WATERWAY) / sizeof(LineStyle); }
  else if (g.layer == L_RAIL)    { tbl = STYLE_RAIL;     n = sizeof(STYLE_RAIL) / sizeof(LineStyle); }
  else if (g.layer == L_AEROWAY) { tbl = STYLE_AEROWAY;  n = sizeof(STYLE_AEROWAY) / sizeof(LineStyle); }
  else return;
  if (g.cls >= n || tbl[g.cls].color == 0xFFFF) return;
  const LineStyle& s = tbl[g.cls];
  float w = style_width(s.w, s.n_w, zoom);
  for (uint16_t i = 0; i < g.n_features; i++) {
    TileFeature f; if (!t.readFeature(p, f)) return;
    strokeFeature(f, tp, w, s.color, 255, 0, 0);
    if (s.dash_on && s.dash_color != 0xFFFF && w >= 1.2f)      // railway "sleepers": lighter dashes on top
      strokeFeature(f, tp, w * 0.6f, s.dash_color, 255, s.dash_on, s.dash_off);
    p = t.skipFeature(f);
  }
}
