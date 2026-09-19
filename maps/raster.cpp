#include "raster.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <esp_heap_caps.h>
#ifdef ARDUINO
#include <esp_cpu.h>
static inline uint32_t cyc() { return esp_cpu_get_cycle_count(); }
#else
static inline uint32_t cyc() { return 0; }
#endif

// Scratch: 16K floats (64 KB) of internal RAM. A shape taller than the band
// that fits is processed in several bands; rows are independent because every
// row of a closed, clipped shape sums to zero coverage past its right edge.
static const int ACC_FLOATS = 8192;     // 32 KB internal; TLS for voice needs the headroom
static const int EDGE_CAP   = 12000;
static const int MAX_BAND_ROWS = 512;

bool Raster::init(uint16_t* canvas, int w, int h, int stride_px) {
  _canvas = canvas; _w = w; _h = h; _stride = stride_px;
  if (!_acc)   _acc   = (float*)heap_caps_malloc(ACC_FLOATS * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!_edges) _edges = (Edge*)heap_caps_malloc(EDGE_CAP * sizeof(Edge), MALLOC_CAP_SPIRAM);
  if (!_rowMin) _rowMin = (int16_t*)heap_caps_malloc(MAX_BAND_ROWS * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!_rowMax) _rowMax = (int16_t*)heap_caps_malloc(MAX_BAND_ROWS * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  _edgeCap = EDGE_CAP;
  if (_acc) memset(_acc, 0, ACC_FLOATS * sizeof(float));
  return _acc && _edges && _rowMin && _rowMax;
}

void Raster::clear(uint16_t c) {
  for (int y = 0; y < _h; y++) {
    uint16_t* row = _canvas + y * _stride;
    for (int x = 0; x < _w; x++) row[x] = c;
  }
  statEdges = statPixels = statShapes = 0;
  cycBuild = cycAcc = cycBlend = 0;
}

// ---- shape assembly ------------------------------------------------------------
void Raster::beginShape() {
  _tBuild = cyc();
  _nEdges = 0;
  _bx0 = _by0 = 1e9f; _bx1 = _by1 = -1e9f;
}

void Raster::addEdgeClipped(float x0, float y0, float x1, float y1) {
  if (_nEdges >= _edgeCap) return;
  if (y0 == y1) return;                                   // horizontal edges contribute nothing
  Edge& e = _edges[_nEdges++];
  e.x0 = x0; e.y0 = y0; e.x1 = x1; e.y1 = y1;
  float lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
  if (lo < _by0) _by0 = lo;  if (hi > _by1) _by1 = hi;
  float xl = x0 < x1 ? x0 : x1, xh = x0 < x1 ? x1 : x0;
  if (xl < _bx0) _bx0 = xl;  if (xh > _bx1) _bx1 = xh;
}

// Split the edge at x = 0 and x = w, then clamp the outside pieces onto the
// boundary (they become vertical, which keeps their winding contribution).
void Raster::addEdge(float x0, float y0, float x1, float y1) {
  if (y0 == y1) return;
  const float W = (float)_w;
  // trivial reject in y (rows outside the canvas never get rasterised anyway)
  if ((y0 < 0 && y1 < 0) || (y0 > _h && y1 > _h)) return;
  float xs[4] = {x0}, ys[4] = {y0}; int n = 1;
  auto split_at = [&](float xb) {
    if ((x0 < xb && x1 > xb) || (x0 > xb && x1 < xb)) {
      float t = (xb - x0) / (x1 - x0);
      xs[n] = xb; ys[n] = y0 + t * (y1 - y0); n++;
    }
  };
  if (x0 < x1) { split_at(0); split_at(W); } else { split_at(W); split_at(0); }
  xs[n] = x1; ys[n] = y1; n++;
  for (int i = 0; i + 1 < n; i++) {
    float ax = xs[i], ay = ys[i], bx = xs[i + 1], by = ys[i + 1];
    float mid = 0.5f * (ax + bx);
    if (mid < 0)      { ax = 0; bx = 0; }
    else if (mid > W) { ax = W; bx = W; }
    addEdgeClipped(ax, ay, bx, by);
  }
}

void Raster::addQuad(RPoint a, RPoint b, float hw) {
  float dx = b.x - a.x, dy = b.y - a.y;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1e-4f) return;
  float nx = -dy / len * hw, ny = dx / len * hw;
  RPoint p0 = {a.x + nx, a.y + ny}, p1 = {b.x + nx, b.y + ny};
  RPoint p2 = {b.x - nx, b.y - ny}, p3 = {a.x - nx, a.y - ny};
  addEdge(p0.x, p0.y, p1.x, p1.y);
  addEdge(p1.x, p1.y, p2.x, p2.y);
  addEdge(p2.x, p2.y, p3.x, p3.y);
  addEdge(p3.x, p3.y, p0.x, p0.y);
}

