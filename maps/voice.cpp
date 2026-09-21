#include "voice.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP_I2S.h>
#include <ArduinoJson.h>
#include <AudioBoard.h>
#include <math.h>
#include "board_config.h"
#include "secrets.h"

using namespace audio_driver;

// ---- tuning ---------------------------------------------------------------------
#define GROQ_HOST      "api.groq.com"
#define STT_MODEL      "whisper-large-v3"
#define LLM_MODEL      "openai/gpt-oss-120b"
#define SAMPLE_RATE    16000
#define MAX_SECONDS    10
#define CHUNK          1024              // bytes of 16-bit mono per network chunk
#define SILENCE_RMS    400
#define MIC_GAIN_SHIFT 2                 // software gain on top of the ES7210 PGA

static String regionCity = "the city";
static String regionHints = "";
void voice_set_region(const char* city, const char* hintNames) { if (city) regionCity = city; if (hintNames) regionHints = hintNames; }

static String systemPrompt() {
  return "You control a street map of " + regionCity + " on a small device. "
  "The user speaks a short command; the text you get is a speech transcript and may misspell local names - "
  "correct them to their common spelling. Known places include: " + regionHints + ". "
  "Reply with ONLY a JSON object, no prose, one of:\n"
  "{\"action\":\"goto\",\"place\":\"<place, area, road or landmark name only, no city suffix>\",\"zoom\":<11-16 or null>}\n"
  "{\"action\":\"zoom\",\"delta\":<-3..3>}   for zoom in/out requests\n"
  "{\"action\":\"pan\",\"dir\":\"north|south|east|west\"}\n"
  "{\"action\":\"say\",\"text\":\"<max 12 words>\"}   when the request is not a map command.\n"
  "Pick zoom 15-16 for a specific road/landmark, 13-14 for an area/neighbourhood, 11-12 for the whole city.";
}

// ---- hardware -----------------------------------------------------------------------
class CheekoPinsClass : public DriverDeviceInfo {
public:
  CheekoPinsClass() {
    // 4th arg lands in the record's *address* field (library quirk): -1 = let each codec use its default (ES8311 0x18, ES7210 0x40).
    // active=false: Wire is already begun by the sketch.
    addI2C(PinFunction::CODEC, PIN_I2C_SCL, PIN_I2C_SDA, -1, 400000, Wire, false);
    addI2S(PinFunction::CODEC, PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN);
  }
};
static CheekoPinsClass cheekoPins;
static AudioBoard board(AudioDriverES8311_ES7210, cheekoPins);
static I2SClass i2s;
static bool audioOk = false;

// ---- task state ---------------------------------------------------------------------
static TaskHandle_t voiceTask = nullptr;
static volatile VoiceState state = VS_OFF;
static volatile bool stopFlag = false, resultReady = false;
static VoiceCommand result;
static WiFiClientSecure net;
static char testText[120] = "";

void voice_beep(uint16_t hz, uint16_t ms) {
  if (!audioOk) { Serial.println("[voice] beep: audio not ready"); return; }

  static int16_t buf[512];
  uint32_t total = (uint32_t)SAMPLE_RATE * ms / 1000, done = 0;
  while (done < total) {
    uint32_t n = min<uint32_t>(256, total - done);
    for (uint32_t i = 0; i < n; i++) {
      float env = 1.0f; uint32_t k = done + i;
      if (k < 160) env = k / 160.0f; else if (total - k < 160) env = (total - k) / 160.0f;
      int16_t v = (int16_t)(sinf(2 * M_PI * hz * k / SAMPLE_RATE) * 6000 * env);
      buf[2 * i] = v; buf[2 * i + 1] = v;
    }
    size_t w = i2s.write((uint8_t*)buf, n * 4);
    if (w != n * 4 && ms >= 500) Serial.printf("[voice] i2s.write %u/%u\n", (unsigned)w, (unsigned)(n * 4));
    done += n;
  }
}

// ---- helpers from groqmic -----------------------------------------------------------------
static bool sendChunk(const uint8_t* p, size_t n) {
  char hdr[16];
  int hn = snprintf(hdr, sizeof hdr, "%X\r\n", (unsigned)n);
  return net.write((const uint8_t*)hdr, hn) == (size_t)hn && net.write(p, n) == n && net.write((const uint8_t*)"\r\n", 2) == 2;
}

static void wavHeader(uint8_t* h) {
  const uint32_t UNKNOWN = 0xFFFFFFFF, rate = SAMPLE_RATE, bps = rate * 2, fmtLen = 16;
  const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
  memcpy(h + 0, "RIFF", 4); memcpy(h + 4, &UNKNOWN, 4); memcpy(h + 8, "WAVEfmt ", 8);
  memcpy(h + 16, &fmtLen, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &rate, 4); memcpy(h + 28, &bps, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &UNKNOWN, 4);
}

