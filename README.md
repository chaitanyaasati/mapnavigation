# mapnavigation

Vector street maps with voice search on a **Cheeko Gotchi V2** — the ESP32‑S3
"AI companion" kit with a 240×296 touch display, microphones and WiFi.

The map is drawn from **OpenStreetMap vector data**, not pictures: roads,
buildings, water and parks are rasterised on the device with anti‑aliasing, so
they stay crisp at every zoom level. Tiles come over WiFi from a tiny static
tile server and are cached on the device. Say *"take me to Koramangala"* and the
map flies there.

| | |
|---|---|
| Rendering | custom anti‑aliased rasteriser (`maps/raster.cpp`) into an off‑screen canvas, LVGL 9 UI |
| Data | OSM extract → ~10k lean binary tiles (52 MB for Bengaluru) + an offline place index |
| Delivery | HTTP(S) from any static host — GitHub Pages by default — PSRAM LRU + 12 MB flash cache on the device |
| Voice | push‑to‑talk → Groq Whisper → LLM → JSON map command → local geocoder |
| WiFi | captive‑portal setup with a QR code; settings stored in flash |

---

## Using the device

**First power‑on / new WiFi.** If the device cannot reach a known WiFi within
25 s it shows *WiFi setup* with a QR code and opens the hotspot
**`CheekoMaps-Setup`**. Scan the QR with a phone (or join that WiFi). The setup
page pops up automatically (if not, open `http://192.168.4.1`). Choose your
network, type the password, tap **Save & connect**. The device restarts on your
WiFi and the map appears. To change WiFi later, hold **VOL−** for 3 s (or hold
it while powering on).

**Controls**

| Action | Result |
|---|---|
| Drag | pan |
| Double‑tap | zoom in (around the tap) |
| Long‑press | zoom out |
| VOL+ / VOL− (short) | zoom in / out |
| VOL− held 3 s | WiFi setup |
| Mic button (bottom‑right), **hold while speaking** | voice command |

Voice commands: *"take me to Indiranagar"*, *"show the airport"*, *"go to MG Road"*,
*"zoom out a lot"*, *"pan north"*. The button is red while recording, amber
while thinking; the result appears as a message at the top.

The status line at the bottom‑left shows zoom, WiFi state, last render time
and free heap.

---

## Repository layout

```
maps/                 Arduino sketch (LVGL 9 + LovyanGFX)
  maps.ino              setup/loop, gestures, status, serial console
  map_view.*            viewport maths, render task, canvas swap, labels
  renderer.*  tile.h    VT01 tile reader and drawing passes
  raster.*              anti-aliased polygon/polyline rasteriser (RGB565)
  tile_store.*          HTTP fetch, PSRAM LRU, LittleFS cache, prefetch
  voice.*               ES7210/ES8311 codec, Whisper upload, LLM command parsing
  geocode.*             places.bin lookup
  provision.*           NVS settings + captive-portal WiFi setup
  touch_cst810.h        CST810 touch over Wire
  lgfx_setup.h          JD9853 panel config
  board_config.h        pin map (recovered from the stock firmware)
  style.h               GENERATED from tools/style.json
  lv_conf.h             LVGL configuration (sketch-local)
  partitions.csv        3.5 MB app + 12 MB LittleFS tile cache
  secrets.h.example     WiFi / Groq key / tile server defaults
tools/                Map data pipeline (Python 3.12)
  build_tiles.py        OSM .pbf -> tiles  (format: tile_format.md)
  build_places.py       OSM .pbf -> places.bin geocoder index
  style.json            shared map style;  gen_style_h.py -> maps/style.h
  serve.py              HTTP/1.1 keep-alive tile server for development
  inspect_tile.py       dump a tile's structure
  hosttest/             builds the device renderer on the Mac (render_tile: any lon/lat/zoom -> image)
hw_probe/             hardware discovery sketch + decode_gpio_matrix.py
build.sh              compile / flash helper
```

---

## Developer setup

### 1. Toolchain (once per machine, macOS)

