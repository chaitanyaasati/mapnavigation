// Host harness: render a lat/lon view from a tile directory with the exact
// device code (tile.h + renderer.cpp + raster.cpp) and write a PPM.
//   render_tile <tiles_dir> <lon> <lat> <zoom> <w> <h> <out.ppm>
#include "../../maps/raster.h"
#include "../../maps/renderer.h"
#include "../../maps/style.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <chrono>

static bool loadFile(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  out.resize(n); fread(out.data(), 1, n, f); fclose(f); return true;
}

int main(int argc, char** argv) {
  if (argc < 8) { fprintf(stderr, "usage: %s tiles_dir lon lat zoom w h out.ppm\n", argv[0]); return 1; }
  std::string dir = argv[1]; double lon = atof(argv[2]), lat = atof(argv[3]), zoom = atof(argv[4]);
  int W = atoi(argv[5]), H = atoi(argv[6]);
  std::vector<uint16_t> canvas(W * H);
  Raster raster; raster.init(canvas.data(), W, H, W);
  MapRenderer renderer(raster); renderer.init();

  int zi = (int)floor(zoom); if (zi > 16) zi = 16;
  double n = pow(2.0, zi);
  double tilePx = 256.0 * pow(2.0, zoom - zi);            // screen px per tile at this fractional zoom
  double cx = (lon + 180.0) / 360.0 * n;                    // tile coords of the view centre
  double s = sin(lat * M_PI / 180.0);
  double cy = (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * n;
  int tx0 = (int)floor(cx - (W / 2.0) / tilePx), tx1 = (int)floor(cx + (W / 2.0) / tilePx);
  int ty0 = (int)floor(cy - (H / 2.0) / tilePx), ty1 = (int)floor(cy + (H / 2.0) / tilePx);

  struct T { std::vector<uint8_t> data; TilePlacement tp; };
  std::vector<T> tiles;
  for (int tx = tx0; tx <= tx1; tx++) for (int ty = ty0; ty <= ty1; ty++) {
    T t; if (!loadFile(dir + "/" + std::to_string(zi) + "/" + std::to_string(tx) + "/" + std::to_string(ty) + ".bin", t.data)) continue;
    t.tp.ox = (float)((tx - cx) * tilePx + W / 2.0); t.tp.oy = (float)((ty - cy) * tilePx + H / 2.0);
    t.tp.scale = (float)(tilePx / TILE_EXTENT);
    tiles.push_back(std::move(t));
  }
  auto t0 = std::chrono::steady_clock::now();
  raster.clear(STYLE_BG);
  for (int pass = PASS_AREA; pass < PASS_LABELS; pass++)
    for (auto& t : tiles) renderer.drawTile(t.data.data(), t.data.size(), t.tp, (float)zoom, (RenderPass)pass);
  int labels = 0;
  for (auto& t : tiles) renderer.drawTile(t.data.data(), t.data.size(), t.tp, (float)zoom, PASS_LABELS,
    [](const LabelCandidate& c, void* u) { (*(int*)u)++; if (*(int*)u <= 5) fprintf(stderr, "  label L%d/%d %s @ %.0f,%.0f\n", c.layer, c.cls, c.name, c.x, c.y); }, &labels);
  auto t1 = std::chrono::steady_clock::now();
  size_t bytes = 0; for (auto& t : tiles) bytes += t.data.size();
  fprintf(stderr, "%zu tiles (%zu KB), %d label candidates, host render %.1f ms, %u edges, %u px\n", tiles.size(), bytes / 1024, labels,
          std::chrono::duration<double, std::milli>(t1 - t0).count(), raster.statEdges, raster.statPixels);
  FILE* f = fopen(argv[7], "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int i = 0; i < W * H; i++) { uint16_t c = canvas[i]; unsigned char px[3] = {(unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31)}; fwrite(px, 1, 3, f); }
  fclose(f); return 0;
}
