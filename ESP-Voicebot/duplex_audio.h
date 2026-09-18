#pragma once

#include <Arduino.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_memory_utils.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/gpio_sig_map.h"
#include "duplex_timeline.h"
#include "hardware_pins.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "DuplexAudio clock fanout and I2S format are reviewed for ESP32-S3 only."
#endif

namespace voicebot_audio {

static bool duplexAudioSent(i2s_chan_handle_t, i2s_event_data_t*, void*);
static bool duplexAudioReceived(i2s_chan_handle_t, i2s_event_data_t*, void*);

// ESP32-S3 / Arduino 3.3.11 (IDF 5.5.5). Keep this object in internal RAM.
// One task submits postgain speaker PCM; one task receives aligned mic/ref PCM.
// begin/end belong to setup/teardown, with those tasks stopped for reinitializing.
// DMA callbacks own all I2S buffers; never use i2s_channel_read/write on the pair.
class DuplexAudio {
 public:
  bool begin() {
    if (!end()) return false;
    if (!esp_ptr_internal(this) || !esp_ptr_internal(reinterpret_cast<const uint8_t*>(this) + sizeof(*this) - 1)) return false;
    if (!wake_) wake_ = xSemaphoreCreateBinaryStatic(&wakeState_);
    if (!wake_) return false;
    while (xSemaphoreTake(wake_, 0) == pdTRUE) {}

    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    lastClockUs_ = static_cast<uint32_t>(now);
    clockMs_ = static_cast<uint32_t>(now / 1000U);
    remainderUs_ = static_cast<uint32_t>(now % 1000U);
    timeline_.reset(1, 0, clockMs_);
    invalidDmaEvents_ = 0;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = kDuplexDmaBlocks;
    channel.dma_frame_num = kDuplexSamples;
    channel.auto_clear_after_cb = false;
    channel.auto_clear_before_cb = false;
    if (i2s_new_channel(&channel, &tx_, &rx_) != ESP_OK) { end(); return false; }
    i2s_std_config_t config{};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);
    config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    config.gpio_cfg.bclk = static_cast<gpio_num_t>(voicebot_hardware::kMicrophoneBclk);
    config.gpio_cfg.ws = static_cast<gpio_num_t>(voicebot_hardware::kMicrophoneWs);
    config.gpio_cfg.dout = static_cast<gpio_num_t>(voicebot_hardware::kSpeakerData);
    config.gpio_cfg.din = static_cast<gpio_num_t>(voicebot_hardware::kMicrophoneData);
    // Initialization order is intentional: IDF makes the later RX channel a
    // slave sharing TX's physical clocks. No separate 16 kHz clock estimator.
    if (i2s_channel_init_std_mode(tx_, &config) != ESP_OK ||
        i2s_channel_init_std_mode(rx_, &config) != ESP_OK) { end(); return false; }
    i2s_event_callbacks_t txCallbacks{}, rxCallbacks{};
    txCallbacks.on_sent = &duplexAudioSent;
    rxCallbacks.on_recv = &duplexAudioReceived;
    if (i2s_channel_register_event_callback(tx_, &txCallbacks, this) != ESP_OK ||
        i2s_channel_register_event_callback(rx_, &rxCallbacks, this) != ESP_OK) { end(); return false; }
    // Fan out the same TX BCLK/WS to the amplifier's existing pins. Native I2S
    // owns the microphone clock pins; the amplifier's two matrix outputs
    // belong exclusively to us.
    fanout_ = true; // Also clean up a partially configured pair on failure.
    const gpio_num_t speakerBclk = static_cast<gpio_num_t>(voicebot_hardware::kSpeakerBclk);
    const gpio_num_t speakerWs = static_cast<gpio_num_t>(voicebot_hardware::kSpeakerWs);
    if (gpio_reset_pin(speakerBclk) != ESP_OK || gpio_reset_pin(speakerWs) != ESP_OK ||
        gpio_set_direction(speakerBclk, GPIO_MODE_OUTPUT) != ESP_OK ||
        gpio_set_direction(speakerWs, GPIO_MODE_OUTPUT) != ESP_OK) { end(); return false; }
    esp_rom_gpio_connect_out_signal(speakerBclk, I2S0O_BCK_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(speakerWs, I2S0O_WS_OUT_IDX, false, false);

    // Fresh IDF DMA buffers are zeroed. Start the timeline immediately before
    // arming RX, which then waits for TX's first shared clock edge.
    const uint64_t started = static_cast<uint64_t>(esp_timer_get_time());
    lastClockUs_ = static_cast<uint32_t>(started);
    clockMs_ = static_cast<uint32_t>(started / 1000U);
    remainderUs_ = static_cast<uint32_t>(started % 1000U);
    timeline_.reset(1, 0, clockMs_);
    running_ = true;
    if (i2s_channel_enable(rx_) != ESP_OK) { end(); return false; }
    rxEnabled_ = true;
    if (i2s_channel_enable(tx_) != ESP_OK) { end(); return false; }
    txEnabled_ = true;
    return true;
  }