// Unit circles, clockwise (matches the quads' winding), so discs need no trig.
static const float CIRC8[8][2]  = {{1,0},{0.7071f,-0.7071f},{0,-1},{-0.7071f,-0.7071f},{-1,0},{-0.7071f,0.7071f},{0,1},{0.7071f,0.7071f}};
static const float CIRC12[12][2] = {{1,0},{0.8660f,-0.5f},{0.5f,-0.8660f},{0,-1},{-0.5f,-0.8660f},{-0.8660f,-0.5f},{-1,0},{-0.8660f,0.5f},{-0.5f,0.8660f},{0,1},{0.5f,0.8660f},{0.8660f,0.5f}};
static const float CIRC20[20][2] = {{1,0},{0.9511f,-0.3090f},{0.8090f,-0.5878f},{0.5878f,-0.8090f},{0.3090f,-0.9511f},{0,-1},{-0.3090f,-0.9511f},{-0.5878f,-0.8090f},{-0.8090f,-0.5878f},{-0.9511f,-0.3090f},{-1,0},{-0.9511f,0.3090f},{-0.8090f,0.5878f},{-0.5878f,0.8090f},{-0.3090f,0.9511f},{0,1},{0.3090f,0.9511f},{0.5878f,0.8090f},{0.8090f,0.5878f},{0.9511f,0.3090f}};

// Disc wound the same way as addQuad's quads so they union under the clamp.
void Raster::addDisc(RPoint c, float r) {
  const float (*tab)[2]; int n;
  if (r < 3) { tab = CIRC8; n = 8; } else if (r < 8) { tab = CIRC12; n = 12; } else { tab = CIRC20; n = 20; }
  float px = c.x + r, py = c.y;
  for (int i = 1; i <= n; i++) {
    int k = i % n;
    float x = c.x + r * tab[k][0], y = c.y + r * tab[k][1];
    addEdge(px, py, x, y);
    px = x; py = y;
  }
}

// ---- public shapes -------------------------------------------------------------
void Raster::fillPath(const RPoint* pts, const uint16_t* rings, int nRings, uint16_t c, uint8_t alpha) {
  beginShape();
  int base = 0;
  for (int r = 0; r < nRings; r++) {
    int n = rings[r];
    for (int i = 0; i < n; i++) {
      const RPoint& a = pts[base + i];
      const RPoint& b = pts[base + (i + 1) % n];
      addEdge(a.x, a.y, b.x, b.y);
    }
    base += n;
  }
  endShape(c, alpha);
}

void Raster::strokePolyline(const RPoint* pts, int n, float width, uint16_t c, uint8_t alpha, bool round) {
  if (n < 2) return;
  float hw = width * 0.5f;
  if (hw < 0.35f) hw = 0.35f;                 // hairlines still get ~0.7 px of coverage
  bool discs = round && width >= 1.8f;
  beginShape();
  for (int i = 0; i + 1 < n; i++) addQuad(pts[i], pts[i + 1], hw);
  if (discs) {
    addDisc(pts[0], hw);
    addDisc(pts[n - 1], hw);
    // interior joints only need a disc when the turn leaves a visible notch:
    // the gap between consecutive quads is ~hw*|sin(turn)|, skip below ~0.4 px
    for (int i = 1; i + 1 < n; i++) {
      float ax = pts[i].x - pts[i - 1].x, ay = pts[i].y - pts[i - 1].y;
      float bx = pts[i + 1].x - pts[i].x, by = pts[i + 1].y - pts[i].y;
      float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
      if (la < 1e-4f || lb < 1e-4f) continue;
      float sinTurn = fabsf(ax * by - ay * bx) / (la * lb);
      float cosTurn = (ax * bx + ay * by) / (la * lb);
      if (cosTurn < 0 || sinTurn * hw > 0.4f) addDisc(pts[i], hw);
    }
  }
  endShape(c, alpha);
}

