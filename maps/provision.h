#pragma once
// =============================================================================
//  provision - WiFi / server settings in flash + captive-portal setup
// =============================================================================
//  Settings live in NVS ("maps" namespace). When the saved WiFi cannot be
//  reached (or the user asks), the device opens the hotspot CheekoMaps-Setup,
//  shows a QR code on the display, and serves a one-page form at 192.168.4.1
//  (captive portal: any URL redirects there). Saving reboots onto the new WiFi.
#include <Arduino.h>
#include <lvgl.h>

struct Settings {
  char ssid[33];
  char pass[65];
  char tiles[128];       // e.g. http://192.168.0.4:8000/bengaluru
};

void prov_load(Settings& s, const char* defSsid, const char* defPass, const char* defTiles);
bool prov_save(const Settings& s);
void prov_start(const Settings& current);   // stops STA, starts AP + portal + setup screen
void prov_loop();                           // call from loop()
bool prov_active();