  bool end() {
    bool ok = true;
    portENTER_CRITICAL(&mux_);
    running_ = false;
    portEXIT_CRITICAL(&mux_);
    // Stop the master before the slave. No callback can access released state
    // after the native channel has been disabled/deleted.
    if (txEnabled_) {
      if (i2s_channel_disable(tx_) == ESP_OK) txEnabled_ = false;
      else ok = false;
    }
    if (rxEnabled_) {
      if (i2s_channel_disable(rx_) == ESP_OK) rxEnabled_ = false;
      else ok = false;
    }
    // Retain a handle if native teardown fails, so a later call can retry; do
    // not lose an allocated channel or delete a possibly running one.
    if (tx_ && !txEnabled_) {
      if (i2s_del_channel(tx_) == ESP_OK) tx_ = nullptr;
      else ok = false;
    }
    if (rx_ && !rxEnabled_) {
      if (i2s_del_channel(rx_) == ESP_OK) rx_ = nullptr;
      else ok = false;
    }
    if (fanout_) {
      const gpio_num_t speakerBclk = static_cast<gpio_num_t>(voicebot_hardware::kSpeakerBclk);
      const gpio_num_t speakerWs = static_cast<gpio_num_t>(voicebot_hardware::kSpeakerWs);
      esp_rom_gpio_connect_out_signal(speakerBclk, SIG_GPIO_OUT_IDX, false, false);
      esp_rom_gpio_connect_out_signal(speakerWs, SIG_GPIO_OUT_IDX, false, false);
      const bool resetBclk = gpio_reset_pin(speakerBclk) == ESP_OK;
      const bool resetWs = gpio_reset_pin(speakerWs) == ESP_OK;
      if (resetBclk && resetWs) fanout_ = false;
      else ok = false;
    }
    portENTER_CRITICAL(&mux_);
    timeline_.reset();
    portEXIT_CRITICAL(&mux_);
    if (wake_) xSemaphoreGive(wake_);
    return ok;
  }

  bool submit(const int16_t* postGainMono, size_t samples, uint32_t generation) {
    portENTER_CRITICAL(&mux_);
    const bool accepted = running_ && timeline_.submit(postGainMono, samples, generation);
    portEXIT_CRITICAL(&mux_);
    return accepted;
  }
  void invalidate(uint32_t generation) {
    const uint32_t now = millis();
    portENTER_CRITICAL(&mux_);
    timeline_.invalidate(generation, now);
    portEXIT_CRITICAL(&mux_);
  }
  bool receive(AlignedAudioBlock& output, uint32_t timeoutMs) {
    const uint32_t started = millis();
    for (;;) {
      portENTER_CRITICAL(&mux_);
      const bool running = running_;
      const bool ready = running && timeline_.receive(output);
      portEXIT_CRITICAL(&mux_);
      if (ready) return true;
      const uint32_t elapsed = millis() - started;
      if (!running || elapsed >= timeoutMs || !wake_) return false;
      TickType_t ticks = pdMS_TO_TICKS(timeoutMs - elapsed);
      if (!ticks) ticks = 1;
      if (xSemaphoreTake(wake_, ticks) != pdTRUE) return false;
    }
  }
  uint32_t pendingBlocks() { portENTER_CRITICAL(&mux_); const uint32_t value = timeline_.pendingBlocks(); portEXIT_CRITICAL(&mux_); return value; }
  uint32_t startedGeneration() { portENTER_CRITICAL(&mux_); const uint32_t value = timeline_.startedGeneration(); portEXIT_CRITICAL(&mux_); return value; }
  uint32_t drainedAt() { portENTER_CRITICAL(&mux_); const uint32_t value = timeline_.drainedAt(); portEXIT_CRITICAL(&mux_); return value; }
  bool drainedValid() { portENTER_CRITICAL(&mux_); const bool value = timeline_.drainedValid(); portEXIT_CRITICAL(&mux_); return value; }
  DuplexDiagnostics diagnostics() { portENTER_CRITICAL(&mux_); const DuplexDiagnostics value = timeline_.diagnostics(); portEXIT_CRITICAL(&mux_); return value; }
  uint32_t invalidDmaEvents() { portENTER_CRITICAL(&mux_); const uint32_t value = invalidDmaEvents_; portEXIT_CRITICAL(&mux_); return value; }
  bool clearPlaybackProgress() { portENTER_CRITICAL(&mux_); const bool cleared = timeline_.clearPlaybackProgress(); portEXIT_CRITICAL(&mux_); return cleared; }

