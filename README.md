# maps — vector maps on the Cheeko Gotchi V2 (ESP32-S3)

An offline-style **vector** map viewer (OpenStreetMap data, anti-aliased, crisp at
any zoom) for the "CGOTCHI ESP32-S3 AI Development Kit" (= Cheeko Gotchi V2:
240×296 touch display, mics, speaker, WiFi). Tiles are fetched over WiFi from a
tiny static tile server and cached in PSRAM + flash. Voice search goes through
Groq (Whisper → LLM) and an offline place index built from the same OSM data.

```
OSM .pbf ──build_tiles.py──▶ tiles/z/x/y.bin ─┐
         ──build_places.py─▶ places.bin        ├── serve.py (HTTP/1.1) ──WiFi──▶ ESP32-S3
                                               ┘        tile_store (PSRAM LRU + LittleFS) → renderer (raster.cpp) → LVGL
```

## Layout

| Path | What |
|---|---|
| `maps/` | Arduino sketch (LVGL 9 + LovyanGFX). `raster.cpp` is the AA rasteriser, `renderer.cpp` draws tiles, `map_view.cpp` owns the viewport/render task, `tile_store.cpp` fetches+caches, `voice.cpp` push-to-talk, `geocode.cpp` place lookup. |
| `maps/board_config.h` | Pin map of the board (recovered from the stock firmware, see below). |
| `tools/build_tiles.py` | OSM `.pbf` → tiles (format: `tools/tile_format.md`). |
| `tools/build_places.py` | OSM `.pbf` → `places.bin` geocoder index. |
| `tools/style.json` → `tools/gen_style_h.py` → `maps/style.h` | Shared map style. |
| `tools/serve.py` | Keep-alive tile server for development. |
| `tools/hosttest/` | Builds the device renderer on the Mac (`render_tile` renders any lon/lat/zoom to a PPM with the exact device code). |
| `hw_probe/` | Hardware discovery sketch + `decode_gpio_matrix.py` (reads a running firmware's pin routing over USB-JTAG). |

## Installing on the device (from a fresh checkout)

```bash
git clone https://github.com/chaitanyaasati/mapnavigation.git
cd mapnavigation

# 1. toolchain + libraries (once per machine)
brew install arduino-cli osmium-tool
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli config set library.enable_unsafe_install true
arduino-cli core update-index && arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install lvgl@9.6.0 LovyanGFX ArduinoJson
arduino-cli lib install --git-url https://github.com/pschatzmann/arduino-audio-driver

# 2. your settings
cp maps/secrets.h.example maps/secrets.h     # WiFi SSID/password, Groq key, TILE_SERVER=http://<server-ip>:8000/bengaluru

# 3. plug the board in with a DATA cable, directly into the Mac (hubs drop its USB), then
./build.sh maps upload                        # port defaults to /dev/cu.usbmodem1101; override with PORT=/dev/cu.xxx
```
If the board is running a sketch that is not responding, hold the VOL+ (BOOT)
button while powering it on to enter download mode, then run the upload.

To go back to the original Cheeko firmware:
`esptool --port /dev/cu.usbmodem1101 write-flash 0 ~/Documents/Arduino/maps-backup/cheeko_stock_16MB.bin`
(the backup is not in this repo).

## Build / flash (day to day)

```bash
./build.sh maps            # compile only
./build.sh maps upload     # compile + flash
```
`build.sh` pins the FQBN (ESP32-S3, OPI PSRAM, 16 MB, hardware CDC), builds at
-O2, and the sketch-local `maps/partitions.csv` (3.5 MB app + 12 MB LittleFS
tile cache) is picked up automatically. `maps/lv_conf.h` lives in the sketch.

## Map data

```bash
cd tools && uv venv -p 3.12 .venv && uv pip install -p .venv/bin/python osmium shapely numpy pillow
osmium extract -b 77.40,12.78,77.85,13.25 -s smart -o bengaluru.osm.pbf southern-zone-latest.osm.pbf
.venv/bin/python build_tiles.py bengaluru.osm.pbf out/bengaluru --bbox 77.40 12.78 77.85 13.25   # ~2 min, 52 MB
.venv/bin/python build_places.py bengaluru.osm.pbf out/bengaluru/places.bin --bbox 77.40 12.78 77.85 13.25
.venv/bin/python serve.py --port 8000 --dir out                                                  # device fetches /bengaluru/z/x/y.bin
```
Any static host works for the tile folder (it is plain files).

## Controls

Drag = pan · double-tap = zoom in · long-press = zoom out · VOL+/VOL− = zoom ·
mic button: press and hold while speaking (release = stop) → "take me to Koramangala",
"zoom out", "show the airport"…

Serial console (115200): `z <zoom>`, `g <lon> <lat> [zoom]`, `f <place>` (geocode),
`v <text>` (run the LLM path on typed text), `s` (stats), `b <0-255>` brightness,
`p` test tone, `L` re-init LCD.

## Performance (measured)

Full 360×444 canvas redraw: z10–12 30–120 ms, z13 ~250 ms, dense z14–16
350–600 ms (rasteriser ≈ 60 % of that). Uncached view adds 100–400 ms of
download from a LAN server. ThorVG (LVGL's vector backend) was tried first and
rejected: ~1 ms + 8 KB heap per stroked polyline on this chip.

## Hardware notes (this board is undocumented)

Everything in `board_config.h` was recovered by dumping the GPIO matrix of the
stock firmware over the built-in USB-JTAG (`hw_probe/decode_gpio_matrix.py`) and
confirmed with `hw_probe`. The stock 16 MB flash image is backed up in
`~/Documents/Arduino/maps-backup/`; `esptool write-flash 0 <file>` restores it.

Known quirks:
- **Backlight drops out after a while** (also with the original firmware); only a
  power-cycle brings it back. The ESP keeps running. Not fixable from firmware
  as far as found (LEDC keeps running, LCD re-init / GPIO2 / GPIO4 don't help).
- **Speaker is silent**: ES8311 is configured and I2S clocks are proven by the
  mic path, but the amplifier's enable line was not found (GPIO
  1/3/4/18/21/38/41/42/47/48 and 2 tried). Voice feedback is on-screen for now.
- GPIO2 must be held LOW (stock does); floating it, the board powers off after
  ~30 min. Driving it HIGH for seconds powers the board off.
- The `audio-driver` `addI2C(function, scl, sda, port, …)` puts `port` into the
  I2C *address* field — pass `-1` or the ES7210 is addressed at 0x00.
