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

  // Optional Heltec V4 front-end (FEM/LNA/PA). If configured, we force RX path.
  void set_fem_ctrl_pin(InternalGPIOPin *pin) { this->fem_ctrl_pin_ = pin; }
  void set_fem_en_pin(InternalGPIOPin *pin) { this->fem_en_pin_ = pin; }
  void set_fem_pa_pin(InternalGPIOPin *pin) { this->fem_pa_pin_ = pin; }

  void setup() override;
  void restart_rx() override;
  optional<uint8_t> read() override;
  int8_t get_rssi() override;
  const char *get_name() override;

 protected:
  void wait_while_busy_();
  void cmd_write_(uint8_t cmd, std::initializer_list<uint8_t> args);
  void cmd_read_(uint8_t cmd, std::initializer_list<uint8_t> args, uint8_t *out, size_t out_len);
  void write_register_(uint16_t addr, std::initializer_list<uint8_t> data);

  void set_rf_frequency_(uint32_t freq_hz);
  void set_sync_word_(uint8_t sync2);

  bool has_rx_done_();
  bool load_rx_buffer_();

  // Bias towards Block B (0x3D). Every 4th hop switches to Block A (0xCD).
  uint8_t sync_cycle_{0};

  // Config
  bool dio2_rf_switch_{true};
  bool has_tcxo_{false};
  SX1262RxGain rx_gain_{BOOSTED};

  // Optional FEM pins
  InternalGPIOPin *fem_ctrl_pin_{nullptr};
  InternalGPIOPin *fem_en_pin_{nullptr};
  InternalGPIOPin *fem_pa_pin_{nullptr};

  std::vector<uint8_t> rx_buffer_{};
  size_t rx_idx_{0};
  size_t rx_len_{0};
  bool rx_loaded_{false};
};

}  // namespace wmbus_radio
}  // namespace esphome
