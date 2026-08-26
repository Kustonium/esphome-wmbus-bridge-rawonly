// SPDX-License-Identifier: GPL-3.0-or-later
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
  void set_frequency_mhz(float frequency_mhz) {
    this->configured_frequency_hz_ = (uint32_t) (frequency_mhz * 1000000.0f + 0.5f);
  }
  void set_dio2_rf_switch(bool v) { this->dio2_rf_switch_ = v; }
  void set_has_tcxo(bool v) { this->has_tcxo_ = v; }

  // Enable Semtech AN1200.53 long GFSK RX path.
  // This bypasses the 255-byte internal data-buffer limitation by streaming
  // from the radio buffer while RX is still running (rxAddrPtr wrap + RxTxPldLen).
  // Useful for WMBus T-mode where 3-of-6 expands telegrams beyond 255 raw bytes.
  void set_long_gfsk_packets(bool v) { this->long_gfsk_packets_ = v; }

  // Optional: clear latched device errors on boot (and capture before/after).
  void set_clear_device_errors_on_boot(bool v) { this->clear_device_errors_on_boot_ = v; }
  bool get_boot_device_errors(uint16_t &before, uint16_t &after) const override {
    if (!this->boot_dev_err_valid_) return false;
    before = this->boot_dev_err_before_;
    after = this->boot_dev_err_after_;
    return true;
  }

  // Optional Heltec V4 front-end (FEM/LNA/PA). If configured, we force RX path.
  void set_fem_ctrl_pin(InternalGPIOPin *pin) { this->fem_ctrl_pin_ = pin; }
  void set_fem_en_pin(InternalGPIOPin *pin) { this->fem_en_pin_ = pin; }
  void set_fem_pa_pin(InternalGPIOPin *pin) { this->fem_pa_pin_ = pin; }

  // Optional external gate of the module's internal RF switch, held high for as
  // long as the radio is in use. Distinct from dio2_rf_switch: DIO2 selects the
  // TX/RX direction, this enables the switch at all. See setup() for why a board
  // that needs it is unusable without it.
  void set_rf_sw_pin(InternalGPIOPin *pin) { this->rf_sw_pin_ = pin; }

  void setup() override;
  void restart_rx() override;
  optional<uint8_t> read() override;
  int8_t get_rssi() override;
  bool read_channel_rssi_dbm(int8_t *out) override;
  bool take_rssi_diag(RssiDiag &out) override;
  const char *get_name() override;
  void log_reg_status() override;
  void dump_debug_status(const char *reason) override;

 protected:
  void wait_while_busy_();
  void cmd_write_(uint8_t cmd, std::initializer_list<uint8_t> args);
  void cmd_read_(uint8_t cmd, std::initializer_list<uint8_t> args, uint8_t *out, size_t out_len);
  void write_register_(uint16_t addr, std::initializer_list<uint8_t> data);

  // Register helpers
  uint8_t read_register8_(uint16_t addr);
  uint16_t get_irq_status_();

  // Raw SX126x status byte (GetStatus). Chip mode lives in bits 6:4 and is the
  // SX126x answer to "is the receiver actually running": cmd_read_() discards
  // this byte as protocol overhead, so it needs its own transaction.
  uint8_t get_status_();
  void read_buffer_(uint8_t offset, uint8_t *out, size_t out_len);

  // Instantaneous RSSI. Only valid while the frame is still being transmitted;
  // after the frame it reads the noise floor.
  int8_t read_rssi_inst_dbm_();

  // Raw RssiSync / RssiAvg from GetPacketStatus (0 = never latched).
  void read_packet_status_rssi_(uint8_t &raw_sync, uint8_t &raw_avg);

  // Which of the two receive paths produced a frame. Doubles as the index into
  // rssi_diag_reported_, so the values must stay 0/1.
  enum class RxPath : uint8_t { FIFO = 0, STREAM = 1 };

  // Diagnostic: note which source the frame's RSSI came from, for Radio::loop()
  // to report. Called from the receiver task; must not log at INFO itself.
  void record_rssi_diag_(RxPath path, uint8_t raw_sync, uint8_t raw_avg, int8_t inflight,
                         uint16_t trigger_irq = 0, const char *exit_reason = "n/a");

  void set_rf_frequency_(uint32_t freq_hz);
  void set_sync_word_(uint8_t sync2);
  void set_s1_sync_word_();

  // Diagnostic: search the captured stream for the chip offset at which a valid
  // L+C actually sits, and report how clean the frame is from there. Answers
  // whether capture_rx_stream_() starts where s1_expected_raw_len_() assumes.
  void log_s1_frame_start_(const std::vector<uint8_t> &raw);

  bool has_rx_done_();
  bool load_rx_buffer_();

  // Long GFSK reception (Semtech AN1200.53)
  bool capture_rx_stream_(uint16_t trigger_irq);

  // Adaptive long-packet mode:
  // long_gfsk_packets=false -> always use fast normal FIFO/RX_DONE path.
  // long_gfsk_packets=true  -> still use fast normal path by default; if a packet
  // reaches the SX126x FIFO edge (>=250 B), enable long-stream reception for a
  // short hold window so subsequent long frames can exceed the 255-byte FIFO limit.
  bool long_stream_active_() const;
  void configure_irq_params_();

  // Bias towards Block B (0x3D). Every 4th hop switches to Block A (0xCD).
  uint8_t sync_cycle_{0};

  // Config
  uint32_t configured_frequency_hz_{868950000UL};
  bool dio2_rf_switch_{true};
  bool has_tcxo_{false};
  SX1262RxGain rx_gain_{BOOSTED};
  bool long_gfsk_packets_{false};
  bool clear_device_errors_on_boot_{false};

  // Adaptive long-stream hold. Only meaningful when long_gfsk_packets_ is true.
  uint32_t long_stream_hold_until_ms_{0};

  // Captured SX126x device errors (best-effort)
  bool boot_dev_err_valid_{false};
  uint16_t boot_dev_err_before_{0};
  uint16_t boot_dev_err_after_{0};

  // Optional FEM pins
  InternalGPIOPin *fem_ctrl_pin_{nullptr};
  InternalGPIOPin *fem_en_pin_{nullptr};
  InternalGPIOPin *fem_pa_pin_{nullptr};

  // Optional external RF switch gate (Wio-SX1262 pin 1, RF_SW).
  InternalGPIOPin *rf_sw_pin_{nullptr};

  std::vector<uint8_t> rx_buffer_{};
  size_t rx_idx_{0};
  size_t rx_len_{0};
  bool rx_loaded_{false};

  // Packet RSSI captured while the frame was on air (or latched at sync-word
  // detection). In long-GFSK mode we stop RX (standby) after capture, so
  // GetPacketStatus may return zeros later. Cache it here.
  // -127 = not measured for this frame; rf_runtime.cpp ignores anything <= -126.
  int8_t last_rssi_dbm_{-127};

  // Mailbox handing the RSSI provenance of the first frame on each receive path
  // to the main task, which is the only one that can log it. Written by the
  // receiver task, drained by take_rssi_diag() from Radio::loop().
  //
  // One slot per path, indexed by RxPath. A single shared slot loses reports:
  // the receiver task can take both paths within one Radio::loop() iteration,
  // and the second snapshot then overwrites the first before anything reads it.
  RssiDiag rssi_diag_[2]{};
  bool rssi_diag_pending_[2]{};
  bool rssi_diag_reported_[2]{};
};

}  // namespace wmbus_radio
}  // namespace esphome