```bash
brew install arduino-cli osmium-tool
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli config set library.enable_unsafe_install true
arduino-cli core update-index && arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install lvgl@9.6.0 LovyanGFX ArduinoJson
arduino-cli lib install --git-url https://github.com/pschatzmann/arduino-audio-driver
```

### 2. Settings

```bash
git clone https://github.com/chaitanyaasati/mapnavigation.git && cd mapnavigation
cp maps/secrets.h.example maps/secrets.h
```
Edit `maps/secrets.h`: `SEED_WIFI_SSID` / `SEED_WIFI_PASS` (factory defaults;
the portal overrides them), `SEED_GROQ_KEY` (voice), `TILE_SERVER`
(e.g. `http://My-Mac.local:8000/bengaluru` — a `.local` name is resolved via
mDNS at boot, so a DHCP address change on the server does not break the device;
`scutil --get LocalHostName` prints a Mac's name). `secrets.h` is git‑ignored.

### 3. Flash

Plug the board in with a **data** cable, directly into the Mac (USB hubs drop
its native USB port), then:

```bash
./build.sh maps upload        # port defaults to /dev/cu.usbmodem1101; PORT=/dev/cu.xxx to override
./build.sh maps               # compile only
```
`build.sh` pins the FQBN (ESP32‑S3, OPI PSRAM, 16 MB, hardware CDC), builds at
`-O2`, and picks up the sketch‑local `partitions.csv` and `lv_conf.h`.
If the board does not respond to the upload, hold **VOL+** (wired to BOOT)
while powering it on to enter download mode.

### 4. Map data

```bash
cd tools
uv venv -p 3.12 .venv && uv pip install -p .venv/bin/python osmium shapely numpy pillow
curl -LO https://download.geofabrik.de/asia/india/southern-zone-latest.osm.pbf      # any Geofabrik extract
osmium extract -b 77.40,12.78,77.85,13.25 -s smart -o bengaluru.osm.pbf southern-zone-latest.osm.pbf
.venv/bin/python build_tiles.py  bengaluru.osm.pbf out/bengaluru --bbox 77.40 12.78 77.85 13.25   # ~2 min, 52 MB
.venv/bin/python build_places.py bengaluru.osm.pbf out/bengaluru/places.bin --bbox 77.40 12.78 77.85 13.25
.venv/bin/python serve.py --port 8000 --dir out        # device fetches <TILE_SERVER>/z/x/y.bin and /places.bin
```
Other cities: same commands with a different bbox and output name; set
`TILE_SERVER` accordingly. `HOME_LON/LAT` in `maps.ino` is the start position.

**Hosting on GitHub Pages (the default).** The tiles are plain static files, so
they live in a second public repo served by Pages:
[mapnavigation-tiles](https://github.com/chaitanyaasati/mapnavigation-tiles) →
`https://chaitanyaasati.github.io/mapnavigation-tiles/bengaluru`. To publish a
new build: copy `out/<region>` into that repo, commit, push — Pages redeploys
in a few minutes. Pages is HTTPS-only; the device keeps one TLS connection open
while tiles are flowing and closes it when idle or when the mic is pressed
(TLS needs ~40 KB of RAM that voice also needs). `tools/serve.py` remains the
fast LAN option for development (`http://<mac-name>.local:8000/<region>`).

### Host preview (no board needed)

```bash
tools/hosttest/build.sh
tools/hosttest/render_tile tools/out/bengaluru 77.6094 12.9752 15 240 296 view.ppm
```
Renders with the exact device code — use it to check style/pipeline changes.

### Serial console (115200 baud)

`z <zoom>` · `g <lon> <lat> [zoom]` · `f <place>` geocode · `v <text>` run the
LLM/command path on typed text · `s` stats · `b <0‑255>` brightness ·
`p` test tone · `L` re‑init LCD · `w` open WiFi setup · `t <url>` set the tile
server (saved to flash).

---

## How it works

**Tiles.** `build_tiles.py` walks an OSM `.pbf` once, keeps what the map draws
(roads by class, water, landuse, buildings, rail, places, POIs), simplifies per
zoom (z10–16) and writes one small file per tile: int16 tile‑local coordinates,
grouped by layer/class in draw order, with a string table for names
(`tools/tile_format.md`). A dense city‑centre tile is 5–20 KB.

**Rendering.** The render task (core 0) fetches the tiles covering a
360×444 canvas (screen + 50 % margin) and draws them in passes — areas, lines,
road casings, road fills — with a signed‑area coverage rasteriser (the
font‑rs / stb_truetype technique): exact‑area anti‑aliasing, strokes built from
quads + round joints, no per‑shape heap. LVGL (core 1) shows the finished
canvas as an image, pans it instantly by offset while you drag and scales it
while you zoom; the next render replaces it when ready. Labels are LVGL
labels placed with a greedy collision check.

**Voice.** Mic audio (ES7210 → I2S) is streamed as a WAV to Groq Whisper while
you hold the button; the transcript goes to `openai/gpt-oss-120b` with a prompt
that returns `{"action":"goto","place":…}` / `zoom` / `pan`; the place is looked
up in `places.bin` (47k Bengaluru places, POIs, campuses and roads) and the map
jumps. Typical: STT ~250 ms, LLM ~1.1 s.

**Measured render times** (full canvas): z10–12 30–120 ms, z13 ~250 ms, dense
z14–16 350–600 ms. Uncached areas add 100–400 ms of download on a LAN.
LVGL's ThorVG vector backend was tried first and rejected (~1 ms and ~8 KB heap
per stroked polyline on this chip).

---

## Hardware

The board ("CGOTCHI ESP32‑S3 AI Development Kit", Cheeko Gotchi V2) has no
public pin map. `board_config.h` was recovered by dumping the GPIO matrix of the
stock firmware over the built‑in USB‑JTAG (`hw_probe/decode_gpio_matrix.py`) and
confirmed with the `hw_probe` sketch.

| Part | Pins |
|---|---|
| ESP32‑S3 | 16 MB quad flash, 8 MB octal PSRAM, native USB (`/dev/cu.usbmodem*`) |
| LCD JD9853 240×296 | SPI2: SCLK 9, MOSI 10, CS 14, DC 8, RST 17; backlight PWM 13; mounted rotated 180°, BGR, not inverted |
| Touch CST810 | I2C 0x15 on SDA 12 / SCL 11; mounted landscape (`x = 239 − raw_y`, `y = raw_x`); no INT |
| Audio | ES8311 DAC 0x18, ES7210 ADC 0x40; I2S0 MCLK 5, BCLK 15, WS 16, DOUT 6, DIN 7 |
| Keys | VOL+ = GPIO40 (also GPIO0/BOOT), VOL− = GPIO39; power key is hardware |
| Other | LIS2DH‑class IMU 0x19; BQ27220 gauge 0x55 (silent without a battery); no microSD slot |

The original 16 MB firmware image was backed up before the first flash
(kept outside the repo); `esptool write-flash 0 <image>` restores it.

### Known issues

- **Backlight switches off after a while** — also happens with the original
  firmware; only a power‑cycle brings it back. The ESP keeps running and the
  backlight PWM keeps running, so it is the board's power circuitry, not
  firmware. LCD re‑init and GPIO2/GPIO4 manipulation do not restore it.
- **Speaker is silent.** The ES8311 is configured and I2S clocks are proven by
  the working mic path, but the amplifier's enable line was not found (GPIO
  1/2/3/4/18/21/38/41/42/47/48 tried). Voice feedback is on screen.
- GPIO2 must be driven LOW as the stock firmware does; left floating the board
  powers off after ~30 min, driven HIGH for seconds it powers off at once.
- If voice says *Not found* for everything, the device could not download
  `places.bin` — check the tile server is reachable (status line shows `...`
  when tiles are pending).
- `audio-driver`: `addI2C(function, scl, sda, port, …)` stores `port` in the
  I2C *address* field — pass `-1` or the ES7210 is addressed at 0x00.

---

## Data

Map data © OpenStreetMap contributors (ODbL). Speech and language models via Groq.
