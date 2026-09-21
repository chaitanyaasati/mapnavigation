#include "map_view.h"
#include "style.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

static void renderTaskTrampoline(void* p) { ((MapView*)p)->renderTaskLoop(); }

static inline void lonlat_to_merc(double lon, double lat, double& mx, double& my) {
  mx = (lon + 180.0) / 360.0;
  double s = sin(lat * M_PI / 180.0);
  my = 0.5 - log((1 + s) / (1 - s)) / (4 * M_PI);
}
static inline void merc_to_lonlat(double mx, double my, double& lon, double& lat) {
  lon = mx * 360.0 - 180.0;
  lat = atan(sinh(M_PI * (1 - 2 * my))) * 180.0 / M_PI;
}

double MapView::pxPerWorld(float zoom) const { return 256.0 * pow(2.0, (double)zoom); }

bool MapView::begin(lv_obj_t* parent, TileStore* store, int minzoom, int maxzoom) {
  _store = store; _minzoom = minzoom; _maxzoom = maxzoom;
  for (int i = 0; i < 2; i++) {
    _canvas[i] = (uint16_t*)heap_caps_aligned_alloc(16, CANVAS_W * CANVAS_H * 2, MALLOC_CAP_SPIRAM);
    if (!_canvas[i]) return false;
    for (int p = 0; p < CANVAS_W * CANVAS_H; p++) _canvas[i][p] = STYLE_BG;
    _dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
    _dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
    _dsc[i].header.w = CANVAS_W; _dsc[i].header.h = CANVAS_H;
    _dsc[i].header.stride = CANVAS_W * 2;
    _dsc[i].data_size = CANVAS_W * CANVAS_H * 2;
    _dsc[i].data = (const uint8_t*)_canvas[i];
  }
  if (!_raster.init(_canvas[1], CANVAS_W, CANVAS_H, CANVAS_W)) return false;
  _renderer = new MapRenderer(_raster);
  if (!_renderer->init()) return false;

  _img = lv_image_create(parent);
  lv_image_set_src(_img, &_dsc[0]);
  lv_image_set_pivot(_img, CANVAS_W / 2, CANVAS_H / 2);
  lv_obj_set_pos(_img, (lv_obj_get_width(parent) - CANVAS_W) / 2, (lv_obj_get_height(parent) - CANVAS_H) / 2);
  lv_obj_set_clickable(_img, false);
  _rendered = {_mx, _my, _zoom};

  xTaskCreatePinnedToCore(renderTaskTrampoline, "render", 20 * 1024, this, 1, &_task, 0);
  return true;
}

void MapView::setCenter(double lon, double lat, float zoom) {
  lonlat_to_merc(lon, lat, _mx, _my);
  _zoom = constrain(zoom, (float)_minzoom, (float)_maxzoom + 1.5f);
  placeImage();
  requestRender();
}

void MapView::centerLonLat(double& lon, double& lat) const { merc_to_lonlat(_mx, _my, lon, lat); }

void MapView::panBy(int dx, int dy) {
  double ppw = pxPerWorld(_zoom);
  _mx -= dx / ppw; _my -= dy / ppw;
  placeImage();
  // re-render early if the view has left the rendered margin
  double offx = (_mx - _rendered.mx) * ppw, offy = (_my - _rendered.my) * ppw;
  if (fabs(offx) > (CANVAS_W - LV_HOR_RES) / 2 - 8 || fabs(offy) > (CANVAS_H - LV_VER_RES) / 2 - 8) requestRender();
}

void MapView::endPan() { requestRender(); }