 private:
  friend bool duplexAudioSent(i2s_chan_handle_t, i2s_event_data_t*, void*);
  friend bool duplexAudioReceived(i2s_chan_handle_t, i2s_event_data_t*, void*);
  // Avoid 64-bit division helpers in the IRAM callback. At least one EOF runs
  // every 10 ms; unsigned subtraction also handles microsecond counter wrap.
  uint32_t __attribute__((always_inline)) inline nowFromIsr() {
    const uint32_t us = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t elapsed = us - lastClockUs_;
    lastClockUs_ = us;
    clockMs_ += elapsed / 1000U;
    remainderUs_ += elapsed % 1000U;
    if (remainderUs_ >= 1000U) { ++clockMs_; remainderUs_ -= 1000U; }
    return clockMs_;
  }
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  DuplexTimeline timeline_;
  i2s_chan_handle_t tx_ = nullptr, rx_ = nullptr;
  StaticSemaphore_t wakeState_;
  SemaphoreHandle_t wake_ = nullptr;
  uint32_t lastClockUs_ = 0, clockMs_ = 0, remainderUs_ = 0, invalidDmaEvents_ = 0;
  bool running_ = false, txEnabled_ = false, rxEnabled_ = false, fanout_ = false;
};

// Namespace-static definitions avoid the Xtensa inline-method COMDAT literal
// pool relocation problem. All bounded timeline/clock helpers inline here.
static bool IRAM_ATTR duplexAudioSent(i2s_chan_handle_t, i2s_event_data_t* event, void* context) {
  auto* self = static_cast<DuplexAudio*>(context);
  portENTER_CRITICAL_ISR(&self->mux_);
  if (self->running_ && event && event->dma_buf && event->size == kDuplexSamples * 2 * sizeof(int32_t)) {
    self->timeline_.sent(reinterpret_cast<uintptr_t>(event->dma_buf), static_cast<int32_t*>(event->dma_buf), self->nowFromIsr());
  } else if (self->running_) ++self->invalidDmaEvents_;
  portEXIT_CRITICAL_ISR(&self->mux_);
  BaseType_t woken = pdFALSE;
  if (self->wake_) xSemaphoreGiveFromISR(self->wake_, &woken);
  return woken == pdTRUE;
}

static bool IRAM_ATTR duplexAudioReceived(i2s_chan_handle_t, i2s_event_data_t* event, void* context) {
  auto* self = static_cast<DuplexAudio*>(context);
  portENTER_CRITICAL_ISR(&self->mux_);
  if (self->running_ && event && event->dma_buf && event->size == kDuplexSamples * 2 * sizeof(int32_t)) {
    self->timeline_.received(reinterpret_cast<uintptr_t>(event->dma_buf), static_cast<const int32_t*>(event->dma_buf), self->nowFromIsr());
  } else if (self->running_) ++self->invalidDmaEvents_;
  portEXIT_CRITICAL_ISR(&self->mux_);
  BaseType_t woken = pdFALSE;
  if (self->wake_) xSemaphoreGiveFromISR(self->wake_, &woken);
  return woken == pdTRUE;
}

}  // namespace voicebot_audio
