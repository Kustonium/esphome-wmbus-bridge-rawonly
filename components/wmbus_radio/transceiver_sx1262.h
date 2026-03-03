#pragma once

#include "transceiver.h"
#include "esphome/core/hal.h"

#include <vector>

namespace esphome {
namespace wmbus_radio {

// Datasheet (SX1261/2 Rev 2.2): Rx gain control
// Reg 0x08AC: 0x94 power saving, 0x96 boosted
enum SX1262RxGain : uint8_t {
  POWER_SAVING = 0,
  BOOSTED = 1,
};

class SX1262 : public RadioTransceiver {
 public:
  SX1262() { this->irq_edge_ = gpio::INTERRUPT_RISING_EDGE; }

  // RX gain (BOOSTED/POWER_SAVING)
  void set_rx_gain(SX1262RxGain gain) { this->rx_gain_ = gain; }

  // SX1262 tuning / board helpers (set from YAML via __init__.py)
  void set_dio2_rf_switch(bool v) { this->dio2_rf_switch_ = v; }
  void set_has_tcxo(bool v) { this->has_tcxo_ = v; }

  // Enable Semtech AN1200.53 long GFSK RX path.
  // This bypasses the 255-byte internal data-buffer limitation by streaming
  // from the radio buffer while RX is still running (rxAddrPtr wrap + RxTxPldLen).
  // Useful for WMBus T-mode where 3-of-6 expands telegrams beyond 255 raw bytes.
  void set_long_gfsk_packets(bool v) { this->long_gfsk_packets_ = v; }

  // Diagnostics toggles (avoid extra SPI reads unless explicitly enabled)
  void set_diag_expert(bool v) { this->diag_expert_ = v; }
  void set_diag_rx_buf_status(bool v) { this->diag_rx_buf_status_ = v; }

  // Boot-time maintenance
  void set_clear_device_errors_on_boot(bool v) { this->clear_device_errors_on_boot_ = v; }

  // Optional Heltec V4 front-end (FEM/LNA/PA). If configured, we force RX path.
  void set_fem_ctrl_pin(InternalGPIOPin *pin) { this->fem_ctrl_pin_ = pin; }
  void set_fem_en_pin(InternalGPIOPin *pin) { this->fem_en_pin_ = pin; }
  void set_fem_pa_pin(InternalGPIOPin *pin) { this->fem_pa_pin_ = pin; }

  void setup() override;
  void restart_rx() override;
  optional<uint8_t> read() override;
  int8_t get_rssi() override;
  const char *get_name() override;

  bool get_cached_chip_diag(ChipDiagSnapshot &out) override;
  bool get_boot_cleared_device_errors(uint16_t &before, uint16_t &after) override;

 protected:
  void wait_while_busy_();
  void cmd_write_(uint8_t cmd, std::initializer_list<uint8_t> args);
  void cmd_read_(uint8_t cmd, std::initializer_list<uint8_t> args, uint8_t *out, size_t out_len);
  void write_register_(uint16_t addr, std::initializer_list<uint8_t> data);

  // Register helpers
  uint8_t read_register8_(uint16_t addr);
  uint16_t get_irq_status_();
  void read_buffer_(uint8_t offset, uint8_t *out, size_t out_len);

  // Semtech diagnostics (SX126x)
  uint16_t get_device_errors_();
  void clear_device_errors_();
  void get_stats_(uint16_t &rx, uint16_t &crc, uint16_t &hdr);

  void set_rf_frequency_(uint32_t freq_hz);
  void set_sync_word_(uint8_t sync2);

  bool has_rx_done_();
  bool load_rx_buffer_();

  // Long GFSK reception (Semtech AN1200.53)
  bool capture_rx_stream_();

  // Bias towards Block B (0x3D). Every 4th hop switches to Block A (0xCD).
  uint8_t sync_cycle_{0};

  // Config
  bool dio2_rf_switch_{true};
  bool has_tcxo_{false};
  SX1262RxGain rx_gain_{BOOSTED};
  bool long_gfsk_packets_{false};

  // Diagnostics
  bool diag_expert_{false};
  bool diag_rx_buf_status_{false};
  bool clear_device_errors_on_boot_{true};

  // Optional FEM pins
  InternalGPIOPin *fem_ctrl_pin_{nullptr};
  InternalGPIOPin *fem_en_pin_{nullptr};
  InternalGPIOPin *fem_pa_pin_{nullptr};

  std::vector<uint8_t> rx_buffer_{};
  size_t rx_idx_{0};
  size_t rx_len_{0};
  bool rx_loaded_{false};

  // Cached per-RX diagnostics (no SPI at publish-time)
  uint16_t last_irq_{0};
  uint16_t last_dev_err_{0};
  uint16_t last_stat_rx_{0};
  uint16_t last_stat_crc_{0};
  uint16_t last_stat_hdr_{0};
  uint8_t last_rx_buf_len_{0};
  uint8_t last_rx_buf_start_ptr_{0};
  bool last_dev_err_valid_{false};
  bool last_stats_valid_{false};
  bool last_rx_buf_valid_{false};

  // Boot clear snapshot
  bool boot_dev_err_valid_{false};
  uint16_t boot_dev_err_before_{0};
  uint16_t boot_dev_err_after_{0};

  // Packet RSSI captured at the time the RX buffer was filled.
  // In long-GFSK mode we stop RX (standby) after capture, so GetPacketStatus
  // may return zeros later. Cache it here.
  int8_t last_rssi_dbm_{0};
};

}  // namespace wmbus_radio
}  // namespace esphome
