// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "transceiver.h"
#include "esphome/core/hal.h"

#include <vector>

// ---------------------------------------------------------------------------
// Compile gate.
//
// Every other transceiver in this component is compiled into every build,
// because every other transceiver has been run against real hardware. This one
// has not: it was written from the LR1121 datasheet (Rev 2.1), the Waveshare
// resource package and the board schematic, with no board on the desk. A driver
// that has never executed must not be able to break the build of a Heltec or a
// T3S3 that works today, so the whole translation unit is empty unless a config
// actually selects radio_type: LR1121 (__init__.py adds the build flag then).
//
// Remove the gate once the driver has received a frame from a real meter. Until
// then it is the difference between "new radio available for testing" and
// "new radio can take everyone else down with it".
// ---------------------------------------------------------------------------
#ifdef USE_WMBUS_RADIO_LR1121

namespace esphome {
namespace wmbus_radio {

// GFSK double-sideband receiver bandwidth, LR1121 datasheet / driver
// lr11xx_radio_types.h. Only the values that make sense for wM-Bus at
// 100 kb/s are exposed; the full table runs from 4.8 kHz to 467 kHz.
//
// Required minimum is 2*fdev + bitrate = 200 kHz, so 234300 is the tightest
// usable setting and the default. 312000 and 467000 buy tolerance for meters
// whose crystals sit far off channel, at the cost of letting in more noise.
enum LR1121RxBandwidth : uint8_t {
  LR1121_BW_234300 = 0x0A,
  LR1121_BW_312000 = 0x19,
  LR1121_BW_373600 = 0x11,
  LR1121_BW_467000 = 0x09,
};

// Preamble detector length, GFSK packet parameters.
// Shorter = more sensitive and more false triggers; longer = the opposite.
enum LR1121PreambleDetector : uint8_t {
  LR1121_PREAMBLE_OFF      = 0x00,
  LR1121_PREAMBLE_MIN_8B   = 0x04,
  LR1121_PREAMBLE_MIN_16B  = 0x05,
  LR1121_PREAMBLE_MIN_24B  = 0x06,
  LR1121_PREAMBLE_MIN_32B  = 0x07,
};

// TCXO regulator setting, SetTcxoMode argument.
//
// UNRESOLVED ON THIS BOARD, and the reason this is a YAML option rather than a
// constant. The Waveshare package states both: every C example sets 3.0 V
// (a value traceable to Semtech's own shield code, smtc_shield_lr11x0_common.c,
// which fills the field even for a shield declared has_tcxo = false), while the
// Meshtastic variant in the same package sets 1.8 V (a file whose own comments
// mention a different board). Neither is a measurement of the part fitted here,
// and the schematic labels Y2 only as "SMD TCXO 32 MHz (2.0x1.6)".
//
// Failure is asymmetric, which decides the default: too little voltage and the
// oscillator simply does not start, which the chip reports as HF_XOSC_START in
// GetErrors and setup() logs loudly. Too much and it starts anyway, out of
// spec, silently. So the default here is the LOW one - a boot that fails
// audibly beats a boot that works today and drifts later.
enum LR1121TcxoVoltage : uint8_t {
  LR1121_TCXO_1_6V = 0x00,
  LR1121_TCXO_1_7V = 0x01,
  LR1121_TCXO_1_8V = 0x02,
  LR1121_TCXO_2_2V = 0x03,
  LR1121_TCXO_2_4V = 0x04,
  LR1121_TCXO_2_7V = 0x05,
  LR1121_TCXO_3_0V = 0x06,
  LR1121_TCXO_3_3V = 0x07,
};

// ---------------------------------------------------------------------------
// LR1121 - receive-only wM-Bus front end.
//
// Shape of the driver follows SX1262: buffer-based radio, one IRQ line, a BUSY
// handshake, and a read() that serves bytes out of a buffer loaded once per
// frame. What differs from SX126x and is worth knowing before editing:
//
//   * Opcodes are 16-bit, big-endian, and every command starts with them.
//   * A read is TWO SPI transactions, not one. Command goes out, CS rises,
//     BUSY must fall again, and only then does a second transaction clock out
//     one status byte followed by the payload. Doing it in a single transaction
//     returns stale bytes. (Semtech lr11xx_hal.c, verified against the copy in
//     the Waveshare package.)
//   * The RF switch is not a pin this driver toggles. DIO5/DIO6 drive an
//     external SPDT (RTC6603SP on the Waveshare board) and the chip sequences
//     them itself once SetDioAsRfSwitch has been given the table.
//
// wM-Bus specifics that are not negotiable and are therefore not options:
//   * DC-free (whitening) OFF. The chip default would scramble the payload.
//   * CRC OFF. wM-Bus carries its own per-block CRC; hardware CRC here would
//     reject good frames.
//   * Fixed packet length. In T-mode the L field is itself 3-out-of-6 encoded,
//     so the packet engine cannot read a length from the air. It captures a
//     generous fixed run and the host trims - which is exactly what the
//     decode-off-device architecture wants anyway.
// ---------------------------------------------------------------------------
class LR1121 : public RadioTransceiver {
 public:
  LR1121() { this->irq_edge_ = gpio::INTERRUPT_RISING_EDGE; }