static String readReply(uint32_t timeoutMs) {
  String r; uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline && (net.connected() || net.available())) {
    while (net.available()) { r += (char)net.read(); deadline = millis() + timeoutMs; }
    delay(4);
  }
  net.stop();
  return r;
}
static String bodyOf(const String& reply) { int sep = reply.indexOf("\r\n\r\n"); return sep > 0 ? reply.substring(sep + 4) : reply; }
static String jsonEsc(const String& in) {
  String o; o.reserve(in.length() + 8);
  for (char c : in) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if (c == '\n') o += "\\n"; else if ((uint8_t)c >= 0x20) o += c; }
  return o;
}

// Streams the mic to Whisper until voice_stop() or MAX_SECONDS. Returns transcript.
static String listen(uint32_t& msStt) {
  msStt = 0;
  net.setInsecure(); net.setTimeout(20);
  if (!net.connect(GROQ_HOST, 443)) { Serial.println("[voice] connect failed"); return ""; }
  const char* B = "----mapsvoice";
  String pre = String("--") + B + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" STT_MODEL "\r\n"
               "--" + B + "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nen\r\n"
               "--" + B + "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\ntext\r\n"
               "--" + B + "\r\nContent-Disposition: form-data; name=\"prompt\"\r\n\r\n" + regionCity + " place names: " + regionHints + ".\r\n"
               "--" + B + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  net.print(String("POST /openai/v1/audio/transcriptions HTTP/1.1\r\nHost: " GROQ_HOST "\r\nAuthorization: Bearer ") + SEED_GROQ_KEY +
            "\r\nUser-Agent: CheekoMaps/1.0\r\nContent-Type: multipart/form-data; boundary=" + B + "\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
  bool ok = sendChunk((const uint8_t*)pre.c_str(), pre.length());
  uint8_t hdr[44]; wavHeader(hdr);
  ok = ok && sendChunk(hdr, sizeof hdr);

  static int16_t raw[CHUNK];                 // CHUNK/2 stereo frames
  static int16_t mono[CHUNK / 2];
  uint32_t t0 = millis(), peak = 0, nSamp = 0; int64_t sumSq = 0; size_t total = 0;
  // drain whatever the DMA buffered before the button press
  while (i2s.available() > 0 && millis() - t0 < 50) i2s.readBytes((char*)raw, sizeof raw);
  // push-to-talk: record exactly while the button is held (tiny floor so a bounce doesn't send nothing)
  while (ok && millis() - t0 < MAX_SECONDS * 1000UL) {
    if (stopFlag && millis() - t0 > 250) break;
    size_t nraw = i2s.readBytes((char*)raw, sizeof raw);
    size_t frames = nraw / 4;
    if (!frames) { delay(1); continue; }
    for (size_t i = 0; i < frames; i++) {
      int32_t v = (int32_t)raw[2 * i] << MIC_GAIN_SHIFT;      // left = mic 1
      if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
      mono[i] = (int16_t)v;
      uint32_t a = v < 0 ? -v : v; if (a > peak) peak = a;
      sumSq += (int32_t)v * v; nSamp++;
    }
    ok = sendChunk((const uint8_t*)mono, frames * 2);
    total += frames * 2;
  }
  uint32_t tRel = millis();
  int32_t rms = nSamp ? (int32_t)sqrt((double)sumSq / nSamp) : 0;
  Serial.printf("[voice] recorded %u bytes in %lu ms, peak %lu, rms %ld\n", (unsigned)total, (unsigned long)(tRel - t0), (unsigned long)peak, (long)rms);
  if (rms < SILENCE_RMS) { net.stop(); Serial.println("[voice] too quiet"); return ""; }
  String post = String("\r\n--") + B + "--\r\n";
  ok = ok && sendChunk((const uint8_t*)post.c_str(), post.length()) && net.write((const uint8_t*)"0\r\n\r\n", 5) == 5;
  if (!ok) { net.stop(); Serial.println("[voice] upload aborted"); return ""; }
  String reply = readReply(20000);
  msStt = millis() - tRel;
  if (reply.indexOf("200 OK") < 0) { Serial.printf("[voice] stt failed: %s\n", reply.substring(0, 160).c_str()); return ""; }
  String text = bodyOf(reply); text.trim();
  return text;
}

static bool think(const String& heard, VoiceCommand& cmd) {
  uint32_t t0 = millis();
  net.setInsecure(); net.setTimeout(20);
  if (!net.connect(GROQ_HOST, 443)) return false;
  String body = String("{\"model\":\"" LLM_MODEL "\",\"temperature\":0.1,\"max_tokens\":120,\"reasoning_effort\":\"low\","
                       "\"response_format\":{\"type\":\"json_object\"},"
                       "\"messages\":[{\"role\":\"system\",\"content\":\"") + jsonEsc(systemPrompt()) + "\"},"
                "{\"role\":\"user\",\"content\":\"" + jsonEsc(heard) + "\"}]}";
  net.print(String("POST /openai/v1/chat/completions HTTP/1.1\r\nHost: " GROQ_HOST "\r\nAuthorization: Bearer ") + SEED_GROQ_KEY +
            "\r\nUser-Agent: CheekoMaps/1.0\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + String(body.length()) + "\r\n\r\n");
  net.print(body);
  String payload = bodyOf(readReply(30000));
  cmd.msLlm = millis() - t0;
  JsonDocument doc;
  if (deserializeJson(doc, payload)) { Serial.printf("[voice] bad json: %s\n", payload.substring(0, 160).c_str()); return false; }
  if (doc["error"]["message"].is<const char*>()) { Serial.printf("[voice] llm error: %s\n", (const char*)doc["error"]["message"]); return false; }
  const char* content = doc["choices"][0]["message"]["content"] | "";
  Serial.printf("[voice] llm: %s\n", content);
  JsonDocument c;
  if (deserializeJson(c, content)) return false;
  const char* action = c["action"] | "none";
  if (!strcmp(action, "goto")) {
    cmd.action = VoiceCommand::GOTO;
    strlcpy(cmd.place, c["place"] | "", sizeof cmd.place);
    cmd.zoom = c["zoom"].is<float>() ? (float)c["zoom"] : 0;
  } else if (!strcmp(action, "zoom")) {
    cmd.action = VoiceCommand::ZOOM; cmd.delta = c["delta"] | 0;
  } else if (!strcmp(action, "pan")) {
    cmd.action = VoiceCommand::PAN; strlcpy(cmd.dir, c["dir"] | "", sizeof cmd.dir);
  } else {
    cmd.action = VoiceCommand::SAY; strlcpy(cmd.say, c["text"] | "", sizeof cmd.say);
  }
  return true;
}

static void voiceTaskFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    VoiceCommand cmd;
    String heard;
    if (testText[0]) { heard = testText; testText[0] = 0; }
    else {
      state = VS_LISTENING;
      voice_beep(1200, 60);
      heard = listen(cmd.msStt);
      voice_beep(600, 60);
    }
    strlcpy(cmd.heard, heard.c_str(), sizeof cmd.heard);
    if (heard.length()) {
      state = VS_THINKING;
      if (!think(heard, cmd)) { cmd.action = VoiceCommand::SAY; strlcpy(cmd.say, "Could not reach Groq", sizeof cmd.say); }
    } else {
      cmd.action = VoiceCommand::SAY; strlcpy(cmd.say, "Didn't catch that", sizeof cmd.say);
    }
    Serial.printf("[voice] heard \"%s\" -> action %d place \"%s\" (stt %lu ms, llm %lu ms), heap %u KB\n", cmd.heard, cmd.action, cmd.place,
                  (unsigned long)cmd.msStt, (unsigned long)cmd.msLlm, ESP.getFreeHeap() >> 10);
    result = cmd;
    resultReady = true;
    state = VS_IDLE;
  }
}

