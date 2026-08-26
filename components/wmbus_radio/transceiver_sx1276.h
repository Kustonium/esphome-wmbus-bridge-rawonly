// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "transceiver.h"
#include <array>

namespace esphome {
namespace wmbus_radio {

// Burst size used only when SX1276 reports FifoLevel.
// Threshold is set to CHUNK_SIZE - 1, so a burst of exactly CHUNK_SIZE bytes is safe.
static constexpr size_t SX1276_CHUNK_SIZE = 16;

// Short tail-gap bridge after FIFO temporarily becomes empty.
// At 100 kbps one byte takes ~80 us, so 1000 us covers scheduler jitter
// and small intra-frame gaps without waiting long enough to merge frames.
static constexpr uint32_t SX1276_TAIL_GAP_US = 1000;

class SX1276 : public RadioTransceiver {
 public:
  void set_frequency_mhz(float frequency_mhz) {
    this->configured_frequency_hz_ = (uint32_t) (frequency_mhz * 1000000.0f + 0.5f);
  }
  void set_tcxo_pin(InternalGPIOPin *pin) { this->tcxo_pin_ = pin; }
  void setup() override;
  optional<uint8_t> read() override;
  void restart_rx() override;
  int8_t get_rssi() override;
  bool read_channel_rssi_dbm(int8_t *out) override;
  const char *get_name() override;
  bool supports_preamble_retry() const override { return true; }
  bool supports_unknown_size_raw_drain() const override { return true; }
  bool supports_weak_partial_start_abort() const override { return true; }
  bool consume_rx_abort_request() override;
  uint32_t take_fifo_overrun_count() override;
  void log_reg_status() override;
  void dump_debug_status(const char *reason) override;

 protected:
  uint32_t configured_frequency_hz_{868950000UL};
  InternalGPIOPin *tcxo_pin_{nullptr};
  uint8_t sync_cycle_{0};

  // Burst chunk buffered in ESP32 RAM and served byte-by-byte to upper layer.
  std::array<uint8_t, SX1276_CHUNK_SIZE> chunk_buffer_{};
  size_t chunk_len_{0};
  size_t chunk_idx_{0};

  // True after at least one byte/chunk of the current frame has been seen.
  // Used to bridge short empty-FIFO gaps near the tail of one frame.
  bool frame_active_{false};

  int8_t last_rssi_dbm_{-127};
  bool rssi_captured_{false};
  bool abort_requested_{false};
  uint32_t fifo_overrun_count_{0};

  // Burst SPI: CS held low for the entire transfer.
  // SAFE only when caller knows at least 'len' bytes are already in FIFO.
  void spi_read_burst_(uint8_t address, uint8_t *dst, size_t len);

  // Drain one safe chunk or one safe tail byte if available.
  optional<uint8_t> drain_fifo_once_();

  // Sample RSSI, RegAfc and RegFei once per frame, when its first bytes reach
  // the FIFO. All three are only meaningful at that instant: RSSI measures the
  // empty channel afterwards, and AFC/FEI are overwritten by the next preamble
  // detection, noise included. Everything read here is kept for the diagnostic
  // dump, which runs when nothing is arriving and so cannot sample them itself.
  void latch_frame_metrics_();
  uint8_t last_afc_msb_{0};
  uint8_t last_afc_lsb_{0};
  uint8_t last_fei_msb_{0};
  uint8_t last_fei_lsb_{0};
  int8_t last_frame_rssi_dbm_{-127};
  // Timestamp of the latch, 0 = nothing received since boot. Not cleared by
  // restart_rx(): the point is to report how old the last real reading is.
  int64_t frame_metrics_us_{0};

  // Whole FSK register bank as hex, once at boot under verbose diagnostics.
  // Meant to be run in one listen mode and then the other, and the two logs
  // diffed - reading the registers beats arguing from the setup sequence about
  // which of them can possibly differ.
  void dump_register_bank_();
};

}  // namespace wmbus_radio
}  // namespace esphome
