#include "provision.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

static const char* AP_SSID = "CheekoMaps-Setup";
static WebServer* server = nullptr;
static DNSServer* dns = nullptr;
static bool active = false;
static Settings cur;
static lv_obj_t* scr = nullptr;
static lv_obj_t* statusLbl = nullptr;
static String scanHtml;

void prov_load(Settings& s, const char* defSsid, const char* defPass, const char* defTiles) {
  Preferences p; p.begin("maps", true);
  strlcpy(s.ssid, p.getString("ssid", defSsid).c_str(), sizeof s.ssid);
  strlcpy(s.pass, p.getString("pass", defPass).c_str(), sizeof s.pass);
  strlcpy(s.tiles, p.getString("tiles", defTiles).c_str(), sizeof s.tiles);
  p.end();
}

bool prov_save(const Settings& s) {
  Preferences p; if (!p.begin("maps", false)) return false;
  p.putString("ssid", s.ssid); p.putString("pass", s.pass); p.putString("tiles", s.tiles);
  p.end();
  return true;
}

static String htmlEscape(const String& in) {
  String o; for (char c : in) { if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '"') o += "&quot;"; else o += c; }
  return o;
}

static void buildScanList() {
  int n = WiFi.scanNetworks();
  scanHtml = "";
  for (int i = 0; i < n && i < 20; i++) {
    String s = WiFi.SSID(i); if (!s.length()) continue;
    scanHtml += "<option value=\"" + htmlEscape(s) + "\">" + htmlEscape(s) + " (" + String(WiFi.RSSI(i)) + " dBm" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? ", open" : "") + ")</option>";
  }
  WiFi.scanDelete();
}

static void handleRoot() {
  String page =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Cheeko Maps setup</title><style>body{font-family:system-ui;margin:0;background:#f2efe9;color:#222}"
    ".c{max-width:420px;margin:0 auto;padding:20px}h1{font-size:22px;margin:0 0 4px}p{color:#555;margin:0 0 18px}"
    "label{display:block;font-size:14px;margin:14px 0 4px}select,input{width:100%;box-sizing:border-box;font-size:16px;padding:10px;border:1px solid #bbb;border-radius:8px;background:#fff}"
    "button{width:100%;margin-top:22px;padding:14px;font-size:17px;border:0;border-radius:10px;background:#1e3a5f;color:#fff}"
    "small{display:block;color:#777;margin-top:6px}</style></head><body><div class=c>"
    "<h1>Cheeko Maps</h1><p>Connect the map to your WiFi.</p><form method=POST action=/save>"
    "<label>WiFi network</label><select name=ssid id=sel onchange=\"document.getElementById('o').value=this.value\">"
    "<option value=''>-- choose --</option>" + scanHtml + "</select>"
    "<label>or type the network name</label><input id=o name=ssid_other placeholder='SSID' value=\"" + htmlEscape(cur.ssid) + "\">"
    "<label>Password</label><input name=pass type=password placeholder='WiFi password'>"
    "<label>Map tile server</label><input name=tiles value=\"" + htmlEscape(cur.tiles) + "\"><small>Leave as is unless the map server moved.</small>"
    "<button>Save &amp; connect</button></form>"
    "<small style='margin-top:26px'>Hold VOL&minus; for 3 s on the device to open this page again.</small></div></body></html>";
  server->send(200, "text/html", page);
}

static void handleSave() {
  Settings s = cur;
  String ssid = server->arg("ssid_other"); ssid.trim();
  if (!ssid.length()) ssid = server->arg("ssid");
  String pass = server->arg("pass");
  String tiles = server->arg("tiles"); tiles.trim();
  if (!ssid.length()) { server->send(400, "text/html", "<meta charset=utf-8><p>Please choose or type a network name. <a href=/>Back</a></p>"); return; }
  strlcpy(s.ssid, ssid.c_str(), sizeof s.ssid);
  strlcpy(s.pass, pass.c_str(), sizeof s.pass);
  if (tiles.length()) strlcpy(s.tiles, tiles.c_str(), sizeof s.tiles);
  prov_save(s);
  server->send(200, "text/html", "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><body style='font-family:system-ui;padding:24px'>"
               "<h2>Saved</h2><p>Connecting to <b>" + htmlEscape(ssid) + "</b>. The device is restarting; you can close this page and reconnect your phone to your own WiFi.</p></body>");
  if (statusLbl) { lv_label_set_text_fmt(statusLbl, "Saved. Connecting to\n%s ...", s.ssid); lv_timer_handler(); }
  delay(1500);
  ESP.restart();
}

// Captive-portal probes from phones/laptops: answer with a redirect to our page.
static void handleNotFound() {
  server->sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server->send(302, "text/plain", "");
}

static void buildScreen() {
  scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xF2EFE9), 0);
  lv_obj_t* t = lv_label_create(scr);
  lv_label_set_text(t, "WiFi setup");
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t* qr = lv_qrcode_create(scr);
  lv_qrcode_set_size(qr, 150);
  lv_qrcode_set_dark_color(qr, lv_color_black());
  lv_qrcode_set_light_color(qr, lv_color_hex(0xF2EFE9));
  String wifi = String("WIFI:T:nopass;S:") + AP_SSID + ";;";
  lv_qrcode_update(qr, wifi.c_str(), wifi.length());
  lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 40);

  lv_obj_t* l = lv_label_create(scr);
  lv_label_set_text(l, "1. Scan with your phone, or join WiFi\n   \"CheekoMaps-Setup\"\n2. A setup page opens\n   (or go to 192.168.4.1)\n3. Choose your WiFi, save");
  lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
  lv_obj_align(l, LV_ALIGN_TOP_LEFT, 8, 198);

  statusLbl = lv_label_create(scr);
  lv_label_set_text(statusLbl, "Waiting for a phone...");
  lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(statusLbl, lv_color_hex(0x1E3A5F), 0);
  lv_obj_align(statusLbl, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_screen_load(scr);
}

void prov_start(const Settings& current) {
  if (active) return;
  cur = current;
  active = true;
  Serial.println("[prov] starting setup portal");
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_AP_STA);                    // STA kept so we can scan while the AP is up
  buildScanList();
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID);
  dns = new DNSServer();
  dns->start(53, "*", WiFi.softAPIP());
  server = new WebServer(80);
  server->on("/", handleRoot);
  server->on("/save", HTTP_POST, handleSave);
  server->on("/generate_204", handleNotFound);       // Android
  server->on("/hotspot-detect.html", handleNotFound); // iOS/macOS
  server->on("/ncsi.txt", handleNotFound);            // Windows
  server->on("/connecttest.txt", handleNotFound);
  server->on("/fwlink", handleNotFound);
  server->onNotFound(handleNotFound);
  server->begin();
  buildScreen();
}

void prov_loop() {
  if (!active) return;
  dns->processNextRequest();
  server->handleClient();
  static uint32_t last = 0;
  if (millis() - last > 1000) {
    last = millis();
    int n = WiFi.softAPgetStationNum();
    if (statusLbl) lv_label_set_text_fmt(statusLbl, n ? "Phone connected - open the setup page" : "Waiting for a phone...");
  }
}

bool prov_active() { return active; }
