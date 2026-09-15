#pragma once
#include <stdint.h>

// ST7735 128x160 portrait. Change these local defaults for a different panel.
#ifndef VOICE_DISPLAY_CS
#define VOICE_DISPLAY_CS 47
#endif
#ifndef VOICE_DISPLAY_DC
#define VOICE_DISPLAY_DC 45
#endif
#ifndef VOICE_DISPLAY_RST
#define VOICE_DISPLAY_RST 14
#endif
#ifndef VOICE_DISPLAY_MOSI
#define VOICE_DISPLAY_MOSI 41
#endif
#ifndef VOICE_DISPLAY_SCLK
#define VOICE_DISPLAY_SCLK 42
#endif
#ifndef VOICE_DISPLAY_BL
#define VOICE_DISPLAY_BL 21
#endif
#ifndef VOICE_DISPLAY_TAB
#define VOICE_DISPLAY_TAB INITR_BLACKTAB
#endif
#ifndef VOICE_DISPLAY_ROTATION
#define VOICE_DISPLAY_ROTATION 0
#endif
#ifndef VOICE_DISPLAY_SPI_HZ
#define VOICE_DISPLAY_SPI_HZ 16000000
#endif

namespace voice_display {
enum class State : uint8_t {
  Booting, Connecting, SyncingClock, Ready, StartingSession, AwaitingSpeech, Listening,
  Recognizing, Thinking, PreparingSpeech, Speaking, TimedOut, Error
};

// Call once from setup. Allocates the canvas and starts a low-priority task;
// true means resources were created, not that an SPI-only panel was detected.
bool begin();

// Bounded copies only: no drawing, SPI, heap allocation, or blocking queue waits.
// Main owns generation tags: increment for session start, stop/timeout, and
// every actual utterance. Older nonzero tags are ignored. Turn zero is for
// boot/network status; asynchronous utterance work must pass its generation.
// Ready (inactive), StartingSession, AwaitingSpeech and TimedOut retain the
// conversation, clear errors and close their generation to late work. The next
// utterance needs a newer generation. Listening clears old text once; delayed
// same-turn Listening cannot erase a transcript or downgrade a later phase.
void setState(State state, uint32_t turn = 0);
void setUserText(const char* text, uint32_t turn = 0);
void setReplyText(const char* text, uint32_t turn = 0);
void setError(const char* text, uint32_t turn = 0);
}  // namespace voice_display
