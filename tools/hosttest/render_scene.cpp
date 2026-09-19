// Renders the phase-1 benchmark scene with the device rasteriser on the host
// and writes scene.ppm, so AA quality can be inspected without the board.
#include "../../maps/raster.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <chrono>
static const int CW = 240, CH = 296;
static uint32_t seedr = 99;
static uint32_t rnd(uint32_t n) { seedr = seedr * 1103515245u + 12345u; return (seedr >> 8) % n; }
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
int main() {
  static uint16_t canvas[CW * CH];
  Raster raster; raster.init(canvas, CW, CH, CW);
  auto t0 = std::chrono::steady_clock::now();
  raster.clear(rgb(0xF2, 0xEF, 0xE9));
  RPoint pts[64];
  for (int i = 0; i < 100; i++) {
    int n = 4 + rnd(5); float cx = rnd(CW), cy = rnd(CH), r = 6 + rnd(24);
    for (int k = 0; k < n; k++) { float ang = 6.2831853f * k / n; float rr = r * (0.7f + 0.3f * (rnd(100) / 100.0f)); pts[k] = {cx + cosf(ang) * rr, cy + sinf(ang) * rr}; }
    uint16_t ring = n;
    raster.fillPath(pts, &ring, 1, (i % 3 == 0) ? rgb(0xC8, 0xE6, 0xC0) : rgb(0xD9, 0xD0, 0xC9));
  }
  for (int pass = 0; pass < 2; pass++) {
    uint32_t seed = 12345;
    for (int i = 0; i < 300; i++) {
      seed = seed * 1103515245u + 12345u;
      float x = (seed >> 8) % CW, y = (seed >> 20) % CH; pts[0] = {x, y};
      int n = 3 + (seed % 5);
      for (int k = 0; k < n; k++) { seed = seed * 1103515245u + 12345u; x += (int)((seed >> 8) % 40) - 20; y += (int)((seed >> 20) % 40) - 20; pts[k + 1] = {x, y}; }
      bool major = (i % 10) == 0; float w = major ? 5.0f : 2.5f;
      if (pass == 0) raster.strokePolyline(pts, n + 1, w + 1.5f, rgb(0x9A, 0x8F, 0x85));
      else if (major) raster.strokePolyline(pts, n + 1, w, rgb(0xFF, 0xD7, 0x6E));
      else raster.strokePolyline(pts, n + 1, w, rgb(0xFF, 0xFF, 0xFF));
    }
  }
  // a few deliberate test shapes: hairline, thick diagonal, dashed, polygon with hole
  RPoint hair[2] = {{10, 280}, {230, 260}}; raster.strokePolyline(hair, 2, 1.0f, rgb(0, 0, 0));
  RPoint thick[3] = {{20, 20}, {120, 60}, {60, 120}}; raster.strokePolyline(thick, 3, 9.0f, rgb(0x30, 0x60, 0xC0));
  RPoint dash[2] = {{10, 240}, {230, 200}}; raster.strokeDashed(dash, 2, 3.0f, 8, 5, rgb(0x40, 0x40, 0x40));
  RPoint ring[8] = {{150, 150}, {220, 150}, {220, 220}, {150, 220}, {170, 170}, {170, 200}, {200, 200}, {200, 170}}; // outer CW-ish, inner reversed
  uint16_t rings[2] = {4, 4};
  raster.fillPath(ring, rings, 2, rgb(0x50, 0xA0, 0xFF), 200);
  auto t1 = std::chrono::steady_clock::now();
  fprintf(stderr, "host render %.1f ms, %u edges, %u px\n", std::chrono::duration<double, std::milli>(t1 - t0).count(), raster.statEdges, raster.statPixels);
  FILE* f = fopen("scene.ppm", "wb"); fprintf(f, "P6\n%d %d\n255\n", CW, CH);
  for (int i = 0; i < CW * CH; i++) { uint16_t c = canvas[i]; unsigned char px[3] = {(unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31)}; fwrite(px, 1, 3, f); }
  fclose(f); return 0;
}