  // --- set from YAML via __init__.py ---------------------------------------
  void set_frequency_mhz(float frequency_mhz) {
    this->configured_frequency_hz_ = (uint32_t) (frequency_mhz * 1000000.0f + 0.5f);
  }
  void set_bitrate(uint32_t bps) { this->bitrate_bps_ = bps; }
  void set_deviation(uint32_t hz) { this->deviation_hz_ = hz; }
  void set_rx_bandwidth(LR1121RxBandwidth bw) { this->rx_bandwidth_ = bw; }
  void set_preamble_detector(LR1121PreambleDetector d) { this->preamble_detector_ = d; }
  void set_payload_length(uint8_t len) { this->payload_length_ = len; }
  void set_rx_boosted(bool v) { this->rx_boosted_ = v; }
  void set_tcxo_voltage(LR1121TcxoVoltage v) { this->tcxo_voltage_ = v; }
  void set_tcxo_startup_ticks(uint32_t ticks) { this->tcxo_startup_ticks_ = ticks; }

  // --- RadioTransceiver ----------------------------------------------------
  void setup() override;
  void restart_rx() override;
  optional<uint8_t> read() override;
  int8_t get_rssi() override;
  const char *get_name() override;
  void log_reg_status() override;
  void dump_debug_status(const char *reason) override;
  bool take_rssi_diag(RssiDiag &out) override;

 protected:
  // --- SPI plumbing --------------------------------------------------------

  // Blocks until BUSY falls, or gives up after a bounded wait. Returns false on
  // timeout so callers can log rather than hang.
  //
  // A false here is NOT treated as fatal, and that is deliberate. There are two
  // very different worlds behind "BUSY stayed high": the chip is dead, or the
  // BUSY line is not telling us the truth (wrong pin, floating input, board
  // revision). They are told apart by asking the chip something over SPI
  // anyway - which only works if a stuck BUSY does not gag the driver first.
  bool wait_while_busy_(uint32_t timeout_ms = 100);

  // NSS pulse. The vendor HAL wakes the chip by driving CS low for ~1 ms and
  // releasing it, and calls this immediately after reset. ESPHome owns the CS
  // pin through the SPI delegate, so the pulse is produced by performing a real
  // one-byte transaction: an empty begin/end pair is not guaranteed to move CS
  // at all, which is exactly the bug this replaces.
  void wake_pulse_();

  void cmd_write_(uint16_t opcode, std::initializer_list<uint8_t> args);
  void cmd_write_buf_(uint16_t opcode, const uint8_t *args, size_t len);
  bool cmd_read_(uint16_t opcode, std::initializer_list<uint8_t> args, uint8_t *out, size_t out_len);

  // --- chip helpers --------------------------------------------------------
  bool get_version_(uint8_t &hw, uint8_t &type, uint16_t &fw);
  uint16_t get_errors_();
  uint32_t get_irq_status_();
  void set_sync_word_(uint8_t sync2);
  void set_s1_sync_word_();
  void configure_gfsk_();
  bool load_rx_buffer_();

  // Packet RSSI, from GetPacketStatus. Both fields are reported as
  // -(raw >> 1) dBm by the chip.
  void read_packet_status_rssi_(uint8_t &raw_sync, uint8_t &raw_avg);

  // Instantaneous RSSI. Meaningful only while a frame is on air; after RX_DONE
  // it measures the empty channel. Used for the diagnostic snapshot, never as
  // the frame's reported level - the SX1262 driver learned that the hard way.
  int8_t read_rssi_inst_dbm_();

  // --- config --------------------------------------------------------------
  uint32_t configured_frequency_hz_{868950000UL};
  uint32_t bitrate_bps_{100000UL};
  uint32_t deviation_hz_{50000UL};
  LR1121RxBandwidth rx_bandwidth_{LR1121_BW_234300};
  LR1121PreambleDetector preamble_detector_{LR1121_PREAMBLE_MIN_16B};
  uint8_t payload_length_{255};
  bool rx_boosted_{true};
  LR1121TcxoVoltage tcxo_voltage_{LR1121_TCXO_3_0V};
  uint32_t tcxo_startup_ticks_{3000};  // ~91.6 ms at 32.768 kHz

  // --- state ---------------------------------------------------------------
  uint8_t sync_cycle_{0};
  std::vector<uint8_t> rx_buffer_{};
  size_t rx_idx_{0};
  size_t rx_len_{0};
  bool rx_loaded_{false};

  // -127 = not measured for this frame; rf_runtime.cpp ignores anything <= -126.
  int8_t last_rssi_dbm_{-127};

  // Boot snapshot, logged from setup() and repeated by log_reg_status() so it
  // reaches the API log stream and not only UART.
  uint8_t boot_hw_{0};
  uint8_t boot_type_{0};
  uint16_t boot_fw_{0};
  uint16_t boot_errors_{0};
  uint16_t errors_after_xosc_{0};
  uint16_t errors_after_image_{0};
  uint16_t errors_after_calibrate_{0};
  bool boot_ok_{false};

  // Set when BUSY never fell during boot. Kept so the state is reported once,
  // and so per-command waits stop re-logging the same thing forever.
  bool busy_line_suspect_{false};

  // S1 probe: reported once, so a sync-without-packet condition is stated and
  // then stops repeating for every detector trigger.
  bool s1_sync_seen_{false};

  // One provenance snapshot per boot, same contract as the SX1262 driver:
  // written by the receiver task, drained by Radio::loop() on the main task.
  RssiDiag rssi_diag_{};
  bool rssi_diag_pending_{false};
  bool rssi_diag_reported_{false};
};

}  // namespace wmbus_radio
}  // namespace esphome

#endif  // USE_WMBUS_RADIO_LR1121