void MapView::zoomBy(float delta, int px, int py) {
  float nz = constrain(_zoom + delta, (float)_minzoom, (float)_maxzoom + 1.5f);
  if (nz == _zoom) return;
  if (px < 0) { px = LV_HOR_RES / 2; py = LV_VER_RES / 2; }
  // keep the world point under (px,py) fixed
  double ppw0 = pxPerWorld(_zoom), ppw1 = pxPerWorld(nz);
  double wx = _mx + (px - LV_HOR_RES / 2) / ppw0, wy = _my + (py - LV_VER_RES / 2) / ppw0;
  _zoom = nz;
  _mx = wx - (px - LV_HOR_RES / 2) / ppw1; _my = wy - (py - LV_VER_RES / 2) / ppw1;
  placeImage();
  requestRender();
}

void MapView::flyTo(double lon, double lat, float zoom) {
  // v1: jump. (An lv_anim over centre/zoom can be layered on later.)
  setCenter(lon, lat, zoom);
}

// Position + scale the front canvas so the rendered centre lands where the
// current view says it should be.
void MapView::placeImage() {
  double ppw = pxPerWorld(_zoom);
  double sx = LV_HOR_RES / 2.0 - (_mx - _rendered.mx) * ppw;   // screen pos of the rendered centre
  double sy = LV_VER_RES / 2.0 - (_my - _rendered.my) * ppw;
  float scale = powf(2.0f, _zoom - _rendered.zoom);
  lv_obj_set_pos(_img, (int)lroundf(sx - CANVAS_W / 2.0f), (int)lroundf(sy - CANVAS_H / 2.0f));
  lv_image_set_scale(_img, (uint32_t)(256 * scale));
}

void MapView::requestRender() {
  taskENTER_CRITICAL(&_mux);
  _req = {_mx, _my, _zoom};
  _reqPending = true;
  taskEXIT_CRITICAL(&_mux);
  if (_task) xTaskNotifyGive(_task);
}

// ---- render task -----------------------------------------------------------------
void MapView::labelSink(const LabelCandidate& c, void* user) {
  MapView* self = (MapView*)user;
  if (self->_nLabelScratch >= MAX_LABELS) return;
  if (c.x < 0 || c.y < 0 || c.x >= CANVAS_W || c.y >= CANVAS_H) return;
  MapLabel& l = self->_labelScratch[self->_nLabelScratch++];
  l.x = c.x; l.y = c.y; l.layer = c.layer; l.cls = c.cls;
  strncpy(l.text, c.name, sizeof l.text - 1); l.text[sizeof l.text - 1] = 0;
}

void MapView::renderInto(int buf, const View& v, uint32_t& missing) {
  _raster.setCanvas(_canvas[buf]);
  _raster.clear(STYLE_BG);
  int zi = (int)floorf(v.zoom);
  if (zi > _maxzoom) zi = _maxzoom;
  if (zi < _minzoom) zi = _minzoom;
  double n = pow(2.0, zi);
  double tilePx = 256.0 * pow(2.0, v.zoom - zi);
  double cx = v.mx * n, cy = v.my * n;
  int tx0 = (int)floor(cx - (CANVAS_W / 2.0) / tilePx), tx1 = (int)floor(cx + (CANVAS_W / 2.0) / tilePx);
  int ty0 = (int)floor(cy - (CANVAS_H / 2.0) / tilePx), ty1 = (int)floor(cy + (CANVAS_H / 2.0) / tilePx);

  struct T { TileData d; TilePlacement tp; } tiles[16];
  int nt = 0; missing = 0;
  for (int tx = tx0; tx <= tx1 && nt < 16; tx++)
    for (int ty = ty0; ty <= ty1 && nt < 16; ty++) {
      if (tx < 0 || ty < 0 || tx >= n || ty >= n) continue;
      TileData d;
      if (!_store->get(zi, tx, ty, d)) { missing++; continue; }
      if (!d.data) continue;
      tiles[nt].d = d;
      tiles[nt].tp.ox = (float)((tx - cx) * tilePx + CANVAS_W / 2.0);
      tiles[nt].tp.oy = (float)((ty - cy) * tilePx + CANVAS_H / 2.0);
      tiles[nt].tp.scale = (float)(tilePx / TILE_EXTENT);
      nt++;
    }
  for (int pass = PASS_AREA; pass < PASS_LABELS; pass++)
    for (int i = 0; i < nt; i++) _renderer->drawTile(tiles[i].d.data, tiles[i].d.len, tiles[i].tp, v.zoom, (RenderPass)pass);
  _nLabelScratch = 0;
  for (int i = 0; i < nt; i++) _renderer->drawTile(tiles[i].d.data, tiles[i].d.len, tiles[i].tp, v.zoom, PASS_LABELS, labelSink, this);
}

