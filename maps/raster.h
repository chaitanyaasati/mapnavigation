#pragma once
// =============================================================================
//  raster - tiny anti-aliased polygon rasteriser for RGB565 canvases
// =============================================================================
//  Why not ThorVG (LVGL's vector backend)? Measured on this ESP32-S3: ~1 ms and
//  ~8 KB of heap per stroked polyline. A map view has hundreds. This renderer
//  does only what maps need - nonzero-winding polygon fills and wide polylines
//  with round joins/caps - using signed-area coverage accumulation (the
//  font-rs / stb_truetype technique): exact-area AA, no per-shape heap use.
//
//  Coordinates are float canvas pixels; anything outside the canvas is clipped.
// =============================================================================
#include <stdint.h>
#include <stddef.h>

struct RPoint { float x, y; };

class Raster {
public:
  // canvas: w*h RGB565 pixels, row stride in pixels. Allocates scratch internally.
  bool init(uint16_t* canvas, int w, int h, int stride_px);
  void setCanvas(uint16_t* canvas) { _canvas = canvas; }
  void clear(uint16_t rgb565);

  // Closed polygon(s). rings[i] = point count of ring i; rings that wind the
  // opposite way cut holes (nonzero rule). Orientation must be consistent
  // across rings of one call for the union/hole behaviour to be right.
  void fillPath(const RPoint* pts, const uint16_t* rings, int nRings, uint16_t rgb565, uint8_t alpha = 255);

  // Open polyline stroked with the given width (px). Round joins and caps
  // when round==true (skipped automatically for hairlines).
  void strokePolyline(const RPoint* pts, int n, float width, uint16_t rgb565, uint8_t alpha = 255, bool round = true);

  // Same, dashed: on/off lengths in px (e.g. railways).
  void strokeDashed(const RPoint* pts, int n, float width, float on, float off, uint16_t rgb565, uint8_t alpha = 255);

  // stats for the last frame (reset with clear())
  uint32_t statEdges = 0, statPixels = 0, statShapes = 0;
  uint32_t cycBuild = 0, cycAcc = 0, cycBlend = 0;   // CPU cycles (device only)

private:
  struct Edge { float x0, y0, x1, y1; };

  uint16_t* _canvas = nullptr;
  int _w = 0, _h = 0, _stride = 0;
  float* _acc = nullptr;        // accumulation scratch, ACC_FLOATS entries, kept all-zero between shapes
  int16_t* _rowMin = nullptr;   // per band row: leftmost / rightmost column touched (blend + clear only that span)
  int16_t* _rowMax = nullptr;
  Edge*  _edges = nullptr;      // edge scratch for the shape being drawn
  int    _nEdges = 0, _edgeCap = 0;
  float  _bx0, _by0, _bx1, _by1; // bbox of pending edges
  uint32_t _tBuild = 0;

  void beginShape();
  void addEdge(float x0, float y0, float x1, float y1);     // clips to canvas x range
  void addEdgeClipped(float x0, float y0, float x1, float y1);
  void addQuad(RPoint a, RPoint b, float hw);
  void addDisc(RPoint c, float r);
  void endShape(uint16_t rgb565, uint8_t alpha);           // rasterise + blend
  void accumulateBand(int by0, int by1, int bx0, int bw);
  void blendBand(int by0, int by1, int bx0, int bw, uint16_t rgb565, uint8_t alpha);
};
