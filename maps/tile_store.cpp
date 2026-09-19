#include "tile_store.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

static const size_t MAX_TILE = 256 * 1024;
static const size_t FLASH_KEEP_FREE = 256 * 1024;      // stop caching to flash below this

static void writerTrampoline(void* p) { ((TileStore*)p)->writerLoop(); }

bool TileStore::begin(const char* base_url, size_t psram_budget) {
  strncpy(_base, base_url, sizeof _base - 1);
  _budget = psram_budget;
  _fsOk = LittleFS.begin(true, "/tiles", 8, "tiles");
  if (_fsOk) Serial.printf("[tiles] flash cache: %u / %u KB used\n", (unsigned)(flashUsed() >> 10), (unsigned)(flashTotal() >> 10));
  else       Serial.println("[tiles] LittleFS mount failed; flash cache disabled");
  if (_fsOk) {
    if (!LittleFS.exists("/v2")) {                 // layout changed: wipe old cache once
      Serial.println("[tiles] formatting flash cache (layout v2)");
      LittleFS.format(); LittleFS.begin(true, "/tiles", 8, "tiles");
      File m = LittleFS.open("/v2", "w"); m.close();
    }
    buildFlashIndex();
    xTaskCreatePinnedToCore(writerTrampoline, "tilewr", 6 * 1024, this, 0, &_writer, 0);
  }
  return true;
}

bool TileStore::flashHas(uint32_t key) const {
  if (!_flashIdx) return false;
  uint32_t i = (key * 2654435761u) & (FLASH_IDX_CAP - 1);
  while (_flashIdx[i]) { if (_flashIdx[i] == key) return true; i = (i + 1) & (FLASH_IDX_CAP - 1); }
  return false;
}
void TileStore::flashAdd(uint32_t key) {
  if (!_flashIdx || key == 0 || _flashCount > FLASH_IDX_CAP * 3 / 4) return;
  uint32_t i = (key * 2654435761u) & (FLASH_IDX_CAP - 1);
  while (_flashIdx[i]) { if (_flashIdx[i] == key) return; i = (i + 1) & (FLASH_IDX_CAP - 1); }
  _flashIdx[i] = key; _flashCount++;
}
void TileStore::buildFlashIndex() {
  _flashIdx = (uint32_t*)heap_caps_calloc(FLASH_IDX_CAP, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
  uint32_t t0 = millis();
  File root = LittleFS.open("/");
  for (File zd = root.openNextFile(); zd; zd = root.openNextFile()) {
    if (!zd.isDirectory()) continue;
    int z = atoi(zd.name());
    for (File xd = zd.openNextFile(); xd; xd = zd.openNextFile()) {
      if (!xd.isDirectory()) continue;
      int x = atoi(xd.name());
      for (File f = xd.openNextFile(); f; f = xd.openNextFile()) flashAdd(keyOf(z, x, atoi(f.name())));
    }
  }
  Serial.printf("[tiles] flash index: %d tiles in %lu ms\n", _flashCount, (unsigned long)(millis() - t0));
}

void TileStore::enqueueWrite(int z, int x, int y, const uint8_t* data, uint32_t len) {
  if (!_fsOk) return;
  int next = (_wqHead + 1) % WRITE_Q;
  if (next == _wqTail) return;                                   // queue full: skip, it's only a cache
  uint8_t* copy = nullptr;
  if (len) { copy = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM); if (!copy) return; memcpy(copy, data, len); }
  _wq[_wqHead] = {z, x, y, copy, len};
  _wqHead = next;
  if (_writer) xTaskNotifyGive(_writer);
}

void TileStore::writerLoop() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (_wqTail != _wqHead) {
      Pending p = _wq[_wqTail];
      writeFlash(p.z, p.x, p.y, p.data, p.len);
      if (p.data) heap_caps_free(p.data);
      _wqTail = (_wqTail + 1) % WRITE_Q;
      vTaskDelay(1);
    }
  }
}

size_t TileStore::flashUsed() const  { return _fsOk ? LittleFS.usedBytes() : 0; }
size_t TileStore::flashTotal() const { return _fsOk ? LittleFS.totalBytes() : 0; }

TileStore::Entry* TileStore::find(uint32_t key) {
  for (int i = 0; i < _n; i++) if (_e[i].key == key) { _e[i].lastUse = ++_tick; return &_e[i]; }
  return nullptr;
}