void Raster::strokeDashed(const RPoint* pts, int n, float width, float on, float off, uint16_t c, uint8_t alpha) {
  if (n < 2) return;
  float hw = width * 0.5f, period = on + off, t = 0;   // t = distance into the current period
  beginShape();
  for (int i = 0; i + 1 < n; i++) {
    RPoint a = pts[i], b = pts[i + 1];
    float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) continue;
    float ux = dx / len, uy = dy / len, pos = 0;
    while (pos < len) {
      float remainOn = (t < on) ? on - t : 0;
      if (remainOn > 0) {
        float seg = remainOn < len - pos ? remainOn : len - pos;
        RPoint s = {a.x + ux * pos, a.y + uy * pos}, e = {a.x + ux * (pos + seg), a.y + uy * (pos + seg)};
        addQuad(s, e, hw);
        pos += seg; t += seg;
      } else {
        float remainOff = period - t;
        float seg = remainOff < len - pos ? remainOff : len - pos;
        pos += seg; t += seg;
        if (t >= period) t -= period;
      }
    }
  }
  endShape(c, alpha);
}

// ---- rasterisation -------------------------------------------------------------
void Raster::endShape(uint16_t c, uint8_t alpha) {
  cycBuild += cyc() - _tBuild;
  statShapes++;
  if (_nEdges == 0) return;
  int bx0 = (int)floorf(_bx0), bx1 = (int)ceilf(_bx1) + 1;   // +1: accumulation spills one column right
  int by0 = (int)floorf(_by0), by1 = (int)ceilf(_by1);
  if (bx0 < 0) bx0 = 0;  if (bx1 > _w + 1) bx1 = _w + 1;
  if (by0 < 0) by0 = 0;  if (by1 > _h) by1 = _h;
  if (bx1 <= bx0 || by1 <= by0) return;
  int bw = bx1 - bx0 + 1;                                     // one more spare column for the far spill
  int rowsPerBand = ACC_FLOATS / bw;
  if (rowsPerBand < 1) return;
  if (rowsPerBand > MAX_BAND_ROWS) rowsPerBand = MAX_BAND_ROWS;
  statEdges += _nEdges;
  for (int y = by0; y < by1; y += rowsPerBand) {
    int ye = y + rowsPerBand < by1 ? y + rowsPerBand : by1;
    for (int r = 0; r < ye - y; r++) { _rowMin[r] = 0x7FFF; _rowMax[r] = -1; }
    uint32_t t0 = cyc();
    accumulateBand(y, ye, bx0, bw);
    uint32_t t1 = cyc();
    blendBand(y, ye, bx0, bw, c, alpha);     // also re-zeroes the touched spans
    cycAcc += t1 - t0; cycBlend += cyc() - t1;
  }
}

