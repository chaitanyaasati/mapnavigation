// =============================================================================
//  maps - vector map viewer for the Cheeko Gotchi V2 (ESP32-S3, 240x296)
// =============================================================================
//  Tiles (tools/tile_format.md) are fetched over WiFi from TILE_SERVER, cached
//  in PSRAM and flash, and drawn with the in-house anti-aliased rasteriser into
//  an off-screen canvas that LVGL pans/scales instantly while the next render
//  runs on the other core.
//
//  Controls: drag = pan, double-tap = zoom in there, long-press = zoom out,
//            VOL+ / VOL- = zoom in / out.
// =============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include "board_config.h"
#include "lgfx_setup.h"
#include "secrets.h"
#include <Wire.h>
#include "touch_cst810.h"
#include "tile_store.h"
#include "map_view.h"
#include "geocode.h"
#include "voice.h"
#include "provision.h"

static LGFX lcd;
static TileStore store;
static MapView mapv;   // "map" clashes with Arduino's map()
static Geocoder geo;
static Settings settings;
static String tileBase;          // settings.tiles with a .local host resolved to an IP
static lv_obj_t* status_lbl;
static lv_obj_t* toast_lbl;
static lv_obj_t* mic_btn;
static uint32_t toast_until = 0;

// Bengaluru, MG Road-ish
static const double HOME_LON = 77.6094, HOME_LAT = 12.9752;
static const float  HOME_ZOOM = 14;

// ---- LVGL <-> LovyanGFX glue -------------------------------------------------
static const size_t DRAW_BUF_LINES = 24;

static void flush_cb(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  lcd.startWrite();
  lcd.setAddrWindow(a->x1, a->y1, w, h);
  lcd.writePixels((lgfx::rgb565_t*)px, w * h);
  lcd.endWrite();
  lv_display_flush_ready(d);
}

static void touch_cb(lv_indev_t*, lv_indev_data_t* data) {
  uint16_t rx, ry;
  if (cst810_read(rx, ry)) {
    // Raw controller axes (x along the 296 px side, y along the 240 px side) -> screen, as measured
    // with the board held upright: top-left = (0,239), bottom-right = (295,0). Independent of setRotation.
    int dx = LCD_W - 1 - (int)ry, dy = (int)rx;
    data->point.x = constrain(dx, 0, LCD_W - 1); data->point.y = constrain(dy, 0, LCD_H - 1);
    data->state = LV_INDEV_STATE_PRESSED;
  } else data->state = LV_INDEV_STATE_RELEASED;
}

// ---- gestures -------------------------------------------------------------------
static void map_event(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t* indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p;
  switch (code) {
    case LV_EVENT_PRESSING:
      lv_indev_get_vect(indev, &p);
      if (p.x || p.y) mapv.panBy(p.x, p.y);
      break;
    case LV_EVENT_RELEASED:
      mapv.endPan();
      break;
    case LV_EVENT_DOUBLE_CLICKED:
      lv_indev_get_point(indev, &p);
      mapv.zoomBy(+1, p.x, p.y);
      break;
    case LV_EVENT_LONG_PRESSED:
      mapv.zoomBy(-1);
      break;
    default: break;
  }
}

// "http://Macbook-Pro.local:8000/x" -> "http://192.168.0.7:8000/x". Plain IPs/hostnames pass through.
// lwIP's resolver does not speak mDNS, so .local names are looked up here once WiFi is up.
static String resolveTileBase(const char* url) {
  String u = url;
  int hs = u.indexOf("://"); if (hs < 0) return u;
  hs += 3;
  int he = u.indexOf('/', hs); if (he < 0) he = u.length();
  String hostport = u.substring(hs, he);
  int colon = hostport.indexOf(':');
  String host = colon >= 0 ? hostport.substring(0, colon) : hostport;
  String port = colon >= 0 ? hostport.substring(colon) : "";
  if (!host.endsWith(".local")) return u;
  String name = host.substring(0, host.length() - 6);
  IPAddress ip = MDNS.queryHost(name.c_str(), 3000);
  if (ip == IPAddress((uint32_t)0)) { Serial.printf("[mdns] %s not found\n", host.c_str()); return u; }
  Serial.printf("[mdns] %s -> %s\n", host.c_str(), ip.toString().c_str());
  return u.substring(0, hs) + ip.toString() + port + u.substring(he);
}

static void toast(const char* text, uint32_t ms = 4000) {
  lv_label_set_text(toast_lbl, text);
  lv_obj_set_hidden(toast_lbl, false);
  toast_until = millis() + ms;
}