void TileStore::evict(size_t need) {
  while (_n > 0 && (_used + need > _budget || _n >= MAX_ENTRIES)) {
    int victim = 0;
    for (int i = 1; i < _n; i++) if (_e[i].lastUse < _e[victim].lastUse) victim = i;
    if (_e[victim].data) { heap_caps_free(_e[victim].data); _used -= _e[victim].len; }
    _e[victim] = _e[--_n];
  }
}

TileStore::Entry* TileStore::insert(uint32_t key, uint8_t* data, uint32_t len, bool empty) {
  evict(len);
  Entry& e = _e[_n++];
  e.key = key; e.data = data; e.len = len; e.empty = empty; e.lastUse = ++_tick;
  _used += len;
  return &e;
}

static void pathOf(char* buf, size_t cap, int z, int x, int y) { snprintf(buf, cap, "/%d/%d/%d", z, x, y); }

bool TileStore::readFlash(int z, int x, int y, uint8_t*& data, uint32_t& len) {
  if (!_fsOk || !flashHas(keyOf(z, x, y))) return false;
  char path[40]; pathOf(path, sizeof path, z, x, y);
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  len = f.size();
  data = len ? (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM) : nullptr;
  if (len && !data) { f.close(); return false; }
  if (len) f.read(data, len);
  f.close();
  return true;
}

void TileStore::writeFlash(int z, int x, int y, const uint8_t* data, uint32_t len) {
  if (!_fsOk) return;
  if (flashTotal() - flashUsed() < len + FLASH_KEEP_FREE) return;
  char dir[24]; snprintf(dir, sizeof dir, "/%d", z);
  if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);
  snprintf(dir, sizeof dir, "/%d/%d", z, x);
  if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);
  char path[40]; pathOf(path, sizeof path, z, x, y);
  File f = LittleFS.open(path, "w");
  if (!f) return;
  if (len) f.write(data, len);
  f.close();
  flashAdd(keyOf(z, x, y));
}

bool TileStore::fetch(int z, int x, int y, uint8_t*& data, uint32_t& len, bool& notFound) {
  notFound = false; data = nullptr; len = 0;
  if (!_online || WiFi.status() != WL_CONNECTED) return false;
  char url[192]; snprintf(url, sizeof url, "%s/%d/%d/%d.bin", _base, z, x, y);
  static WiFiClient client;
  static HTTPClient http;
  http.setReuse(true);
  http.setTimeout(4000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code == 404) { notFound = true; http.end(); return true; }
  if (code != 200) { Serial.printf("[tiles] GET %s -> %d (%s)\n", url, code, http.errorToString(code).c_str()); http.end(); return false; }
  int size = http.getSize();
  if (size <= 0 || (size_t)size > MAX_TILE) { Serial.printf("[tiles] GET %s: bad size %d\n", url, size); http.end(); return false; }
  data = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (!data) { http.end(); return false; }
  WiFiClient* s = http.getStreamPtr();
  uint32_t got = 0, t0 = millis();
  while (got < (uint32_t)size && millis() - t0 < 4000) {
    int avail = s->available();
    if (avail > 0) { int r = s->read(data + got, min<int>(avail, size - got)); if (r > 0) got += r; }
    else delay(1);
  }
  http.end();
  if (got != (uint32_t)size) { Serial.printf("[tiles] GET %s: short read %lu/%d\n", url, (unsigned long)got, size); heap_caps_free(data); data = nullptr; return false; }
  len = size;
  statBytes += len;
  return true;
}

bool TileStore::has(int z, int x, int y) {
  uint32_t key = keyOf(z, x, y);
  for (int i = 0; i < _n; i++) if (_e[i].key == key) return true;
  return false;
}

bool TileStore::get(int z, int x, int y, TileData& out) {
  uint32_t key = keyOf(z, x, y);
  if (Entry* e = find(key)) { statHits++; out.data = e->empty ? nullptr : e->data; out.len = e->len; return true; }
  uint8_t* data; uint32_t len;
  if (readFlash(z, x, y, data, len)) {
    statFlashHits++;
    Entry* e = insert(key, data, len, len == 0);
    out.data = e->empty ? nullptr : e->data; out.len = e->len;
    return true;
  }
  bool notFound;
  uint32_t t0 = millis();
  bool ok = fetch(z, x, y, data, len, notFound);
  statFetchMs += millis() - t0;
  if (!ok) { statFails++; return false; }
  statFetches++;
  enqueueWrite(z, x, y, data, len);             // empty file marks a known-missing tile
  Entry* e = insert(key, data, len, notFound || len == 0);
  out.data = e->empty ? nullptr : e->data; out.len = e->len;
  return true;
}