// font-rs draw_line, restricted to rows [by0, by1) and shifted by bx0.
void Raster::accumulateBand(int by0, int by1, int bx0, int bw) {
  const int H = by1 - by0;
  float* acc = _acc;
  for (int ei = 0; ei < _nEdges; ei++) {
    const Edge& e = _edges[ei];
    float dir, px0, py0, px1, py1;
    if (e.y0 < e.y1) { dir = 1;  px0 = e.x0; py0 = e.y0; px1 = e.x1; py1 = e.y1; }
    else             { dir = -1; px0 = e.x1; py0 = e.y1; px1 = e.x0; py1 = e.y0; }
    // to band-local coordinates
    px0 -= bx0; px1 -= bx0; py0 -= by0; py1 -= by0;
    if (py1 <= 0 || py0 >= H) continue;
    float dxdy = (px1 - px0) / (py1 - py0);
    float x = px0;
    int y0 = py0 > 0 ? (int)py0 : 0;
    if (py0 < 0) x -= py0 * dxdy;
    int yEnd = (int)py1; if ((float)yEnd != py1) yEnd++; if (yEnd > H) yEnd = H;
    for (int y = y0; y < yEnd; y++) {
      float* line = acc + y * bw;
      float top = (float)y > py0 ? (float)y : py0;
      float bot = (float)(y + 1) < py1 ? (float)(y + 1) : py1;
      float dy = bot - top;
      float xnext = x + dxdy * dy;
      float d = dy * dir;
      float x0 = x < xnext ? x : xnext, x1 = x < xnext ? xnext : x;
      int x0i = (int)x0; float x0floor = (float)x0i;                 // x >= 0: truncation is floor
      int x1i = (int)x1; if ((float)x1i != x1) x1i++; float x1ceil = (float)x1i;
      if (x0i < 0) { x0i = 0; x0floor = 0; }             // safety; edges are pre-clipped
      if (x1i > bw - 1) x1i = bw - 1;
      if (x0i < _rowMin[y]) _rowMin[y] = x0i;
      if (x1i + 1 > _rowMax[y]) _rowMax[y] = x1i + 1;
      if (x1i <= x0i + 1) {
        float xmf = 0.5f * (x + xnext) - x0floor;
        line[x0i]     += d - d * xmf;
        line[x0i + 1] += d * xmf;
      } else {
        float s = 1.0f / (x1 - x0);
        float x0f = x0 - x0floor;
        float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
        float x1f = x1 - x1ceil + 1.0f;
        float am = 0.5f * s * x1f * x1f;
        line[x0i] += d * a0;
        if (x1i == x0i + 2) {
          line[x0i + 1] += d * (1.0f - a0 - am);
        } else {
          float a1 = s * (1.5f - x0f);
          line[x0i + 1] += d * (a1 - a0);
          for (int xi = x0i + 2; xi < x1i - 1; xi++) line[xi] += d * s;
          float a2 = a1 + (float)(x1i - x0i - 3) * s;
          line[x1i - 1] += d * (1.0f - a2 - am);
        }
        line[x1i] += d * am;
      }
      x = xnext;
    }
  }
}

void Raster::blendBand(int by0, int by1, int bx0, int bw, uint16_t c, uint8_t alpha) {
  const int sr = (c >> 11) & 0x1F, sg = (c >> 5) & 0x3F, sb = c & 0x1F;
  const float aScale256 = alpha * (256.0f / 255.0f);
  int xEnd = bx0 + bw - 1; if (xEnd > _w) xEnd = _w;            // spare column is never a pixel
  const int visible = xEnd - bx0;
  for (int y = by0; y < by1; y++) {
    int r = y - by0;
    if (_rowMax[r] < 0) continue;                                 // nothing touched this row
    float* line = _acc + r * bw;
    uint16_t* row = _canvas + y * _stride + bx0;
    int xs = _rowMin[r], xe = _rowMax[r] < bw - 1 ? _rowMax[r] : bw - 1;
    // zero the touched span for the next shape (the buffer is kept clean between shapes)
    int xeVis = xe < visible ? xe : visible;
    float acc = 0;
    for (int x = xs; x <= xe; x++) {
      if (x >= xeVis) { line[x] = 0; continue; }
      acc += line[x];
      line[x] = 0;
      float cov = acc < 0 ? -acc : acc;
      if (cov < 0.004f) continue;
      int a = (int)(cov * aScale256);
      if (a >= 255) { row[x] = c; statPixels++; continue; }
      uint16_t d = row[x];
      int dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
      dr += ((sr - dr) * a) >> 8;
      dg += ((sg - dg) * a) >> 8;
      db += ((sb - db) * a) >> 8;
      row[x] = (uint16_t)((dr << 11) | (dg << 5) | db);
      statPixels++;
    }
  }
}