// While idle, pull in the ring of tiles just outside the canvas (and the parent
// zoom's tile) so a pan or zoom-out finds them cached. Stops as soon as a new
// render is requested.
void MapView::prefetchAround(const View& v) {
  int zi = (int)floorf(v.zoom); if (zi > _maxzoom) zi = _maxzoom; if (zi < _minzoom) zi = _minzoom;
  double n = pow(2.0, zi), tilePx = 256.0 * pow(2.0, v.zoom - zi);
  double cx = v.mx * n, cy = v.my * n;
  int tx0 = (int)floor(cx - (CANVAS_W / 2.0) / tilePx) - 1, tx1 = (int)floor(cx + (CANVAS_W / 2.0) / tilePx) + 1;
  int ty0 = (int)floor(cy - (CANVAS_H / 2.0) / tilePx) - 1, ty1 = (int)floor(cy + (CANVAS_H / 2.0) / tilePx) + 1;
  TileData d;
  for (int ty = ty0; ty <= ty1; ty++)
    for (int tx = tx0; tx <= tx1; tx++) {
      if (_reqPending) return;
      if (tx < 0 || ty < 0 || tx >= n || ty >= n) continue;
      if ((tx == tx0 || tx == tx1 || ty == ty0 || ty == ty1) && !_store->has(zi, tx, ty)) _store->get(zi, tx, ty, d);
    }
  if (zi > _minzoom && !_reqPending) {
    int px = (int)(cx / 2), py = (int)(cy / 2);
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
      if (!_reqPending && px + dx >= 0 && py + dy >= 0 && !_store->has(zi - 1, px + dx, py + dy)) _store->get(zi - 1, px + dx, py + dy, d);
  }
}

void MapView::renderTaskLoop() {
  for (;;) {
    while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) _store->idleTick();
    for (;;) {
      taskENTER_CRITICAL(&_mux);
      bool pending = _reqPending; View v = _req; _reqPending = false;
      taskEXIT_CRITICAL(&_mux);
      if (!pending) break;
      while (_ackPending) vTaskDelay(1);          // UI has not swapped the previous result in yet
      _rendering = true;
      int buf = 1 - _front;
      uint32_t t0 = millis(), missing, f0 = _store->statFetchMs;
      renderInto(buf, v, missing);
      _resultView = v; _resultBuf = buf; _resultMs = millis() - t0; _resultMissing = missing; _resultFetchMs = _store->statFetchMs - f0;
      memcpy(_resultLabels, _labelScratch, sizeof(MapLabel) * _nLabelScratch);
      _nResultLabels = _nLabelScratch;
      _rendering = false;
      _ackPending = true;
      _resultReady = true;
      if (!missing && !_reqPending) prefetchAround(v);
      if (missing) {                              // tiles were still loading: try again, backing off while offline
        static uint32_t backoff = 250;
        backoff = (_store->statFetches == 0 && _store->statFails > 8) ? min<uint32_t>(backoff * 2, 5000) : 250;
        vTaskDelay(pdMS_TO_TICKS(backoff));
        taskENTER_CRITICAL(&_mux);
        if (!_reqPending) { _req = v; _reqPending = true; }
        taskEXIT_CRITICAL(&_mux);
      }
    }
  }
}

