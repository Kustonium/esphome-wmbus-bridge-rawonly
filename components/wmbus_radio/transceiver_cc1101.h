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
  // Bench option (see __init__.py): the register it changes, and what the
  // health check then expects instead of the default profile.
  void set_sync_15_16(bool v) { this->sync_15_16_ = v; }
  // T1/C1 RX filter: 0 = 325 kHz (TI AN067 link B), 1 = 270 kHz, 2 = 232 kHz.
  void set_rx_bandwidth(uint8_t i) { this->rx_bw_ = i; }
  // Carrier sense as sync qualifier (MDMCFG2 SYNC_MODE bit 2). Off = 0x02/0x01.
  void set_carrier_sense(bool v) { this->cs_ = v; }
  void set_rx_state_wait(bool v) { this->rx_state_wait_ = v; }
  void set_idle_rx_refresh(bool v) { this->idle_rx_refresh_ = v; }
  uint32_t idle_rx_poll_interval_ms() const override { return this->idle_rx_refresh_ ? 50 : 0; }
  void service_idle_rx() override;

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
  bool take_frame_freq_error(int32_t *afc_hz, int32_t *fei_hz) override;

 protected:
  InternalGPIOPin *gdo0_pin_{nullptr};
  InternalGPIOPin *gdo2_pin_{nullptr};

  std::array<uint8_t, CC1101_CHUNK_SIZE> chunk_buffer_{};
  size_t chunk_len_{0};
  size_t chunk_idx_{0};

  uint32_t configured_frequency_hz_{868950000UL};
  int8_t last_rssi_dbm_{-127};
  bool rssi_captured_{false};
  // FREQEST of the current frame; see take_frame_freq_error().
  int8_t last_freqest_raw_{0};
  bool freq_error_fresh_{false};
  bool abort_requested_{false};
  bool sync_15_16_{false};
  bool rx_state_wait_{false};
  bool idle_rx_refresh_{false};
  uint32_t idle_refresh_count_{0};
  uint32_t idle_refresh_skipped_{0};
  uint32_t state_wait_timeouts_{0};
  uint8_t last_wait_state_{0};
  // S1 takes TI's 0x05 (15/16 sync bits, no carrier sense) as AN067 Radio Link A
  // does; T1/C1 keep 0x06 unless cc1101_sync_15_16.
  // SYNC_MODE (SWRS061I p.44): 0x06 = 16/16 + carrier sense, 0x05 = 15/16 +
  // carrier sense, 0x02 / 0x01 = the same without carrier sense. S1 keeps TI's
  // 0x05.
  uint8_t exp_mdmcfg2_() const {
    if (this->s1_()) return 0x05;
    uint8_t m = this->sync_15_16_ ? 0x05 : 0x06;
    if (!this->cs_) m &= (uint8_t) ~0x04;
    return m;
  }
  uint8_t rx_bw_{2};  // 232 kHz, the YAML default
  bool cs_{true};
  uint16_t rx_bw_khz_() const { return this->rx_bw_ == 1 ? 270 : (this->rx_bw_ == 2 ? 232 : 325); }
  // S1 (S-mode, 32.768 kcps Manchester) needs its own modem profile; T1/C1
  // keep the inherited 100 kcps one. S1 values: TI AN067 (SWRA234), Appendix C,
  // "CC1101 Register Settings for Radio Link A" (S-mode). The T1/C1 profile is
  // that note's Radio Link B (Appendix D) except MDMCFG2.
  bool s1_() const { return this->listen_mode_ == LISTEN_MODE_S1; }
  // T1/C1: CHANBW_E=1, CHANBW_M=1/2/3 -> 325/270/232 kHz, DRATE_E=12.
  uint8_t exp_mdmcfg4_() const {
    if (this->s1_()) return 0x6A;
    return this->rx_bw_ == 1 ? 0x6C : (this->rx_bw_ == 2 ? 0x7C : 0x5C);
  }
  uint8_t exp_mdmcfg3_() const { return this->s1_() ? 0x4A : 0x04; }
  uint8_t exp_deviatn_() const { return this->s1_() ? 0x47 : 0x44; }
  uint8_t exp_bscfg_() const { return this->s1_() ? 0x6D : 0xBF; }
  uint8_t exp_agcctrl0_() const { return this->s1_() ? 0xB2 : 0xB5; }
  uint8_t exp_sync1_() const { return this->s1_() ? 0x76 : 0x54; }
  uint8_t exp_sync0_() const { return this->s1_() ? 0x96 : 0x3D; }
  // 0x43: MAX_DVGA_GAIN forbids the top DVGA step. 0x03 (every step) was
  // measured on 2026-11-10/11 and cost ~30% of frames; see __init__.py.
  // S1: TI Link A uses 0x04 (every DVGA step, 36 dB target). Note that 0x03 on
  // T1 was measured worse on 2026-11-10/11; S1 follows TI until measured.
  uint8_t exp_agcctrl2_() const { return this->s1_() ? 0x04 : 0x43; }
  uint32_t fifo_overrun_count_{0};
  // SPI transactions the chip answered with CHIP_RDYn still high.
  uint32_t chip_not_ready_count_{0};
  // Boot-profile registers that never held their value, and how many extra
  // write attempts the profile needed in total.
  uint32_t reg_failed_count_{0};
  uint32_t reg_retry_count_{0};

  void reset_cc1101_();
  void apply_radio_profile_();
  bool validate_startup_config_();
  void set_frequency_(uint32_t frequency_hz);
  void set_sync_word_(uint8_t sync2);

  uint8_t strobe_(uint8_t cmd);
  uint8_t raw_strobe_(uint8_t cmd);
  bool wait_chip_ready_(uint32_t timeout_us);
  uint8_t read_reg_(uint8_t address);
  uint8_t read_status_(uint8_t address);
  void write_reg_(uint8_t address, uint8_t value);
  bool write_reg_verified_(uint8_t address, uint8_t value);
  void write_burst_(uint8_t address, const uint8_t *data, size_t len);
  void read_burst_(uint8_t address, uint8_t *data, size_t len);

  uint8_t rxbytes_raw_();
  uint8_t rxbytes_count_();
  bool rx_overflow_();
  bool flush_rx_();
  bool wait_marcstate_(uint8_t expected);
  void enter_rx_();
  void capture_rssi_();

  optional<uint8_t> drain_fifo_once_();
};

}  // namespace wmbus_radio
}  // namespace esphome
