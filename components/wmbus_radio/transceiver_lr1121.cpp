// SPDX-License-Identifier: GPL-3.0-or-later
#include "transceiver_lr1121.h"

#ifdef USE_WMBUS_RADIO_LR1121

#include "esphome/core/log.h"

#include <cstdio>

namespace esphome {
namespace wmbus_radio {
static const char *TAG = "LR1121";

// ---------------------------------------------------------------------------
// Opcodes. 16-bit, big-endian, always the first two bytes of a command.
//
// Values transcribed from the Semtech LR11xx driver shipped in the Waveshare
// resource package (lr11xx_system.c, lr11xx_radio.c, lr11xx_regmem.c). They are
// the driver's own enumerations, not a datasheet reading, which is why the
// source file is named rather than a datasheet table.
// ---------------------------------------------------------------------------
static constexpr uint16_t OC_GET_VERSION        = 0x0101;
static constexpr uint16_t OC_READ_BUFFER8       = 0x010A;
static constexpr uint16_t OC_GET_ERRORS         = 0x010D;
static constexpr uint16_t OC_CLEAR_ERRORS       = 0x010E;
static constexpr uint16_t OC_CALIBRATE          = 0x010F;
static constexpr uint16_t OC_SET_REG_MODE       = 0x0110;
static constexpr uint16_t OC_CALIBRATE_IMAGE    = 0x0111;
static constexpr uint16_t OC_SET_DIO_AS_RFSW    = 0x0112;
static constexpr uint16_t OC_SET_DIO_IRQ_PARAMS = 0x0113;
static constexpr uint16_t OC_CLEAR_IRQ          = 0x0114;
static constexpr uint16_t OC_CFG_LFCLK          = 0x0116;
static constexpr uint16_t OC_SET_TCXO_MODE      = 0x0117;
static constexpr uint16_t OC_SET_STANDBY        = 0x011C;

static constexpr uint16_t OC_GET_RXBUFFER_STATUS = 0x0203;
static constexpr uint16_t OC_GET_PKT_STATUS      = 0x0204;
static constexpr uint16_t OC_GET_RSSI_INST       = 0x0205;
static constexpr uint16_t OC_SET_GFSK_SYNC_WORD  = 0x0206;
static constexpr uint16_t OC_SET_RX              = 0x0209;
static constexpr uint16_t OC_SET_RF_FREQUENCY    = 0x020B;
static constexpr uint16_t OC_SET_PKT_TYPE        = 0x020E;
static constexpr uint16_t OC_SET_MODULATION_PARAM = 0x020F;
static constexpr uint16_t OC_SET_PKT_PARAM       = 0x0210;
static constexpr uint16_t OC_SET_RX_BOOSTED      = 0x0227;
static constexpr uint16_t OC_SET_RSSI_CALIBRATION = 0x0229;

// Argument constants (lr11xx_system_types.h / lr11xx_radio_types.h)
static constexpr uint8_t STANDBY_RC   = 0x00;
static constexpr uint8_t STANDBY_XOSC = 0x01;
static constexpr uint8_t REG_MODE_DCDC = 0x01;
static constexpr uint8_t LFCLK_XTAL = 0x01;
static constexpr uint8_t PKT_TYPE_GFSK = 0x01;
static constexpr uint8_t GFSK_PULSE_SHAPE_OFF = 0x00;
static constexpr uint8_t GFSK_ADDR_FILTER_DISABLE = 0x00;
static constexpr uint8_t GFSK_PKT_FIX_LEN = 0x00;
static constexpr uint8_t GFSK_CRC_OFF = 0x01;
static constexpr uint8_t GFSK_DC_FREE_OFF = 0x00;

// RF switch table for the Waveshare ESP32-S3-LR1121-XF board.
//
// DIO5 (RFSW0) drives V1 of the RTC6603SP, DIO6 (RFSW1) drives V2. The switch
// datasheet gives V1=1,V2=0 -> RFC-RF2 and V1=0,V2=1 -> RFC-RF1, so RX lands on
// RF2 and TX on RF1. This component never transmits; the TX rows exist because
// the chip wants a full table and because leaving them zero would park the
// switch in an undefined state on any mode change.
//
// Chain of evidence for these four bytes: Semtech shield source ->
// LR1121 datasheet Table 4-1 (DIO mapping) -> board schematic netlist ->
// RichWave RTC6603SP datasheet truth table. Independently confirmed by the
// Meshtastic variant shipped in the same package, which is written against
// RadioLib and arrives at the same bits.
static constexpr uint8_t RFSW_ENABLE  = 0x03;  // RFSW0 | RFSW1 used as switch lines
static constexpr uint8_t RFSW_STANDBY = 0x00;
static constexpr uint8_t RFSW_RX      = 0x01;  // RFSW0 high -> V1=1, V2=0 -> RF2
static constexpr uint8_t RFSW_TX      = 0x02;  // RFSW1 high -> V1=0, V2=1 -> RF1
static constexpr uint8_t RFSW_TX_HP   = 0x02;
static constexpr uint8_t RFSW_TX_HF   = 0x00;  // 2.4 GHz port bypasses this switch
static constexpr uint8_t RFSW_GNSS    = 0x00;
static constexpr uint8_t RFSW_WIFI    = 0x00;

// IRQ bits (lr11xx_system_types.h). The mask is 32-bit on this chip, unlike
// SX126x where it is 16.
static constexpr uint32_t IRQ_RX_DONE       = (1UL << 3);
static constexpr uint32_t IRQ_TIMEOUT       = (1UL << 10);
static constexpr uint32_t IRQ_FSK_LEN_ERROR = (1UL << 24);
static constexpr uint32_t IRQ_ALL           = 0xFFFFFFFFUL;

// GetErrors bits. Bit 5 is the one that matters at bring-up: it is the chip
// saying "the 32 MHz oscillator never started", which on this board means the
// TCXO voltage is wrong. See the note on LR1121TcxoVoltage.
static constexpr uint16_t ERR_LF_RC_CALIB   = (1 << 0);
static constexpr uint16_t ERR_HF_RC_CALIB   = (1 << 1);
static constexpr uint16_t ERR_ADC_CALIB     = (1 << 2);
static constexpr uint16_t ERR_PLL_CALIB     = (1 << 3);
static constexpr uint16_t ERR_IMG_CALIB     = (1 << 4);
static constexpr uint16_t ERR_HF_XOSC_START = (1 << 5);
static constexpr uint16_t ERR_LF_XOSC_START = (1 << 6);
static constexpr uint16_t ERR_PLL_LOCK      = (1 << 7);

// Image calibration window for the 863-870 MHz band, in the chip's 4 MHz steps.
// The Waveshare examples ship with the 430-440 MHz pair active and this one
// commented out - in all thirteen of their config files. On an HF board that
// calibrates the image rejection for the wrong band and costs ~10 dB of image
// attenuation (datasheet Table 3-8, IMRFSK: 40 dB uncalibrated vs 50 dB
// calibrated). Do not copy those files without swapping these two bytes.
static constexpr uint8_t CAL_IMG_FREQ1_863MHZ = 0xD7;
static constexpr uint8_t CAL_IMG_FREQ2_870MHZ = 0xDB;

// Full calibration set (all blocks), as used by every vendor example.
static constexpr uint8_t CALIBRATE_ALL = 0x3F;

// RX continuous. 0xFFFFFF is the documented magic value: the chip stays in RX
// after a packet instead of dropping to standby (lr11xx_radio.h).
static constexpr uint8_t RX_CONTINUOUS[3] = {0xFF, 0xFF, 0xFF};

static constexpr int8_t RSSI_NOT_MEASURED = -127;

// How long to wait for BUSY to fall. Generous: the longest legitimate wait is
// the TCXO start-up (~9.2 ms at the default 300 ticks) plus calibration.
static constexpr uint32_t BUSY_TIMEOUT_MS = 100;

// ---------------------------------------------------------------------------
// SPI plumbing
// ---------------------------------------------------------------------------

bool LR1121::wait_while_busy_() {
  if (this->busy_pin_ == nullptr)
    return true;
  const uint32_t start = millis();
  while (this->busy_pin_->digital_read()) {
    if ((uint32_t) (millis() - start) > BUSY_TIMEOUT_MS)
      return false;
  }
  return true;
}

void LR1121::cmd_write_buf_(uint16_t opcode, const uint8_t *args, size_t len) {
  if (!this->wait_while_busy_()) {
    ESP_LOGW(TAG, "BUSY stuck high before command 0x%04X", opcode);
    return;
  }
  this->delegate_->begin_transaction();
  this->delegate_->transfer((uint8_t) (opcode >> 8));
  this->delegate_->transfer((uint8_t) (opcode & 0xFF));
  for (size_t i = 0; i < len; i++)
    this->delegate_->transfer(args[i]);
  this->delegate_->end_transaction();
}

void LR1121::cmd_write_(uint16_t opcode, std::initializer_list<uint8_t> args) {
  uint8_t buf[16];
  size_t n = 0;
  for (auto b : args) {
    if (n >= sizeof(buf))
      break;
    buf[n++] = b;
  }
  this->cmd_write_buf_(opcode, buf, n);
}

// Two transactions, and that is not an accident - see the class comment.
// Between them BUSY must fall again: the chip is preparing the answer, and
// clocking early returns whatever was left in the shift register.
bool LR1121::cmd_read_(uint16_t opcode, std::initializer_list<uint8_t> args, uint8_t *out, size_t out_len) {
  uint8_t buf[16];
  size_t n = 0;
  for (auto b : args) {
    if (n >= sizeof(buf))
      break;
    buf[n++] = b;
  }
  this->cmd_write_buf_(opcode, buf, n);

  if (!this->wait_while_busy_()) {
    ESP_LOGW(TAG, "BUSY stuck high after command 0x%04X", opcode);
    return false;
  }

  this->delegate_->begin_transaction();
  (void) this->delegate_->transfer((uint8_t) 0x00);  // status byte, discarded
  for (size_t i = 0; i < out_len; i++)
    out[i] = this->delegate_->transfer((uint8_t) 0x00);
  this->delegate_->end_transaction();
  return true;
}

// ---------------------------------------------------------------------------
// Chip helpers
// ---------------------------------------------------------------------------

bool LR1121::get_version_(uint8_t &hw, uint8_t &type, uint16_t &fw) {
  uint8_t r[4]{};
  if (!this->cmd_read_(OC_GET_VERSION, {}, r, sizeof(r)))
    return false;
  hw = r[0];
  type = r[1];
  fw = (uint16_t) (((uint16_t) r[2] << 8) | r[3]);
  // All-zero or all-ones is not a version, it is an absent or mis-wired SPI
  // bus answering with idle levels.
  return !((r[0] == 0x00 && r[1] == 0x00 && r[2] == 0x00 && r[3] == 0x00) ||
           (r[0] == 0xFF && r[1] == 0xFF && r[2] == 0xFF && r[3] == 0xFF));
}

uint16_t LR1121::get_errors_() {
  uint8_t r[2]{};
  if (!this->cmd_read_(OC_GET_ERRORS, {}, r, sizeof(r)))
    return 0;
  return (uint16_t) (((uint16_t) r[0] << 8) | r[1]);
}

uint32_t LR1121::get_irq_status_() {
  // GetStatus is a DIRECT READ: no opcode is sent at all. The chip answers a
  // bare six-byte read with stat1, stat2 and the 32-bit IRQ word, MSB first
  // (Semtech lr11xx_system_get_status via lr11xx_hal_direct_read).
  //
  // Sending 0x0100 as a command here instead would look right and return
  // rubbish, because the first transaction would be interpreted as a command
  // and the answer would be one frame late.
  if (!this->wait_while_busy_())
    return 0;
  uint8_t r[6]{};
  this->delegate_->begin_transaction();
  for (size_t i = 0; i < sizeof(r); i++)
    r[i] = this->delegate_->transfer((uint8_t) 0x00);
  this->delegate_->end_transaction();
  return ((uint32_t) r[2] << 24) | ((uint32_t) r[3] << 16) | ((uint32_t) r[4] << 8) | (uint32_t) r[5];
}

void LR1121::set_sync_word_(uint8_t sync2) {
  // Sync word register is always eight bytes; wM-Bus uses the first two and the
  // remainder is ignored because sync_word_len_in_bits is set to 16.
  const uint8_t sw[8] = {0x54, sync2, 0, 0, 0, 0, 0, 0};
  this->cmd_write_buf_(OC_SET_GFSK_SYNC_WORD, sw, sizeof(sw));
}

int8_t LR1121::read_rssi_inst_dbm_() {
  uint8_t r = 0;
  if (!this->cmd_read_(OC_GET_RSSI_INST, {}, &r, 1))
    return RSSI_NOT_MEASURED;
  return (int8_t) (-(int8_t) (r >> 1));
}

void LR1121::read_packet_status_rssi_(uint8_t &raw_sync, uint8_t &raw_avg) {
  uint8_t r[4]{};
  raw_sync = 0;
  raw_avg = 0;
  if (!this->cmd_read_(OC_GET_PKT_STATUS, {}, r, sizeof(r)))
    return;
  raw_sync = r[0];
  raw_avg = r[1];
}

// ---------------------------------------------------------------------------
// Setup
//
// Order is load-bearing, and not the order the vendor examples use.
//
// The datasheet (section 1.2.4, Clock Sources) states that when a TCXO is
// fitted instead of a crystal, the chip skips ALL power-on calibrations and the
// host must configure the TCXO and re-launch them. The Waveshare examples do
// the opposite: they enter standby XOSC and run CalibrateImage before
// SetTcxoMode is ever issued, which calibrates against a clock that is not
// running yet - on top of calibrating the wrong band (see CAL_IMG_*).
//
// So: standby RC first (no oscillator needed), TCXO configured, and only then
// the calibrations.
// ---------------------------------------------------------------------------
void LR1121::setup() {
  this->common_setup();
  this->reset();

  // Wake-up pulse on NSS. The vendor HAL does this after reset and it costs
  // nothing; a chip already awake ignores it.
  this->delegate_->begin_transaction();
  this->delegate_->end_transaction();
  delay(1);

  if (!this->wait_while_busy_()) {
    ESP_LOGE(TAG, "BUSY never fell after reset - check wiring (BUSY, NSS, power)");
    this->mark_failed();
    return;
  }

  this->cmd_write_(OC_SET_STANDBY, {STANDBY_RC});
  this->cmd_write_(OC_SET_REG_MODE, {REG_MODE_DCDC});

  this->cmd_write_(OC_SET_DIO_AS_RFSW, {RFSW_ENABLE, RFSW_STANDBY, RFSW_RX, RFSW_TX, RFSW_TX_HP, RFSW_TX_HF,
                                        RFSW_GNSS, RFSW_WIFI});

  this->cmd_write_(OC_SET_TCXO_MODE, {(uint8_t) this->tcxo_voltage_,
                                      (uint8_t) (this->tcxo_startup_ticks_ >> 16),
                                      (uint8_t) (this->tcxo_startup_ticks_ >> 8),
                                      (uint8_t) (this->tcxo_startup_ticks_ >> 0)});

  // 32.768 kHz crystal, waiting for it to be ready (bit 2 of the argument).
  // The Waveshare board fits Y1 for this.
  this->cmd_write_(OC_CFG_LFCLK, {(uint8_t) (LFCLK_XTAL | (1 << 2))});

  this->cmd_write_(OC_CLEAR_ERRORS, {});
  this->cmd_write_(OC_CALIBRATE_IMAGE, {CAL_IMG_FREQ1_863MHZ, CAL_IMG_FREQ2_870MHZ});
  this->cmd_write_(OC_CALIBRATE, {CALIBRATE_ALL});

  this->boot_errors_ = this->get_errors_();
  this->cmd_write_(OC_CLEAR_ERRORS, {});

  this->boot_ok_ = this->get_version_(this->boot_hw_, this->boot_type_, this->boot_fw_);
  if (!this->boot_ok_) {
    ESP_LOGE(TAG, "No sane answer to GetVersion - SPI or BUSY wiring is wrong");
    this->mark_failed();
    return;
  }

  this->configure_gfsk_();
  this->restart_rx();

  this->log_reg_status();
}

void LR1121::configure_gfsk_() {
  this->cmd_write_(OC_SET_PKT_TYPE, {PKT_TYPE_GFSK});

  this->cmd_write_(OC_SET_RF_FREQUENCY, {(uint8_t) (this->configured_frequency_hz_ >> 24),
                                         (uint8_t) (this->configured_frequency_hz_ >> 16),
                                         (uint8_t) (this->configured_frequency_hz_ >> 8),
                                         (uint8_t) (this->configured_frequency_hz_ >> 0)});

  // RSSI calibration table for the 600 MHz - 2 GHz range.
  //
  // Easy to miss and expensive to miss in this project specifically: every
  // diagnostic here is built on reported dBm, and without this the chip uses
  // defaults that leave the gain steps uncorrected. The numbers would still
  // look plausible - just systematically wrong, which is the worst failure mode
  // a measurement can have.
  //
  // Values are Semtech's own (smtc_shield_lr11xx_common_rssi_calibration_table_
  // from_600mhz_to_2ghz in the Waveshare package), packed as the driver packs
  // them: two 4-bit gain tunes per byte, then g13hp7 alone, then a 16-bit
  // gain offset. g4..g13 -> 2,2,2,3,3,4,5,4,4,6; g13hp1..7 -> 5,5,6,6,6,7,6;
  // offset 0.
  //
  // The above-2 GHz table (offset 2030) is deliberately not carried: this
  // driver only ever tunes sub-GHz, and an unused second table is a thing that
  // rots.
  this->cmd_write_(OC_SET_RSSI_CALIBRATION, {0x22, 0x32, 0x43, 0x45, 0x64, 0x55, 0x66, 0x76, 0x06, 0x00, 0x00});

  this->cmd_write_(OC_SET_MODULATION_PARAM, {(uint8_t) (this->bitrate_bps_ >> 24),
                                             (uint8_t) (this->bitrate_bps_ >> 16),
                                             (uint8_t) (this->bitrate_bps_ >> 8),
                                             (uint8_t) (this->bitrate_bps_ >> 0),
                                             GFSK_PULSE_SHAPE_OFF,
                                             (uint8_t) this->rx_bandwidth_,
                                             (uint8_t) (this->deviation_hz_ >> 24),
                                             (uint8_t) (this->deviation_hz_ >> 16),
                                             (uint8_t) (this->deviation_hz_ >> 8),
                                             (uint8_t) (this->deviation_hz_ >> 0)});

  // preamble_len_in_bits is a transmit parameter; the receiver uses
  // preamble_detector instead. It is written because the command has the field,
  // not because it does anything here.
  this->cmd_write_(OC_SET_PKT_PARAM, {0x00, 32,
                                      (uint8_t) this->preamble_detector_,
                                      16,  // sync word length in BITS
                                      GFSK_ADDR_FILTER_DISABLE,
                                      GFSK_PKT_FIX_LEN,
                                      this->payload_length_,
                                      GFSK_CRC_OFF,
                                      GFSK_DC_FREE_OFF});

  this->cmd_write_(OC_SET_RX_BOOSTED, {(uint8_t) (this->rx_boosted_ ? 0x01 : 0x00)});

  const uint32_t mask = IRQ_RX_DONE | IRQ_TIMEOUT | IRQ_FSK_LEN_ERROR;
  this->cmd_write_(OC_SET_DIO_IRQ_PARAMS, {(uint8_t) (mask >> 24), (uint8_t) (mask >> 16), (uint8_t) (mask >> 8),
                                           (uint8_t) (mask >> 0),
                                           0x00, 0x00, 0x00, 0x00});  // DIO2: nothing

  char buf[96];
  snprintf(buf, sizeof(buf), "%.3f MHz, %u bps, fdev %u Hz, BW 0x%02X, len %u, boost %s",
           this->configured_frequency_hz_ / 1000000.0f, (unsigned) this->bitrate_bps_,
           (unsigned) this->deviation_hz_, (unsigned) this->rx_bandwidth_, (unsigned) this->payload_length_,
           this->rx_boosted_ ? "on" : "off");
  this->rf_params_str_ = buf;
}

// ---------------------------------------------------------------------------
// RX
// ---------------------------------------------------------------------------

void LR1121::restart_rx() {
  // Same sync-word cycling as the SX1262 driver, and for the same reason:
  // C-mode exists in two format variants that differ only in the second sync
  // byte (A = 0x3D, B = 0xCD). Listening on one of them silently drops the
  // other, so `both` and `c1` rotate 3:1 in favour of A.
  uint8_t sync2;
  if (this->listen_mode_ == LISTEN_MODE_T1) {
    sync2 = 0x3D;
  } else {
    sync2 = (this->sync_cycle_ == 3) ? 0xCD : 0x3D;
    this->sync_cycle_ = (uint8_t) ((this->sync_cycle_ + 1) & 0x03);
  }
  this->set_sync_word_(sync2);

  this->cmd_write_(OC_CLEAR_IRQ, {(uint8_t) (IRQ_ALL >> 24), (uint8_t) (IRQ_ALL >> 16), (uint8_t) (IRQ_ALL >> 8),
                                  (uint8_t) (IRQ_ALL >> 0)});
  this->cmd_write_(OC_SET_STANDBY, {STANDBY_XOSC});
  this->cmd_write_buf_(OC_SET_RX, RX_CONTINUOUS, sizeof(RX_CONTINUOUS));

  this->rx_loaded_ = false;
  this->rx_idx_ = 0;
  this->rx_len_ = 0;
  this->last_rssi_dbm_ = RSSI_NOT_MEASURED;
}

bool LR1121::load_rx_buffer_() {
  uint8_t st[2]{};
  if (!this->cmd_read_(OC_GET_RXBUFFER_STATUS, {}, st, sizeof(st)))
    return false;

  const uint8_t payload_len = st[0];
  const uint8_t start_ptr = st[1];
  if (payload_len == 0)
    return false;

  // Packet RSSI first: RssiSync is latched at sync-word detection and survives
  // the frame, whereas GetRssiInst after RX_DONE would measure the empty
  // channel. Reporting that would make every meter look identical - the exact
  // bug the SX1262 driver carries a warning about.
  uint8_t raw_sync = 0, raw_avg = 0;
  this->read_packet_status_rssi_(raw_sync, raw_avg);
  const int8_t inflight = this->read_rssi_inst_dbm_();
  const char *source = "none";
  if (raw_sync != 0) {
    this->last_rssi_dbm_ = (int8_t) (-(int8_t) (raw_sync >> 1));
    source = "RssiSync";
  } else if (raw_avg != 0) {
    this->last_rssi_dbm_ = (int8_t) (-(int8_t) (raw_avg >> 1));
    source = "RssiAvg";
  } else {
    this->last_rssi_dbm_ = RSSI_NOT_MEASURED;
  }

  this->rx_buffer_.assign(payload_len, 0);
  uint8_t r[2] = {start_ptr, payload_len};
  this->cmd_write_buf_(OC_READ_BUFFER8, r, sizeof(r));
  if (!this->wait_while_busy_())
    return false;
  this->delegate_->begin_transaction();
  (void) this->delegate_->transfer((uint8_t) 0x00);  // status byte
  for (size_t i = 0; i < this->rx_buffer_.size(); i++)
    this->rx_buffer_[i] = this->delegate_->transfer((uint8_t) 0x00);
  this->delegate_->end_transaction();

  this->rx_idx_ = 0;
  this->rx_len_ = this->rx_buffer_.size();
  this->rx_loaded_ = true;

  if (!this->rssi_diag_reported_ || this->diag_verbose_) {
    this->rssi_diag_ = RssiDiag{};
    this->rssi_diag_.path = "fifo";
    this->rssi_diag_.source = source;
    this->rssi_diag_.raw_sync = raw_sync;
    this->rssi_diag_.raw_avg = raw_avg;
    this->rssi_diag_.inflight = inflight;
    this->rssi_diag_.result = this->last_rssi_dbm_;
    this->rssi_diag_.captured = (uint16_t) this->rx_len_;
    this->rssi_diag_.exit_reason = "rx_done";
    this->rssi_diag_.first_len = (uint8_t) (this->rx_len_ < 8 ? this->rx_len_ : 8);
    for (uint8_t i = 0; i < this->rssi_diag_.first_len; i++)
      this->rssi_diag_.first_bytes[i] = this->rx_buffer_[i];
    this->rssi_diag_pending_ = true;
    this->rssi_diag_reported_ = true;
  }

  return true;
}

optional<uint8_t> LR1121::read() {
  if (!this->rx_loaded_) {
    // The IRQ line is the cheap test: no edge, no frame, no SPI traffic. Only
    // when it is asserted do we spend transactions asking the chip what it has.
    if (this->irq_pin_ == nullptr || !this->irq_pin_->digital_read())
      return {};
    if (!this->load_rx_buffer_())
      return {};
  }

  if (this->rx_idx_ < this->rx_len_)
    return this->rx_buffer_[this->rx_idx_++];
  return {};
}

int8_t LR1121::get_rssi() { return this->last_rssi_dbm_; }

const char *LR1121::get_name() { return TAG; }

bool LR1121::take_rssi_diag(RssiDiag &out) {
  if (!this->rssi_diag_pending_)
    return false;
  out = this->rssi_diag_;
  this->rssi_diag_pending_ = false;
  return true;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

static void lr1121_log_errors_(uint16_t errors) {
  if (errors == 0) {
    ESP_LOGCONFIG(TAG, "  Boot errors: none");
    return;
  }
  ESP_LOGW(TAG, "  Boot errors: 0x%04X%s%s%s%s%s%s%s%s", (unsigned) errors,
           (errors & ERR_LF_RC_CALIB) ? " LF_RC_CALIB" : "",
           (errors & ERR_HF_RC_CALIB) ? " HF_RC_CALIB" : "", (errors & ERR_ADC_CALIB) ? " ADC_CALIB" : "",
           (errors & ERR_PLL_CALIB) ? " PLL_CALIB" : "", (errors & ERR_IMG_CALIB) ? " IMG_CALIB" : "",
           (errors & ERR_HF_XOSC_START) ? " HF_XOSC_START" : "",
           (errors & ERR_LF_XOSC_START) ? " LF_XOSC_START" : "", (errors & ERR_PLL_LOCK) ? " PLL_LOCK" : "");

  // The one worth spelling out, because the fix is a config line and the
  // symptom otherwise looks like dead hardware.
  if (errors & ERR_HF_XOSC_START)
    ESP_LOGW(TAG, "  HF_XOSC_START: the 32 MHz TCXO did not start. Try the other tcxo_voltage "
                  "(1.8v / 3.0v) - the vendor package states both and neither is measured.");
}

void LR1121::log_reg_status() {
  ESP_LOGCONFIG(TAG, "  Chip: hw=0x%02X type=0x%02X fw=0x%04X", (unsigned) this->boot_hw_,
                (unsigned) this->boot_type_, (unsigned) this->boot_fw_);
  ESP_LOGCONFIG(TAG, "  RF: %s", this->rf_params_str_.c_str());
  lr1121_log_errors_(this->boot_errors_);
}

void LR1121::dump_debug_status(const char *reason) {
  const uint32_t irq = this->get_irq_status_();
  ESP_LOGW(TAG, "debug (%s): IRQ=0x%08X, errors=0x%04X, busy=%d", reason, (unsigned) irq,
           (unsigned) this->get_errors_(), this->busy_pin_ != nullptr ? (int) this->busy_pin_->digital_read() : -1);
}

}  // namespace wmbus_radio
}  // namespace esphome

#endif  // USE_WMBUS_RADIO_LR1121
