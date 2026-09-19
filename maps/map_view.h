#pragma once
// =============================================================================
//  map_view - viewport state, render orchestration and the LVGL map widget
// =============================================================================
//  Two RGB565 canvases (screen + margin) live in PSRAM. The render task fills
//  the back one for a requested (centre, zoom); loop() swaps it in, positions
//  and scales the lv_image so the map never waits for a render: dragging moves
//  the last rendered canvas, zooming scales it, and the fresh render replaces
//  it when ready.
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "raster.h"
#include "renderer.h"
#include "tile_store.h"

struct MapLabel { float x, y; uint8_t layer, cls; char text[40]; };

class MapView {
public:
  static const int CANVAS_W = 360, CANVAS_H = 444;     // 240x296 screen + 50 % margin
  static const int MAX_LABELS = 96;

  bool begin(lv_obj_t* parent, TileStore* store, int minzoom, int maxzoom);
  void setCenter(double lon, double lat, float zoom);   // also requests a render
  void zoomBy(float delta, int px = -1, int py = -1);   // around a screen point (default: centre)
  void panBy(int dx, int dy);                           // screen px
  void endPan();
  void flyTo(double lon, double lat, float zoom);       // animated
  void loop();                                          // call from loop(): swaps finished renders in
  float zoom() const { return _zoom; }
  void centerLonLat(double& lon, double& lat) const;
  uint32_t lastRenderMs = 0, lastTilesMissing = 0;
  bool busy() const { return _rendering; }

  // ---- render task side (public for the trampoline) ----
  void renderTaskLoop();

private:
  struct View { double mx, my; float zoom; };           // normalised mercator centre + zoom
  lv_obj_t* _img = nullptr;
  lv_obj_t* _labels[MAX_LABELS] = {};
  lv_image_dsc_t _dsc[2];
  uint16_t* _canvas[2] = {nullptr, nullptr};
  int _front = 0;
  Raster _raster;
  MapRenderer* _renderer = nullptr;
  TileStore* _store = nullptr;
  int _minzoom = 10, _maxzoom = 16;

  // UI-owned state
  double _mx = 0.5, _my = 0.5; float _zoom = 13;
  View _rendered;                                       // what the front canvas shows
  bool _haveRender = false;

  // request / result mailbox
  TaskHandle_t _task = nullptr;
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  View _req; bool _reqPending = false;
  volatile bool _rendering = false, _resultReady = false, _ackPending = false;
  View _resultView; int _resultBuf = 0; uint32_t _resultMs = 0, _resultMissing = 0, _resultFetchMs = 0;
  MapLabel _resultLabels[MAX_LABELS]; int _nResultLabels = 0;
  MapLabel _labelScratch[MAX_LABELS]; int _nLabelScratch = 0;

  void requestRender();
  void renderInto(int buf, const View& v, uint32_t& missing);
  void prefetchAround(const View& v);
  void placeImage();
  void showLabels(const MapLabel* labels, int n);
  static void labelSink(const LabelCandidate& c, void* user);
  double pxPerWorld(float zoom) const;                  // screen px per normalised-mercator unit
};
