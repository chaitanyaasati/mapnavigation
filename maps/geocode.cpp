#include "geocode.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <ctype.h>

bool Geocoder::load(const char* url) {
  WiFiClient plain; WiFiClientSecure tls; HTTPClient http;
  bool https = strncmp(url, "https://", 8) == 0;
  if (https) tls.setInsecure();
  http.setTimeout(15000);
  if (!http.begin(https ? (WiFiClient&)tls : plain, url)) return false;
  int code = http.GET();
  int size = http.getSize();
  if (code != 200 || size <= 8) { Serial.printf("[geo] GET %s -> %d\n", url, code); http.end(); return false; }
  uint8_t* buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (!buf) { http.end(); return false; }
  WiFiClient* s = http.getStreamPtr();
  int got = 0; uint32_t t0 = millis();
  while (got < size && millis() - t0 < 30000) {
    int a = s->available();
    if (a > 0) { int r = s->read(buf + got, min(a, size - got)); if (r > 0) got += r; }
    else delay(1);
  }
  http.end();
  if (got != size || memcmp(buf, "PLC1", 4) != 0) { heap_caps_free(buf); return false; }
  memcpy(&_count, buf + 4, 4);
  _data = buf; _len = size;
  Serial.printf("[geo] %lu places (%d KB) in %lu ms\n", (unsigned long)_count, size >> 10, (unsigned long)(millis() - t0));
  return true;
}

// lowercase ASCII, punctuation -> space, collapsed spaces; drops a leading "the "
void Geocoder::normalise(const char* in, char* out, size_t cap) {
  size_t n = 0; bool space = true;
  for (const unsigned char* p = (const unsigned char*)in; *p && n + 1 < cap; p++) {
    char c = *p;
    if (c & 0x80) continue;                       // non-ASCII bytes are dropped (names are mostly ASCII)
    if (isalnum(c)) { out[n++] = tolower(c); space = false; }
    else if (!space) { out[n++] = ' '; space = true; }
  }
  while (n && out[n - 1] == ' ') n--;
  out[n] = 0;
  if (strncmp(out, "the ", 4) == 0) memmove(out, out + 4, n - 3);
}

bool Geocoder::find(const char* query, GeoHit& best) {
  if (!_data) return false;
  char q[64]; normalise(query, q, sizeof q);
  size_t qlen = strlen(q);
  if (qlen < 2) return false;
  // also try without common suffixes people drop/add ("road", "layout", "nagar")
  best.score = 0;
  const uint8_t* p = _data + 8;
  char name[64], nn[64];
  for (uint32_t i = 0; i < _count && p + 11 <= _data + _len; i++) {
    float lon, lat; memcpy(&lon, p, 4); memcpy(&lat, p + 4, 4);
    uint8_t rank = p[8], kind = p[9], len = p[10];
    size_t n = len < sizeof name - 1 ? len : sizeof name - 1;
    memcpy(name, p + 11, n); name[n] = 0;
    p += 11 + len;
    normalise(name, nn, sizeof nn);
    int s = 0;
    if (strcmp(nn, q) == 0) s = 100;
    else {
      const char* at = strstr(nn, q);
      if (at == nn) s = 80;                                            // name starts with query
      else if (at && at[-1] == ' ') s = 60;                            // query starts a word inside the name
      else if (at) s = 40;
      else if (strlen(nn) >= 4 && strstr(q, nn) == q) s = 30;          // query is "<name> <extra words>"
      else if (strlen(nn) >= 4 && strstr(q, nn)) s = 20;
    }
    if (!s) continue;
    // shorter names are better matches for the same score; better rank wins ties
    int score = s * 100 - (int)strlen(nn) - rank * 8;
    if (score > best.score) {
      best.score = score; best.lon = lon; best.lat = lat; best.rank = rank; best.kind = kind;
      strncpy(best.name, name, sizeof best.name - 1); best.name[sizeof best.name - 1] = 0;
    }
  }
  return best.score > 0;
}
