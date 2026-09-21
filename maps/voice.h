#pragma once
// =============================================================================
//  voice - push-to-talk map commands via Groq (Whisper -> LLM -> JSON command)
// =============================================================================
//  Audio path on the Cheeko V2: ES7210 4-mic ADC -> I2S0 DIN, ES8311 DAC <- I2S0
//  DOUT, both codecs configured over I2C (shared with the touch controller).
//  Lifted from ../groqmic.ino: the WAV is streamed to Whisper while you are
//  still talking, so nothing is buffered whole.
//
//  UI contract (all non-blocking, called from loop()):
//    voice_begin()   once, after WiFi/Wire are up
//    voice_start()   finger down on the mic button
//    voice_stop()    finger up  -> transcription + LLM continue in the task
//    voice_poll()    returns true once per finished turn with the command
//    voice_state()   for the status line
#include <stdint.h>

enum VoiceState : uint8_t { VS_OFF, VS_IDLE, VS_LISTENING, VS_THINKING, VS_ERROR };

struct VoiceCommand {
  enum Action : uint8_t { NONE, GOTO, ZOOM, PAN, SAY } action = NONE;
  char place[48] = "";      // GOTO
  float zoom = 0;           // GOTO (0 = keep), ZOOM (absolute if delta == 0)
  int delta = 0;            // ZOOM (+/-)
  char dir[8] = "";         // PAN: north/south/east/west
  char say[96] = "";        // message for the toast
  char heard[120] = "";     // Whisper transcript
  uint32_t msStt = 0, msLlm = 0;
};

bool voice_begin();
void voice_set_region(const char* city, const char* hintNames);   // e.g. "Mumbai, India", "Bandra, Andheri, Colaba, ..."
void voice_start();
void voice_stop();
bool voice_poll(VoiceCommand& out);
VoiceState voice_state();
void voice_beep(uint16_t hz, uint16_t ms);
void voice_test_text(const char* text);   // run the LLM/command path on typed text (no mic)
