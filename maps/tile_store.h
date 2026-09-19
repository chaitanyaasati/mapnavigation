#pragma once
// =============================================================================
//  tile_store - gets VT01 tiles from the network and keeps them close
// =============================================================================
//  Lookup order: PSRAM LRU -> LittleFS cache (flash "tiles" partition) -> HTTP
//  GET <base_url>/<z>/<x>/<y>.bin. Missing tiles (404) are remembered as empty
//  so the sea of nothing around a region does not cost a request per frame.
//  All calls are meant for the render task; nothing here touches LVGL.
#include <stdint.h>
#include <stddef.h>
#include <Arduino.h>

struct TileData {
  const uint8_t* data;   // nullptr for a known-empty tile
  size_t len;
};

class TileStore {
public:
  // base_url like "http://192.168.1.10:8000/bengaluru"; psram_budget bytes for the LRU
  bool begin(const char* base_url, size_t psram_budget);
  // Returns true when the tile state is known (data may still be null = empty).
  // Blocks on the network when needed; returns false on a transient failure.
  bool get(int z, int x, int y, TileData& out);
  void setOnline(bool on) { _online = on; }
  uint32_t statHits = 0, statFlashHits = 0, statFetches = 0, statBytes = 0, statFails = 0, statFetchMs = 0;
  bool has(int z, int x, int y);   // cached in PSRAM (no I/O)
  size_t flashUsed() const, flashTotal() const;

private:
  struct Entry { uint32_t key; uint8_t* data; uint32_t len; uint32_t lastUse; bool empty; };
  static const int MAX_ENTRIES = 512;
  Entry _e[MAX_ENTRIES]; int _n = 0;
  size_t _used = 0, _budget = 0;
  uint32_t _tick = 0;
  char _base[128];
  bool _online = false, _fsOk = false;

  static uint32_t keyOf(int z, int x, int y) { uint32_t k = ((uint32_t)z << 27) ^ ((uint32_t)x << 14) ^ (uint32_t)y ^ ((uint32_t)z * 0x9E3779B1u); return k ? k : 1; }
  Entry* find(uint32_t key);
  Entry* insert(uint32_t key, uint8_t* data, uint32_t len, bool empty);
  void evict(size_t need);
  bool fetch(int z, int x, int y, uint8_t*& data, uint32_t& len, bool& notFound);
  // What is on flash, so a miss never touches LittleFS (its lookups are slow and
  // block behind the writer). Open-addressing set of tile keys, built at boot.
  static const int FLASH_IDX_CAP = 16384;
  uint32_t* _flashIdx = nullptr; int _flashCount = 0;
  bool flashHas(uint32_t key) const;
  void flashAdd(uint32_t key);
  void buildFlashIndex();
  bool readFlash(int z, int x, int y, uint8_t*& data, uint32_t& len);
  void writeFlash(int z, int x, int y, const uint8_t* data, uint32_t len);   // blocking; called by the writer task
  // Flash writes are slow (100-500 ms each) so they are queued to a background task.
  struct Pending { int z, x, y; uint8_t* data; uint32_t len; };
  static const int WRITE_Q = 48;
  Pending _wq[WRITE_Q]; volatile int _wqHead = 0, _wqTail = 0;
  TaskHandle_t _writer = nullptr;
  void enqueueWrite(int z, int x, int y, const uint8_t* data, uint32_t len);
public:
  void writerLoop();
};