static void mic_event(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) { store.closeConnection(); voice_start(); toast("Listening... hold while you speak", 15000); }
  else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) { voice_stop(); if (voice_state() == VS_LISTENING || voice_state() == VS_THINKING) toast("Thinking...", 20000); }
}

static void handle_voice(const VoiceCommand& c) {
  char msg[160];
  switch (c.action) {
    case VoiceCommand::GOTO: {
      GeoHit hit;
      if (geo.find(c.place, hit)) {
        float z = c.zoom > 0 ? c.zoom : (hit.kind == 0 ? (hit.rank <= 1 ? 12 : 14) : 15.5f);
        mapv.flyTo(hit.lon, hit.lat, z);
        snprintf(msg, sizeof msg, "%s", hit.name);
      } else snprintf(msg, sizeof msg, "Not found: %s", c.place);
      break;
    }
    case VoiceCommand::ZOOM: mapv.zoomBy(c.delta); snprintf(msg, sizeof msg, "Zoom %+d", c.delta); break;
    case VoiceCommand::PAN: {
      int dx = 0, dy = 0;
      if (!strcmp(c.dir, "north")) dy = LCD_H / 2; else if (!strcmp(c.dir, "south")) dy = -LCD_H / 2;
      else if (!strcmp(c.dir, "east")) dx = -LCD_W / 2; else if (!strcmp(c.dir, "west")) dx = LCD_W / 2;
      mapv.panBy(dx, dy); mapv.endPan();
      snprintf(msg, sizeof msg, "Pan %s", c.dir); break;
    }
    default: snprintf(msg, sizeof msg, "%s", c.say[0] ? c.say : "?"); break;
  }
  toast(msg);
  Serial.printf("[voice] -> %s\n", msg);
}

