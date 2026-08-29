// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "transceiver.h"

#include <array>

namespace esphome {
namespace wmbus_radio {

// CC1101 support is intentionally strict:
// - GDO2 is used as sync-detect interrupt (start of candidate)
// - GDO0 is used as FIFO-threshold/data hint
// This avoids the single-IRQ ambiguity that produces 8-byte false candidates.
static constexpr size_t CC1101_CHUNK_SIZE = 16;

class CC1101 : public RadioTransceiver {
 public:
  CC1101() { this->irq_edge_ = gpio::INTERRUPT_RISING_EDGE; }

  void set_gdo0_pin(InternalGPIOPin *pin) { this->gdo0_pin_ = pin; }
  void set_gdo2_pin(InternalGPIOPin *pin) { this->gdo2_pin_ = pin; }
  void set_frequency_mhz(float frequency_mhz);

  void setup() override;
  void dump_config() override;
  void restart_rx() override;
  optional<uint8_t> read() override;
  int8_t get_rssi() override;
  const char *get_name() override;

  bool supports_preamble_retry() const override { return true; }
  bool consume_rx_abort_request() override;
  uint32_t take_fifo_overrun_count() override;
  void dump_debug_status(const char *reason) override;
  void log_reg_status() override;

 protected:
  InternalGPIOPin *gdo0_pin_{nullptr};
  InternalGPIOPin *gdo2_pin_{nullptr};

  std::array<uint8_t, CC1101_CHUNK_SIZE> chunk_buffer_{};
  size_t chunk_len_{0};
  size_t chunk_idx_{0};

  uint8_t sync_cycle_{0};
  uint32_t configured_frequency_hz_{868950000UL};
  int8_t last_rssi_dbm_{-127};
  bool rssi_captured_{false};
  bool abort_requested_{false};
  uint32_t fifo_overrun_count_{0};

  void reset_cc1101_();
  void apply_radio_profile_();
  bool validate_startup_config_();
  void set_frequency_(uint32_t frequency_hz);
  void set_sync_word_(uint8_t sync2);

  uint8_t strobe_(uint8_t cmd);
  uint8_t read_reg_(uint8_t address);
  uint8_t read_status_(uint8_t address);
  void write_reg_(uint8_t address, uint8_t value);
  void write_burst_(uint8_t address, const uint8_t *data, size_t len);
  void read_burst_(uint8_t address, uint8_t *data, size_t len);

  uint8_t rxbytes_raw_();
  uint8_t rxbytes_count_();
  bool rx_overflow_();
  void flush_rx_();
  void capture_rssi_();

  optional<uint8_t> drain_fifo_once_();
};

}  // namespace wmbus_radio
}  // namespace esphome
