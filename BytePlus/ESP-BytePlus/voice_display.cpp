#include "voice_display.h"
#include "voice_display_text.h"

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <SPI.h>
#include <atomic>
#include <new>
#include <esp_heap_caps.h>

namespace voice_display {
namespace {
constexpr uint16_t PANEL_WIDTH = (VOICE_DISPLAY_ROTATION & 1) ? 160 : 128;
constexpr uint16_t PANEL_HEIGHT = (VOICE_DISPLAY_ROTATION & 1) ? 128 : 160;
constexpr uint16_t BG = 0x0843, WHITE = 0xffff, MUTED = 0x9cd5;
constexpr uint16_t BLUE = 0x2cbb, GREEN = 0x2d89, AMBER = 0xfda5, RED = 0xe986;
constexpr uint32_t PAGE_MS = 6500;
constexpr size_t ERROR_CAPACITY = 160;
constexpr uint8_t USER_LINES = PANEL_HEIGHT == 160 ? 2 : 1;
constexpr uint8_t REPLY_LINES = PANEL_HEIGHT == 160 ? 3 : 2;

struct StateMessage { State state; uint32_t turn, epoch, revision; };
struct TextMessage { uint32_t turn, epoch; char value[text::kTextCapacity]; };
struct ErrorMessage { uint32_t turn, epoch, revision; char value[ERROR_CAPACITY]; };
StaticQueue_t stateQueueControl, userQueueControl, replyQueueControl, errorQueueControl;
uint8_t stateStorage[sizeof(StateMessage)], userStorage[sizeof(TextMessage)];
uint8_t replyStorage[sizeof(TextMessage)], errorStorage[sizeof(ErrorMessage)];
QueueHandle_t stateQueue = nullptr, userQueue = nullptr, replyQueue = nullptr, errorQueue = nullptr;
portMUX_TYPE publishLock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> started{false};
uint32_t publishedTurn = 0, publishedEpoch = 0, publishedRevision = 0;
State publishedState = State::Booting;
State closedState = State::Ready;
bool errorLatched = false, turnClosed = false;

class FrameCanvas : public GFXcanvas16 {
 public:
  FrameCanvas() : GFXcanvas16(PANEL_WIDTH, PANEL_HEIGHT, false) {
    buffer = static_cast<uint16_t*>(heap_caps_malloc(PANEL_WIDTH * PANEL_HEIGHT * 2,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) buffer = static_cast<uint16_t*>(heap_caps_malloc(PANEL_WIDTH * PANEL_HEIGHT * 2, MALLOC_CAP_8BIT));
    buffer_owned = true;
  }
};
FrameCanvas* canvas = nullptr;
SPIClass panelSPI(FSPI);
Adafruit_ST7735 panel(&panelSPI, VOICE_DISPLAY_CS, VOICE_DISPLAY_DC, VOICE_DISPLAY_RST);
U8G2_FOR_ADAFRUIT_GFX font;

// Used only by the display task. No Arduino String crosses a task boundary.
struct Model {
  State state = State::Booting;
  uint32_t turn = 0, epoch = 0, controlRevision = 0;
  char user[text::kTextCapacity] = {};
  char reply[text::kTextCapacity] = {};
  char error[ERROR_CAPACITY] = {};
  text::Layout userLines, replyLines, errorLines;
  uint16_t userPage = 0, replyPage = 0, errorPage = 0;
} model;
bool modelReset = false;  // Display task only; preserves redraws for empty new-turn messages.

bool older(uint32_t value, uint32_t reference) {
  return static_cast<int32_t>(value - reference) < 0;
}

bool sessionControl(State state) {
  return state == State::Ready || state == State::StartingSession ||
         state == State::AwaitingSpeech || state == State::TimedOut;
}

int phaseRank(State state) {
  switch (state) {
    case State::Listening: return 0;
    case State::Recognizing: return 1;
    case State::Thinking: return 2;
    case State::PreparingSpeech: return 3;
    case State::Speaking: return 4;
    default: return -1;
  }
}

int controlRank(State state) {
  if (state == State::StartingSession) return 0;
  if (state == State::AwaitingSpeech) return 1;
  return 2;  // Ready/TimedOut terminate a session generation.
}

// Called only inside the short publisher critical section. Controls adopt new
// generations without erasing conversation. Actual utterance work clears once;
// its first event may race ahead of Listening on another core.
bool adoptPublishedTurn(uint32_t turn, bool clearConversation) {
  if (!turn) return true;
  if (publishedTurn && older(turn, publishedTurn)) return false;
  if (turn != publishedTurn) {
    publishedTurn = turn;
    if (clearConversation) ++publishedEpoch;
    errorLatched = false;
    turnClosed = false;
    publishedState = State::Listening;
  }
  return true;
}

void publishState() {
  const StateMessage message{publishedState, publishedTurn, publishedEpoch, ++publishedRevision};
  xQueueOverwrite(stateQueue, &message);
}

void publishText(QueueHandle_t queue, const char* value, uint32_t turn) {
  if (!started.load(std::memory_order_acquire)) return;
  TextMessage message{};
  text::copyUtf8(message.value, sizeof(message.value), value);
  portENTER_CRITICAL(&publishLock);
  if (adoptPublishedTurn(turn, true) && !turnClosed && !errorLatched) {
    message.turn = publishedTurn;
    message.epoch = publishedEpoch;
    xQueueOverwrite(queue, &message);
    publishState();
  }
  portEXIT_CRITICAL(&publishLock);
}

bool adoptModel(uint32_t turn, uint32_t epoch) {
  if (older(epoch, model.epoch) || (model.turn && older(turn, model.turn))) return false;
  if (epoch != model.epoch) {
    modelReset = true;
    model.epoch = epoch;
    model.state = State::Listening;
    model.user[0] = model.reply[0] = model.error[0] = '\0';
    model.userLines.count = model.replyLines.count = model.errorLines.count = 0;
    model.userPage = model.replyPage = model.errorPage = 0;
  }
  // Stop/start/wait controls advance generation while retaining the text epoch.
  model.turn = turn;
  return true;
}

int16_t glyphWidth(uint16_t cp, int16_t, int16_t, void*) {
  if (!u8g2_IsGlyph(&font.u8g2, cp)) cp = '?';
  return u8g2_GetGlyphWidth(&font.u8g2, cp);
}

struct DrawOrigin { int16_t x, y; };
int16_t drawGlyph(uint16_t cp, int16_t x, int16_t y, void* context) {
  const DrawOrigin& origin = *static_cast<DrawOrigin*>(context);
  if (!u8g2_IsGlyph(&font.u8g2, cp)) cp = '?';
  font.drawGlyph(origin.x + x, origin.y + y, cp);
  return u8g2_GetGlyphWidth(&font.u8g2, cp);
}

int16_t measureText(const char* value, void*) {
  // Composition helper shared with host tests; same advances as drawText.
  return text::glyphRuns(value, glyphWidth, nullptr);
}

void drawText(int16_t x, int16_t y, const char* value, uint16_t color) {
  font.setForegroundColor(color);
  DrawOrigin origin{x, y};
  text::glyphRuns(value, drawGlyph, &origin);
}

const char* stateLabel(State state) {
  switch (state) {
    case State::Booting: return "Starting";
    case State::Connecting: return "Connecting Wi-Fi";
    case State::SyncingClock: return "Setting clock";
    case State::Ready: return "Tap to start";
    case State::StartingSession: return "Starting session";
    case State::AwaitingSpeech: return "Session on";
    case State::Listening: return "Listening";
    case State::Recognizing: return "Recognizing";
    case State::Thinking: return "Thinking";
    case State::PreparingSpeech: return "Preparing voice";
    case State::Speaking: return "Speaking";
    case State::TimedOut: return "30s silence: off";
    case State::Error: return "Please try again";
  }
  return "BytePlus";
}

uint16_t stateColor(State state) {
  if (state == State::Error) return RED;
  if (state == State::AwaitingSpeech || state == State::Speaking) return GREEN;
  if (state == State::TimedOut) return AMBER;
  if (state == State::Listening) return AMBER;
  return BLUE;
}

void label(int16_t x, int16_t y, const char* value, uint16_t color = MUTED) {
  canvas->setCursor(x, y);
  canvas->setTextColor(color);
  canvas->print(value);
}

void pageLabel(int16_t y, uint16_t page, uint16_t count) {
  if (count <= 1) return;
  char value[16];
  snprintf(value, sizeof(value), "%u/%u", unsigned(page + 1), unsigned(count));
  label(PANEL_WIDTH - 6 - strlen(value) * 6, y, value);
}

void drawPage(const char* value, const text::Layout& layout, uint16_t page,
              uint8_t perPage, int16_t baseline, uint16_t color = WHITE) {
  char line[text::kTextCapacity];
  const size_t first = page * perPage;
  for (uint8_t row = 0; row < perPage && first + row < layout.count; ++row) {
    const text::Line& selected = layout.lines[first + row];
    memcpy(line, value + selected.start, selected.length);
    line[selected.length] = '\0';
    drawText(6, baseline + row * 19, line, color);
  }
}

void render() {
  canvas->fillScreen(BG);
  canvas->fillRect(0, 0, PANEL_WIDTH, 23, stateColor(model.state));
  label(7, 8, stateLabel(model.state), BG);
  const bool landscape = PANEL_HEIGHT < 160;
  const int16_t userBaseline = landscape ? 50 : 51;
  const int16_t divider = landscape ? 57 : 77;
  const int16_t replyBaseline = landscape ? 85 : 106;
  label(6, 29, "YOU");
  pageLabel(29, model.userPage, text::pageCount(model.userLines, USER_LINES));
  if (model.user[0]) drawPage(model.user, model.userLines, model.userPage, USER_LINES, userBaseline);
  else label(6, userBaseline - 7, model.state == State::Listening ? "Listening to you..." :
      model.state == State::AwaitingSpeech ? "Speak naturally" :
      model.state == State::StartingSession ? "Getting ready..." : "Tap button to start");
  canvas->drawFastHLine(6, divider, PANEL_WIDTH - 12, BLUE);
  label(6, divider + 7, model.state == State::Error ? "ERROR" : "BYTEPLUS");
  if (model.state == State::Error) {
    pageLabel(divider + 7, model.errorPage, text::pageCount(model.errorLines, REPLY_LINES));
    if (model.error[0]) drawPage(model.error, model.errorLines, model.errorPage, REPLY_LINES, replyBaseline, RED);
    else label(6, replyBaseline - 7, "Tap to stop / retry", RED);
  } else {
    pageLabel(divider + 7, model.replyPage, text::pageCount(model.replyLines, REPLY_LINES));
    if (model.reply[0]) drawPage(model.reply, model.replyLines, model.replyPage, REPLY_LINES, replyBaseline);
    else label(6, replyBaseline - 7, model.state == State::Listening ? "Pause to send" :
        model.state == State::TimedOut ? "Tap to restart" : "Reply appears here");
  }
  const bool paged = text::pageCount(model.userLines, USER_LINES) > 1 ||
      text::pageCount(model.state == State::Error ? model.errorLines : model.replyLines, REPLY_LINES) > 1;
  if (!landscape) label(6, PANEL_HEIGHT - 8,
      model.state == State::Ready ? "Tap to start" :
      model.state == State::TimedOut ? "Tap to restart" :
      model.state == State::StartingSession || model.state == State::AwaitingSpeech || model.state == State::Listening ||
      model.state == State::Recognizing || model.state == State::Thinking ||
      model.state == State::PreparingSpeech || model.state == State::Speaking ? "Tap button to stop" :
      paged ? "Pages change / 6.5s" : "BytePlus voicebot");
  panel.drawRGBBitmap(0, 0, canvas->getBuffer(), PANEL_WIDTH, PANEL_HEIGHT);
}

void displayTask(void*) {
  pinMode(VOICE_DISPLAY_CS, OUTPUT);
  digitalWrite(VOICE_DISPLAY_CS, HIGH);
  pinMode(VOICE_DISPLAY_DC, OUTPUT);
  digitalWrite(VOICE_DISPLAY_DC, HIGH);
  pinMode(VOICE_DISPLAY_RST, OUTPUT);
  digitalWrite(VOICE_DISPLAY_RST, HIGH);
#if defined(VOICE_DISPLAY_BL) && VOICE_DISPLAY_BL >= 0
  pinMode(VOICE_DISPLAY_BL, OUTPUT);
  digitalWrite(VOICE_DISPLAY_BL, HIGH);
#endif
  // ESP32 SPIClass::begin returns immediately once initialized. Adafruit's
  // later begin() therefore keeps these custom pins instead of board defaults.
  if (!panelSPI.begin(VOICE_DISPLAY_SCLK, -1, VOICE_DISPLAY_MOSI, VOICE_DISPLAY_CS)) {
    Serial.println("[DISPLAY] SPI initialization failed; voice remains available.");
    started.store(false, std::memory_order_release);
    delete canvas;
    canvas = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  panel.initR(VOICE_DISPLAY_TAB);
  panel.setRotation(VOICE_DISPLAY_ROTATION);
  panel.setSPISpeed(VOICE_DISPLAY_SPI_HZ);
  canvas->setTextWrap(false);
  canvas->setTextSize(1);
  font.begin(*canvas);
  font.setFont(u8g2_font_etl14thai_t);
  font.setFontMode(1);
  font.setFontDirection(0);
  font.setBackgroundColor(BG);
  render();
  Serial.printf("[DISPLAY] ST7735 %ux%u ready; CS=%d DC=%d RST=%d MOSI=%d SCLK=%d BL=%d; render task ready.\n",
      unsigned(PANEL_WIDTH), unsigned(PANEL_HEIGHT), VOICE_DISPLAY_CS, VOICE_DISPLAY_DC, VOICE_DISPLAY_RST,
      VOICE_DISPLAY_MOSI, VOICE_DISPLAY_SCLK, VOICE_DISPLAY_BL);
  uint32_t pageAt = millis();
  TextMessage incoming;
  ErrorMessage incomingError;
  StateMessage incomingState;
  while (true) {
    bool dirty = false, textChanged = false;
    modelReset = false;
    if (xQueueReceive(stateQueue, &incomingState, 0) == pdTRUE) {
      const bool changed = incomingState.epoch != model.epoch || incomingState.state != model.state;
      if (!older(incomingState.revision, model.controlRevision) &&
          adoptModel(incomingState.turn, incomingState.epoch)) {
        dirty = changed;
        model.state = incomingState.state;
        model.controlRevision = incomingState.revision;
        if (sessionControl(incomingState.state)) {
          model.error[0] = '\0';
          model.errorLines.count = 0;
          model.errorPage = 0;
        }
      }
    }
    if (xQueueReceive(userQueue, &incoming, 0) == pdTRUE && adoptModel(incoming.turn, incoming.epoch)) {
      if (strcmp(model.user, incoming.value)) {
        memcpy(model.user, incoming.value, sizeof(model.user));
        text::wrap(model.user, PANEL_WIDTH - 12, measureText, nullptr, model.userLines);
        model.userPage = 0;
        dirty = textChanged = true;
      }
    }
    if (xQueueReceive(replyQueue, &incoming, 0) == pdTRUE && adoptModel(incoming.turn, incoming.epoch)) {
      if (strcmp(model.reply, incoming.value)) {
        memcpy(model.reply, incoming.value, sizeof(model.reply));
        text::wrap(model.reply, PANEL_WIDTH - 12, measureText, nullptr, model.replyLines);
        model.replyPage = 0;
        dirty = textChanged = true;
      }
    }
    if (xQueueReceive(errorQueue, &incomingError, 0) == pdTRUE &&
        !older(incomingError.revision, model.controlRevision) &&
        adoptModel(incomingError.turn, incomingError.epoch)) {
      if (strcmp(model.error, incomingError.value)) {
        memcpy(model.error, incomingError.value, sizeof(model.error));
        text::wrap(model.error, PANEL_WIDTH - 12, measureText, nullptr, model.errorLines);
        model.errorPage = 0;
        dirty = textChanged = true;
      }
      dirty = dirty || model.state != State::Error;
      model.state = State::Error;
      model.controlRevision = incomingError.revision;
    }
    dirty = dirty || modelReset;
    textChanged = textChanged || modelReset;
    if (textChanged) pageAt = millis();
    if (millis() - pageAt >= PAGE_MS) {
      uint16_t pages = text::pageCount(model.userLines, USER_LINES);
      if (pages > 1) { model.userPage = (model.userPage + 1) % pages; dirty = true; }
      if (model.state == State::Error) {
        pages = text::pageCount(model.errorLines, REPLY_LINES);
        if (pages > 1) { model.errorPage = (model.errorPage + 1) % pages; dirty = true; }
      } else {
        pages = text::pageCount(model.replyLines, REPLY_LINES);
        if (pages > 1) { model.replyPage = (model.replyPage + 1) % pages; dirty = true; }
      }
      pageAt = millis();
    }
    if (dirty) render();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
}  // namespace

bool begin() {
  if (started.load(std::memory_order_acquire)) return true;
  canvas = new (std::nothrow) FrameCanvas();
  if (!canvas || !canvas->getBuffer()) {
    Serial.printf("[DISPLAY] Could not allocate %ux%u canvas. Free heap=%u, PSRAM=%u.\n",
        unsigned(PANEL_WIDTH), unsigned(PANEL_HEIGHT), unsigned(ESP.getFreeHeap()), unsigned(ESP.getFreePsram()));
    delete canvas;
    canvas = nullptr;
    return false;
  }
  stateQueue = xQueueCreateStatic(1, sizeof(StateMessage), stateStorage, &stateQueueControl);
  userQueue = xQueueCreateStatic(1, sizeof(TextMessage), userStorage, &userQueueControl);
  replyQueue = xQueueCreateStatic(1, sizeof(TextMessage), replyStorage, &replyQueueControl);
  errorQueue = xQueueCreateStatic(1, sizeof(ErrorMessage), errorStorage, &errorQueueControl);
  if (!stateQueue || !userQueue || !replyQueue || !errorQueue) {
    Serial.println("[DISPLAY] Could not create update mailboxes.");
    delete canvas;
    canvas = nullptr;
    return false;
  }
  started.store(true, std::memory_order_release);
  if (xTaskCreate(displayTask, "voice-display", 8192, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("[DISPLAY] Could not create render task.");
    started.store(false, std::memory_order_release);
    delete canvas;
    canvas = nullptr;
    return false;
  }
  return true;
}

void setState(State state, uint32_t turn) {
  if (!started.load(std::memory_order_acquire)) return;
  portENTER_CRITICAL(&publishLock);
  const bool newTurn = turn && turn != publishedTurn;
  const bool control = sessionControl(state);
  if (adoptPublishedTurn(turn, !control)) {
    if (control) {
      // Within one generation, session startup may advance to waiting and
      // then termination. A delayed startup/wait event cannot reopen it.
      const bool lateControl = !newTurn && turnClosed &&
          (controlRank(state) < controlRank(closedState) ||
           (controlRank(closedState) == 2 && state != closedState));
      if (!lateControl) {
        errorLatched = false;
        turnClosed = true;
        closedState = state;
        publishedState = state;
        publishState();
      }
    } else if (!errorLatched && (!turnClosed || phaseRank(state) < 0)) {
      const bool latePhase = phaseRank(state) >= 0 && phaseRank(publishedState) > phaseRank(state);
      if (!latePhase) {
        if (state == State::Error) {
          // Closed session generations reject delayed work errors as well.
          if (!turnClosed) {
            errorLatched = true;
            publishedState = state;
            publishState();
          }
        } else if (state != publishedState || newTurn) {
          publishedState = state;
          publishState();
        }
      }
    }
  }
  portEXIT_CRITICAL(&publishLock);
}

void setUserText(const char* value, uint32_t turn) { publishText(userQueue, value, turn); }
void setReplyText(const char* value, uint32_t turn) { publishText(replyQueue, value, turn); }

void setError(const char* value, uint32_t turn) {
  if (!started.load(std::memory_order_acquire)) return;
  ErrorMessage message{};
  text::copyUtf8(message.value, sizeof(message.value), value);
  portENTER_CRITICAL(&publishLock);
  if (adoptPublishedTurn(turn, true) && !turnClosed) {
    message.turn = publishedTurn;
    message.epoch = publishedEpoch;
    message.revision = publishedRevision + 1;
    errorLatched = true;
    publishedState = State::Error;
    xQueueOverwrite(errorQueue, &message);
    publishState();
  }
  portEXIT_CRITICAL(&publishLock);
}
}  // namespace voice_display