static void build_ui() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xF2EFE9), 0);
  lv_obj_set_scrollable(scr, false);
  lv_obj_set_clickable(scr, true);

  mapv.begin(scr, &store, 10, 16);
  lv_obj_add_event_cb(scr, map_event, LV_EVENT_ALL, nullptr);

  status_lbl = lv_label_create(scr);
  lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_bg_color(status_lbl, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(status_lbl, LV_OPA_50, 0);
  lv_obj_set_style_text_color(status_lbl, lv_color_white(), 0);
  lv_obj_set_style_pad_all(status_lbl, 3, 0);
  lv_obj_set_style_radius(status_lbl, 3, 0);
  lv_label_set_text(status_lbl, "connecting...");
  lv_obj_align(status_lbl, LV_ALIGN_BOTTOM_LEFT, 3, -3);
  lv_obj_set_clickable(status_lbl, false);

  toast_lbl = lv_label_create(scr);
  lv_obj_set_style_text_font(toast_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_bg_color(toast_lbl, lv_color_hex(0x1E3A5F), 0);
  lv_obj_set_style_bg_opa(toast_lbl, LV_OPA_80, 0);
  lv_obj_set_style_text_color(toast_lbl, lv_color_white(), 0);
  lv_obj_set_style_pad_hor(toast_lbl, 8, 0);
  lv_obj_set_style_pad_ver(toast_lbl, 5, 0);
  lv_obj_set_style_radius(toast_lbl, 6, 0);
  lv_obj_set_width(toast_lbl, LCD_W - 16);
  lv_label_set_long_mode(toast_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(toast_lbl, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_set_clickable(toast_lbl, false);
  lv_obj_set_hidden(toast_lbl, true);

  // push-to-talk button
  mic_btn = lv_button_create(scr);
  lv_obj_set_size(mic_btn, 64, 64);
  lv_obj_set_style_radius(mic_btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(mic_btn, lv_color_hex(0x1E3A5F), 0);
  lv_obj_set_style_bg_color(mic_btn, lv_color_hex(0xE0302B), LV_STATE_PRESSED);   // bright red while held
  lv_obj_set_style_border_width(mic_btn, 3, 0);
  lv_obj_set_style_border_color(mic_btn, lv_color_white(), 0);
  lv_obj_set_style_border_width(mic_btn, 6, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(mic_btn, lv_color_hex(0xFFD54F), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(mic_btn, 10, 0);
  lv_obj_set_style_shadow_opa(mic_btn, LV_OPA_40, 0);
  lv_obj_align(mic_btn, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
  lv_obj_t* ic = lv_label_create(mic_btn);
  lv_label_set_text(ic, LV_SYMBOL_AUDIO);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(ic, lv_color_white(), 0);
  lv_obj_center(ic);
  lv_obj_add_event_cb(mic_btn, mic_event, LV_EVENT_ALL, nullptr);
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n=== maps ===");
  pinMode(PIN_KEY_VOL_UP, INPUT_PULLUP);
  pinMode(PIN_KEY_VOL_DOWN, INPUT_PULLUP);
  // Hold GPIO2 low the way the stock firmware does (left floating, the board
  // powered itself off after ~30 min). See board_config.h for what is known.
  pinMode(PIN_POWER_HOLD, OUTPUT); digitalWrite(PIN_POWER_HOLD, LOW);

  bool wireOk = Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  Serial.printf("[i2c] Wire.begin -> %d; devices:", wireOk);
  for (uint8_t a = 1; a < 127; a++) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a); }
  Serial.println();
  cst810_init();

  lcd.init();
  lcd.setRotation(2);              // panel is mounted upside down relative to the case
  lcd.setBrightness(200);
  lcd.fillScreen(TFT_BLACK);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_log_register_print_cb([](lv_log_level_t, const char* buf) { Serial.print(buf); });
  lv_display_t* disp = lv_display_create(LCD_W, LCD_H);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  size_t buf_bytes = LCD_W * DRAW_BUF_LINES * 2;
  lv_display_set_buffers(disp, heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                         heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_indev_t* touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, touch_cb);

  prov_load(settings, SEED_WIFI_SSID, SEED_WIFI_PASS, TILE_SERVER);   // NVS, falling back to secrets.h
  Serial.printf("[wifi] ssid \"%s\", tiles %s\n", settings.ssid, settings.tiles);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  if (settings.ssid[0]) WiFi.begin(settings.ssid, settings.pass);

  tileBase = settings.tiles;
  store.begin(settings.tiles, 3 * 1024 * 1024);
  build_ui();
  mapv.setCenter(HOME_LON, HOME_LAT, HOME_ZOOM);
  voice_begin();
  // VOL- held at power-on, or no WiFi configured at all: go straight to setup
  if (digitalRead(PIN_KEY_VOL_DOWN) == LOW || !settings.ssid[0]) prov_start(settings);
  Serial.printf("heap %u KB, psram %u KB\n", ESP.getFreeHeap() >> 10, ESP.getFreePsram() >> 10);
}

// Serial console for testing without touching the device:
//   z <zoom>            set zoom      g <lon> <lat> [zoom]   go to
//   s                   stats
static void handleSerial() {
  static char line[200]; static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line[n] = 0; n = 0;
      double lon, lat; float z;
      if (line[0] == 'z' && sscanf(line + 1, "%f", &z) == 1) { mapv.zoomBy(z - mapv.zoom()); }
      else if (line[0] == 'g') {
        int k = sscanf(line + 1, "%lf %lf %f", &lon, &lat, &z);
        if (k >= 2) mapv.flyTo(lon, lat, k == 3 ? z : mapv.zoom());
      } else if (line[0] == 'f') {
        GeoHit hit;
        if (geo.find(line + 2, hit)) { Serial.printf("[geo] '%s' -> %s (%.5f,%.5f) rank %d kind %d score %d\n", line + 2, hit.name, hit.lon, hit.lat, hit.rank, hit.kind, hit.score); mapv.flyTo(hit.lon, hit.lat, hit.kind == 0 ? 14 : 15.5f); }
        else Serial.printf("[geo] '%s' not found\n", line + 2);
      } else if (line[0] == 'v') {
        store.closeConnection(); voice_test_text(line + 2);
      } else if (line[0] == 'P') {                 // P <gpio>: tone with that pin held high (hunting the amp enable)
        int pin = atoi(line + 1);
        static const int ok[] = {1, 3, 4, 18, 21, 38, 41, 42, 47, 48};
        bool allowed = false; for (int o : ok) if (o == pin) allowed = true;
        if (allowed) { pinMode(pin, OUTPUT); digitalWrite(pin, HIGH); }
        voice_beep(1000, 700);
        if (allowed) { digitalWrite(pin, LOW); pinMode(pin, INPUT); }
        Serial.printf("[pwr] tone done (GPIO%d high: %s)\n", pin, allowed ? "yes" : "not allowed");
      } else if (line[0] == 'L') {                 // re-init the LCD panel without rebooting (dark-screen test)
        lcd.init(); lcd.setRotation(2); lcd.setBrightness(200);
        lv_obj_invalidate(lv_screen_active());
        Serial.println("[lcd] re-initialised");
      } else if (line[0] == 'p') {
        voice_beep(1000, 1500);
      } else if (line[0] == 't') {                 // t <url> : set the tile server (saved to flash), then reboot
        String u = String(line + 1); u.trim();
        if (u.startsWith("http")) { strlcpy(settings.tiles, u.c_str(), sizeof settings.tiles); prov_save(settings); Serial.printf("[tiles] server -> %s, rebooting\n", settings.tiles); delay(200); ESP.restart(); }
        else Serial.printf("[tiles] server is %s (effective %s)\n", settings.tiles, tileBase.c_str());
      } else if (line[0] == 'w') {                 // w : open the WiFi setup portal now
        prov_start(settings);
      } else if (line[0] == 'b') {
        int v = atoi(line + 1); lcd.setBrightness(v); Serial.printf("[bl] brightness %d\n", v);
      } else if (line[0] == 's') {
        mapv.centerLonLat(lon, lat);
        Serial.printf("[stats] center %.5f,%.5f z%.2f | tiles: %lu psram-hits %lu flash-hits %lu fetches %lu KB %lu fails | flash %u/%u KB | heap %u KB psram %u KB\n",
                      lon, lat, mapv.zoom(), (unsigned long)store.statHits, (unsigned long)store.statFlashHits, (unsigned long)store.statFetches,
                      (unsigned long)(store.statBytes >> 10), (unsigned long)store.statFails, (unsigned)(store.flashUsed() >> 10), (unsigned)(store.flashTotal() >> 10),
                      ESP.getFreeHeap() >> 10, ESP.getFreePsram() >> 10);
      }
    } else if (n < (int)sizeof(line) - 1) line[n++] = c;
  }
}

void loop() {
  static uint32_t lastKeys = 0, lastStatus = 0;
  static int upPrev = 1, dnPrev = 1;
  static bool wasOnline = false;

  lv_timer_handler();
  handleSerial();
  if (prov_active()) { prov_loop(); delay(2); return; }      // setup screen owns the device until it reboots
  mapv.loop();

  // never got online since boot -> open the setup portal (new owner, new WiFi)
  static bool everOnline = false;
  if (WiFi.status() == WL_CONNECTED) everOnline = true;
  if (!everOnline && millis() > 25000) { prov_start(settings); return; }

  bool online = WiFi.status() == WL_CONNECTED;
  if (online != wasOnline) {
    wasOnline = online;
    store.setOnline(online);
    if (online) {
      Serial.printf("[wifi] %s\n", WiFi.localIP().toString().c_str());
      MDNS.begin("cheekomaps");
      tileBase = resolveTileBase(settings.tiles);
      store.setBase(tileBase.c_str());
      mapv.endPan();
    }
  }
  if (online && !geo.ready()) {
    static uint32_t lastTry = 0;
    if (millis() - lastTry > 10000) {
      lastTry = millis();
      if (!geo.load((tileBase + "/places.bin").c_str())) { tileBase = resolveTileBase(settings.tiles); store.setBase(tileBase.c_str()); }
    }
  }
  VoiceCommand vc;
  if (voice_poll(vc)) handle_voice(vc);
  if (toast_until && millis() > toast_until) { toast_until = 0; lv_obj_set_hidden(toast_lbl, true); }

  if (millis() - lastKeys > 20) {
    lastKeys = millis();
    int up = digitalRead(PIN_KEY_VOL_UP), dn = digitalRead(PIN_KEY_VOL_DOWN);
    static uint32_t dnSince = 0;
    if (up != upPrev) { upPrev = up; if (!up) mapv.zoomBy(+1); }
    if (dn != dnPrev) {
      dnPrev = dn;
      if (!dn) dnSince = millis();
      else if (dnSince && millis() - dnSince < 3000) mapv.zoomBy(-1);   // short press: zoom out
      if (dn) dnSince = 0;
    }
    if (!dn && dnSince && millis() - dnSince >= 3000) { dnSince = 0; prov_start(settings); return; }   // 3 s hold: WiFi setup
  }

  if (millis() - lastStatus > 200) {
    lastStatus = millis();
    double lon, lat; mapv.centerLonLat(lon, lat);
    static const char* vs[] = {"", "", "REC", "...", "ERR"};
    VoiceState st = voice_state();
    lv_obj_set_style_bg_color(mic_btn, st == VS_LISTENING ? lv_color_hex(0xE0302B) : st == VS_THINKING ? lv_color_hex(0xE69A1F) : lv_color_hex(0x1E3A5F), 0);
    lv_label_set_text_fmt(status_lbl, "z%.1f %s %lums%s h%uK %s", mapv.zoom(), online ? "wifi" : "no-wifi",
                          (unsigned long)mapv.lastRenderMs, mapv.lastTilesMissing ? " ..." : "", ESP.getFreeHeap() >> 10, vs[voice_state()]);
  }
  delay(2);
}
