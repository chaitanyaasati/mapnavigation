#pragma once
// =============================================================================
//  geocode - offline place lookup from places.bin (tools/build_places.py)
// =============================================================================
#include <stdint.h>
#include <stddef.h>

struct GeoHit { double lon, lat; uint8_t rank, kind; char name[64]; int score; };

class Geocoder {
public:
  bool load(const char* url);                       // GET places.bin into PSRAM
  bool ready() const { return _data != nullptr; }
  uint32_t count() const { return _count; }
  bool find(const char* query, GeoHit& best);       // best fuzzy-ish match, false if nothing plausible
private:
  uint8_t* _data = nullptr; size_t _len = 0; uint32_t _count = 0;
  static void normalise(const char* in, char* out, size_t cap);
};