// ---- UI thread -------------------------------------------------------------------
void MapView::loop() {
  if (!_resultReady) return;
  _resultReady = false;
  _front = _resultBuf;
  _rendered = _resultView;
  _haveRender = true;
  lastRenderMs = _resultMs; lastTilesMissing = _resultMissing;
  Serial.printf("[render] z%.1f %lu ms (fetch %lu)%s, %d labels, heap %u KB | %lu shapes %lu edges %lu px | build %lu acc %lu blend %lu ms\n",
                _rendered.zoom, (unsigned long)_resultMs, (unsigned long)_resultFetchMs, _resultMissing ? " (tiles pending)" : "", _nResultLabels, ESP.getFreeHeap() >> 10,
                (unsigned long)_raster.statShapes, (unsigned long)_raster.statEdges, (unsigned long)_raster.statPixels,
                (unsigned long)(_raster.cycBuild / 240000), (unsigned long)(_raster.cycAcc / 240000), (unsigned long)(_raster.cycBlend / 240000));
  lv_image_set_src(_img, &_dsc[_front]);
  placeImage();
  showLabels(_resultLabels, _nResultLabels);
  _ackPending = false;
}

void MapView::showLabels(const MapLabel* labels, int n) {
  // greedy placement: bigger places first, skip anything overlapping an earlier label
  struct Box { int x0, y0, x1, y1; } placed[MAX_LABELS]; int np = 0;
  int used = 0;
  for (int prio = 0; prio < 2 && used < MAX_LABELS; prio++) {
    for (int i = 0; i < n && used < MAX_LABELS; i++) {
      const MapLabel& l = labels[i];
      bool isPlace = l.layer == L_PLACE;
      if ((prio == 0) != isPlace) continue;
      if (!isPlace && _rendered.zoom < 14.5f) continue;            // POIs only when zoomed in
      const lv_font_t* font = &lv_font_montserrat_12;
      uint16_t color = STYLE_POI.color;
      if (isPlace) {
        uint8_t sz = STYLE_PLACE[l.cls < 5 ? l.cls : 4].size;
        font = sz >= 16 ? &lv_font_montserrat_16 : (sz >= 14 ? &lv_font_montserrat_14 : &lv_font_montserrat_12);
        color = STYLE_PLACE[l.cls < 5 ? l.cls : 4].color;
      }
      lv_point_t sz; lv_text_get_size(&sz, l.text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      int w = sz.x + 6, h = sz.y + 2;
      Box b = {(int)l.x - w / 2, (int)l.y - h / 2, (int)l.x + w / 2, (int)l.y + h / 2};
      if (!isPlace) { b.x0 = (int)l.x + 6; b.x1 = b.x0 + w; }      // POI text to the right of the point
      bool clash = false;
      for (int k = 0; k < np; k++) if (b.x0 < placed[k].x1 + 4 && b.x1 > placed[k].x0 - 4 && b.y0 < placed[k].y1 + 2 && b.y1 > placed[k].y0 - 2) { clash = true; break; }
      if (clash) continue;
      placed[np++] = b;
      lv_obj_t* lab = _labels[used];
      if (!lab) {
        lab = _labels[used] = lv_label_create(_img);
        lv_obj_set_style_pad_hor(lab, 3, 0);
        lv_obj_set_style_pad_ver(lab, 1, 0);
        lv_obj_set_style_radius(lab, 3, 0);
        lv_obj_set_style_bg_opa(lab, LV_OPA_70, 0);
        lv_obj_set_style_bg_color(lab, lv_color_hex(0xF2EFE9), 0);
      }
      lv_label_set_text(lab, l.text);
      lv_obj_set_style_text_font(lab, font, 0);
      lv_obj_set_style_text_color(lab, lv_color_make((color >> 8) & 0xF8, (color >> 3) & 0xFC, (color << 3) & 0xF8), 0);
      lv_obj_set_pos(lab, b.x0, b.y0);
      lv_obj_set_hidden(lab, false);
      used++;
    }
  }
  for (int i = used; i < MAX_LABELS; i++) if (_labels[i]) lv_obj_set_hidden(_labels[i], true);
}
