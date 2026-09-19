#pragma once
// =============================================================================
//  renderer - draws VT01 tiles onto a Raster canvas using style.h
// =============================================================================
//  Multi-tile canvases are drawn in passes so layering is right across tile
//  seams: all areas of all tiles first, then lines, then road casings, then
//  road fills. Labels are not drawn here; point/name candidates are handed to
//  a callback so the caller can place them with collision handling.
#include "raster.h"
#include "tile.h"

enum RenderPass : uint8_t { PASS_AREA, PASS_LINE, PASS_ROAD_CASING, PASS_ROAD_FILL, PASS_LABELS, PASS_COUNT };

// Where a tile lands on the canvas: canvas_px = local * scale + offset.
struct TilePlacement { float ox, oy, scale; };

struct LabelCandidate {
  float x, y;            // canvas px
  uint8_t layer, cls;    // L_PLACE / L_POI (+ named polygons later)
  const char* name;      // valid only during the callback
};
typedef void (*LabelSink)(const LabelCandidate&, void* user);

class MapRenderer {
public:
  explicit MapRenderer(Raster& r) : _r(r) {}
  bool init();                                       // allocates the point scratch
  void drawTile(const uint8_t* data, size_t len, const TilePlacement& tp, float zoom, RenderPass pass,
                LabelSink sink = nullptr, void* user = nullptr);

private:
  Raster& _r;
  RPoint* _pts = nullptr;      // scratch for one feature's transformed points
  int _cap = 0;
  int transform(const TileRing& ring, const TilePlacement& tp, int base);   // returns count appended
  void drawAreaGroup(TileReader& t, const TileGroup& g, const TilePlacement& tp, float zoom);
  void drawLineGroup(TileReader& t, const TileGroup& g, const TilePlacement& tp, float zoom, RenderPass pass);
  void strokeFeature(const TileFeature& f, const TilePlacement& tp, float w, uint16_t color, uint8_t alpha, uint8_t dashOn, uint8_t dashOff);
};