bool voice_begin() {
  AudioDriverLogger.begin(Serial, AudioDriverLogLevel::Warning);
  CodecConfig cfg;
  cfg.input_device = ADC_INPUT_LINE1;
  cfg.output_device = DAC_OUTPUT_ALL;
  cfg.i2s.bits = BIT_LENGTH_16BITS;
  cfg.i2s.rate = RATE_16K;
  cfg.i2s.channels = CHANNELS2;
  cfg.i2s.fmt = I2S_NORMAL;
  cfg.i2s.mode = MODE_SLAVE;              // codecs are slaves; the ESP32 drives MCLK/BCLK/WS
  if (!board.begin(cfg)) { Serial.println("[voice] codec init failed"); return false; }
  board.setVolume(70);
  board.setInputVolume(80);

  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[voice] I2S init failed"); return false;
  }
  audioOk = true;
  state = VS_IDLE;
  xTaskCreatePinnedToCore(voiceTaskFn, "voice", 12 * 1024, nullptr, 2, &voiceTask, 0);
  Serial.printf("[voice] ready, heap %u KB\n", ESP.getFreeHeap() >> 10);
  return true;
}

void voice_start() {
  if (state != VS_IDLE || WiFi.status() != WL_CONNECTED) return;
  stopFlag = false;
  xTaskNotifyGive(voiceTask);
}
void voice_stop() { stopFlag = true; }
void voice_test_text(const char* text) {
  if (state != VS_IDLE) return;
  strlcpy(testText, text, sizeof testText);
  xTaskNotifyGive(voiceTask);
}
bool voice_poll(VoiceCommand& out) {
  if (!resultReady) return false;
  out = result; resultReady = false; return true;
}
VoiceState voice_state() { return state; }
