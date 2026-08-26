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
// Sync word detected. Not used to drive capture - the capture still starts on
// RX_DONE. Enabled in S1 only, as a probe: it is the one bit that separates
// "sync never matched" from "sync matched but the packet never completed", and
// those two have completely different fixes. Kept out of T1/C1 so a proven
// receive path is not disturbed by an extra interrupt source.
static constexpr uint32_t IRQ_SYNC_WORD_VALID = (1UL << 2);
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

// Per-command BUSY timeout lives in the header as a default argument (100 ms).
// The post-reset wait is separate and much longer - see setup(). The first
// version of this driver used one short timeout for both and failed the whole
// component before it ever spoke to the chip.

// ---------------------------------------------------------------------------
// SPI plumbing
// ---------------------------------------------------------------------------

bool LR1121::wait_while_busy_(uint32_t timeout_ms) {
  if (this->busy_pin_ == nullptr)
    return true;
  const uint32_t start = millis();
  while (this->busy_pin_->digital_read()) {
    if ((uint32_t) (millis() - start) > timeout_ms)
      return false;
    delay(1);  // yield; a tight spin here starves the idle task
  }
  return true;
}

void LR1121::wake_pulse_() {
  // One real byte, so the delegate definitely asserts and releases CS.
  this->delegate_->begin_transaction();
  (void) this->delegate_->transfer((uint8_t) 0x00);
  this->delegate_->end_transaction();
  delay(2);
}

void LR1121::cmd_write_buf_(uint16_t opcode, const uint8_t *args, size_t len) {
  if (!this->wait_while_busy_() && !this->busy_line_suspect_) {
    // Warn once, then carry on. Refusing to talk because BUSY looks stuck is
    // how a bad BUSY reading turns into a silent radio that cannot even be
    // interrogated.
    ESP_LOGW(TAG, "BUSY stuck high before command 0x%04X - sending anyway", opcode);
    this->busy_line_suspect_ = true;
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

  // Not fatal for the same reason as above: read the answer regardless and let
  // the caller judge it. A sane GetVersion arriving while BUSY reads high is
  // the single most useful diagnostic this driver can produce.
  (void) this->wait_while_busy_();

  this->delegate_->begin_transaction();
  // Semtech's lr11xx_hal_read clocks exactly one dummy/status byte before the
  // response payload.  Discarding two shifts every result: on LR1121 hardware
  // GetVersion then misleadingly reads type=0x01 even though the documented
  // LR1121 type is 0x03, and GetErrors moves valid low-byte flags into the
  // undefined high byte.
  (void) this->delegate_->transfer((uint8_t) 0x00);
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
  (void) this->wait_while_busy_();  // advisory, not a gate - see wait_while_busy_
  uint8_t r[6]{};
  this->delegate_->begin_transaction();
  for (size_t i = 0; i < sizeof(r); i++)
    r[i] = this->delegate_->transfer((uint8_t) 0x00);
  this->delegate_->end_transaction();
  return ((uint32_t) r[2] << 24) | ((uint32_t) r[3] << 16) | ((uint32_t) r[4] << 8) | (uint32_t) r[5];
}

// S-mode sync, same bytes the SX1276 and SX1262 drivers program: the 18-bit
// S-mode sync 0x7696 preceded by three "01" preamble bits, which packet radios
// express as 0x54 0x76 0x96. Twenty-four bits, not sixteen - configure_gfsk_()
// sizes the field from the listen mode for exactly this reason.
void LR1121::set_s1_sync_word_() {
  const uint8_t sw[8] = {0x54, 0x76, 0x96, 0, 0, 0, 0, 0};
  this->cmd_write_buf_(OC_SET_GFSK_SYNC_WORD, sw, sizeof(sw));
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

  // Wake-up pulse, then a generous wait. Two separate reasons for the length:
  // the chip loads its firmware out of NVM after NRESET is released, and the
  // vendor HAL allows seconds for BUSY on every single command - not the tens
  // of milliseconds a TCXO start would suggest. 100 ms was simply too short.
  this->wake_pulse_();
  bool busy_ok = this->wait_while_busy_(1000);
  if (!busy_ok) {
    // Second attempt with another NSS edge: a chip that came up in sleep needs
    // the pulse, and one pulse may land before it is listening.
    this->wake_pulse_();
    busy_ok = this->wait_while_busy_(1000);
  }

  // Ask the chip who it is EVEN IF BUSY still looks stuck. This is the whole
  // point: the answer separates "no chip / no SPI" from "chip fine, BUSY line
  // lying", and those two have completely different fixes.
  this->boot_ok_ = this->get_version_(this->boot_hw_, this->boot_type_, this->boot_fw_);

  if (!busy_ok) {
    this->busy_line_suspect_ = true;
    ESP_LOGW(TAG, "BUSY did not fall within 2 s after reset (pin reads %d)",
             this->busy_pin_ != nullptr ? (int) this->busy_pin_->digital_read() : -1);
    if (this->boot_ok_) {
      ESP_LOGW(TAG, "...but GetVersion answered hw=0x%02X type=0x%02X fw=0x%04X.",
               (unsigned) this->boot_hw_, (unsigned) this->boot_type_, (unsigned) this->boot_fw_);
      ESP_LOGW(TAG, "   The chip is alive and talking - suspect the BUSY line itself "
                    "(wrong busy_pin, floating input, board revision), not the radio.");
    } else {
      ESP_LOGE(TAG, "...and GetVersion returned nothing sane. SPI, power or reset wiring.");
    }
  }

  if (!this->boot_ok_) {
    ESP_LOGE(TAG, "No sane answer to GetVersion - check SPI (CLK/MOSI/MISO/CS) and reset_pin");
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

  // Calibrations that exercise the RF PLL need the 32 MHz reference running.
  // SetTcxoMode only tells the chip how to power and start that reference; it
  // does not itself leave standby RC.  The first hardware run with correctly
  // framed GetErrors proved the distinction: HF_XOSC_START was clear, but the
  // calibration ended with PLL_LOCK.  Enter XOSC explicitly after configuring
  // the TCXO and before launching either calibration.
  this->cmd_write_(OC_CLEAR_ERRORS, {});
  this->cmd_write_(OC_SET_STANDBY, {STANDBY_XOSC});
  // Give the reference the full startup window before judging it. The default
  // per-command wait is 100 ms, while tcxo_startup_ticks is counted in 32.768
  // kHz ticks - 3000 of them is ~91.6 ms, so the old margin was 8 ms on a board
  // whose BUSY line is not fully trusted. Reading GetErrors mid-startup is a
  // good way to latch a failure that is not one.
  (void) this->wait_while_busy_(1000);
  const uint16_t errors_after_xosc = this->get_errors_();

  this->cmd_write_(OC_CLEAR_ERRORS, {});
  this->cmd_write_(OC_CALIBRATE_IMAGE, {CAL_IMG_FREQ1_863MHZ, CAL_IMG_FREQ2_870MHZ});
  const uint16_t errors_after_image = this->get_errors_();

  this->cmd_write_(OC_CLEAR_ERRORS, {});
  this->cmd_write_(OC_CALIBRATE, {CALIBRATE_ALL});
  // Calibrate(0x3F) is the longest system command.  On this board the BUSY
  // input has already proved unreliable while SPI itself works (GetVersion,
  // RSSI and RX FIFO all answer), so BUSY must remain advisory rather than
  // becoming a fatal gate again.  Give calibration the same one-second window
  // used by the vendor HAL, then read its result.  This avoids the premature
  // 100 ms GetErrors that produced the impossible value 0x1300 without
  // disabling a working receiver when GPIO41 stays asserted.
  (void) this->wait_while_busy_(1000);
  const uint16_t errors_after_calibrate = this->get_errors_();

  // Keep the aggregate for the normal boot report, but expose the individual
  // stages at INFO while this new board is being brought up.  PLL_LOCK is a
  // sticky bit; without clearing between stages the final 0x0080 cannot tell
  // whether entering XOSC, image calibration or the full calibration raised
  // it.  Reading and clearing the diagnostic latch does not undo calibration.
  this->errors_after_xosc_ = errors_after_xosc;
  this->errors_after_image_ = errors_after_image;
  this->errors_after_calibrate_ = errors_after_calibrate;
  this->boot_errors_ = errors_after_xosc | errors_after_image | errors_after_calibrate;
  this->cmd_write_(OC_CLEAR_ERRORS, {});

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

  // S-mode runs at 32768 b/s, not 100000. The bitrate is a YAML option, so an
  // explicit value always wins; this only supplies the right default when the
  // user picked listen_mode: s1 and left the T-mode number alone. The effective
  // value is printed on the RF line below, so the substitution is never silent.
  uint32_t bitrate = this->bitrate_bps_;
  if (this->listen_mode_ == LISTEN_MODE_S1 && bitrate == 100000UL)
    bitrate = 32768UL;

  this->cmd_write_(OC_SET_MODULATION_PARAM, {(uint8_t) (bitrate >> 24),
                                             (uint8_t) (bitrate >> 16),
                                             (uint8_t) (bitrate >> 8),
                                             (uint8_t) (bitrate >> 0),
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
                                      (uint8_t) (this->listen_mode_ == LISTEN_MODE_S1 ? 24 : 16),
                                      GFSK_ADDR_FILTER_DISABLE,
                                      GFSK_PKT_FIX_LEN,
                                      this->payload_length_,
                                      GFSK_CRC_OFF,
                                      GFSK_DC_FREE_OFF});

  this->cmd_write_(OC_SET_RX_BOOSTED, {(uint8_t) (this->rx_boosted_ ? 0x01 : 0x00)});

  uint32_t mask = IRQ_RX_DONE | IRQ_TIMEOUT | IRQ_FSK_LEN_ERROR;
  if (this->listen_mode_ == LISTEN_MODE_S1)
    mask |= IRQ_SYNC_WORD_VALID;
  this->cmd_write_(OC_SET_DIO_IRQ_PARAMS, {(uint8_t) (mask >> 24), (uint8_t) (mask >> 16), (uint8_t) (mask >> 8),
                                           (uint8_t) (mask >> 0),
                                           0x00, 0x00, 0x00, 0x00});  // DIO2: nothing

  char buf[96];
  snprintf(buf, sizeof(buf), "%.3f MHz, %u bps, fdev %u Hz, BW 0x%02X, len %u, boost %s",
           this->configured_frequency_hz_ / 1000000.0f, (unsigned) bitrate,
           (unsigned) this->deviation_hz_, (unsigned) this->rx_bandwidth_, (unsigned) this->payload_length_,
           this->rx_boosted_ ? "on" : "off");
  this->rf_params_str_ = buf;

  if (this->listen_mode_ == LISTEN_MODE_S1) {
    ESP_LOGI(TAG, "S1: modem 32768 b/s, sync 0x54 0x76 0x96 (24 bit). Capture starts on "
                  "RX_DONE against the fixed length and the host trims - measured working "
                  "2026-08-19, Format A, 85 B decoded from a 255 B capture at -59 dBm. "
                  "Sensitivity at the margin is NOT established; that needs a weak real "
                  "transmitter, not a bench generator.");
  }
}

// ---------------------------------------------------------------------------
// RX
// ---------------------------------------------------------------------------

void LR1121::restart_rx() {
  // Same sync-word cycling as the SX1262 driver, and for the same reason:
  // C-mode exists in two format variants that differ only in the second sync
  // byte (A = 0x3D, B = 0xCD). Listening on one of them silently drops the
  // other, so `both` and `c1` rotate 3:1 in favour of A.
  if (this->listen_mode_ == LISTEN_MODE_S1) {
    this->set_s1_sync_word_();
    this->cmd_write_(OC_CLEAR_IRQ, {(uint8_t) (IRQ_ALL >> 24), (uint8_t) (IRQ_ALL >> 16),
                                    (uint8_t) (IRQ_ALL >> 8), (uint8_t) (IRQ_ALL >> 0)});
    this->cmd_write_(OC_SET_STANDBY, {STANDBY_XOSC});
    this->cmd_write_buf_(OC_SET_RX, RX_CONTINUOUS, sizeof(RX_CONTINUOUS));
    this->rx_loaded_ = false;
    this->rx_idx_ = 0;
    this->rx_len_ = 0;
    this->last_rssi_dbm_ = RSSI_NOT_MEASURED;
    return;
  }

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
  if (payload_len == 0) {
    // In S1 the IRQ line also carries SYNC_WORD_VALID, so landing here means the
    // sync word matched and no packet followed. That is the decisive observation
    // for whether a SYNC_WORD_VALID-driven capture path is needed at all - say it
    // once, and clear the latch so the line does not stay asserted.
    if (this->listen_mode_ == LISTEN_MODE_S1) {
      if (!this->s1_sync_seen_) {
        this->s1_sync_seen_ = true;
        ESP_LOGW(TAG, "S1: sync word matched but no packet completed. The modem and sync "
                      "are right; RX_DONE is what does not arrive. This is the case that "
                      "needs a SYNC_WORD_VALID-driven capture (see the SX1262 S1 path).");
      }
      this->cmd_write_(OC_CLEAR_IRQ, {(uint8_t) (IRQ_ALL >> 24), (uint8_t) (IRQ_ALL >> 16),
                                      (uint8_t) (IRQ_ALL >> 8), (uint8_t) (IRQ_ALL >> 0)});
    }
    return false;
  }

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
  (void) this->wait_while_busy_();  // advisory, not a gate
  this->delegate_->begin_transaction();
  // Same single dummy/status byte as lr11xx_hal_read and cmd_read_().
  (void) this->delegate_->transfer((uint8_t) 0x00);
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

// Live channel reading, unlike get_rssi() which returns the value cached at the
// last packet capture. read_rssi_inst_dbm_() already existed for internal use;
// this only exposes it through the transceiver interface.
bool LR1121::read_channel_rssi_dbm(int8_t *out) {
  const int8_t rssi = this->read_rssi_inst_dbm_();
  if (rssi == RSSI_NOT_MEASURED || rssi >= 0)
    return false;
  if (out != nullptr)
    *out = rssi;
  return true;
}

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
  if (errors & ERR_HF_XOSC_START) {
    ESP_LOGW(TAG, "  HF_XOSC_START: latched while entering STDBY_XOSC. Read the stage line above "
                  "before acting on this.");
    ESP_LOGW(TAG, "   IMAGE and ALL clean + frames arriving = startup transient, ignore it: "
                  "measured on the Waveshare HF board at tcxo_voltage 3.0v, which receives fine.");
    ESP_LOGW(TAG, "   IMAGE or ALL also failing = the 32 MHz reference really is not running; "
                  "that is when tcxo_voltage / tcxo_startup_ticks are worth changing.");
  }
}

// Receiver bandwidth in Hz for the exposed codes, so the sanity block can check
// it against the signal instead of printing a hex code nobody can judge.
static uint32_t lr1121_bw_hz_(uint8_t code) {
  switch (code) {
    case LR1121_BW_234300: return 234300;
    case LR1121_BW_312000: return 312000;
    case LR1121_BW_373600: return 373600;
    case LR1121_BW_467000: return 467000;
    default: return 0;
  }
}

void LR1121::log_reg_status() {
  // INFO, not LOGCONFIG. This is the evidence that SPI and the chip answer at
  // all, and LOGCONFIG sits BELOW info in ESPHome's level order - so on a
  // perfectly ordinary `logger: level: info` it would never be printed, which
  // is precisely when someone needs it most.
  ESP_LOGI(TAG, "  Chip: hw=0x%02X type=0x%02X fw=0x%04X", (unsigned) this->boot_hw_,
           (unsigned) this->boot_type_, (unsigned) this->boot_fw_);
  ESP_LOGCONFIG(TAG, "  RF: %s", this->rf_params_str_.c_str());

  // YAML sanity, same idea as the SX1262 block in component.cpp: echo what was
  // chosen and say what it means, at INFO, where it is actually visible. Kept in
  // the driver because the driver already holds these values - the SX1262 path
  // copies them into the component, which is plumbing this does not need.
  ESP_LOGI(TAG, "LR1121 YAML sanity / sprawdzenie YAML LR1121:");

  if (this->tcxo_voltage_ == LR1121_TCXO_3_0V) {
    ESP_LOGI(TAG, "  tcxo_voltage: 3.0v -> measured working on the Waveshare HF board / "
                  "zmierzone jako dzialajace na plytce Waveshare HF");
  } else if (this->tcxo_voltage_ == LR1121_TCXO_1_8V) {
    ESP_LOGW(TAG, "  tcxo_voltage: 1.8v -> RISK(!): the 32 MHz TCXO did not start at this "
                  "setting on the Waveshare HF board / na tej plytce TCXO nie wystartowal "
                  "przy tym ustawieniu");
  } else {
    ESP_LOGW(TAG, "  tcxo_voltage: code 0x%02X -> untested on this board; 3.0v is the "
                  "measured one / nietestowane na tej plytce",
             (unsigned) this->tcxo_voltage_);
  }
  ESP_LOGI(TAG, "  tcxo_startup_ticks: %u (~%u ms at 32.768 kHz)",
           (unsigned) this->tcxo_startup_ticks_,
           (unsigned) ((this->tcxo_startup_ticks_ * 1000UL) / 32768UL));

  const uint32_t bw = lr1121_bw_hz_((uint8_t) this->rx_bandwidth_);
  const uint32_t needed = 2UL * this->deviation_hz_ + this->bitrate_bps_;
  if (bw >= needed) {
    ESP_LOGI(TAG, "  rx_bandwidth: %u Hz -> covers 2*fdev+bitrate = %u Hz / pokrywa wymagane %u Hz",
             (unsigned) bw, (unsigned) needed, (unsigned) needed);
  } else {
    ESP_LOGW(TAG, "  rx_bandwidth: %u Hz -> RISK(!): narrower than 2*fdev+bitrate = %u Hz, "
                  "frames will be clipped / wezsze niz wymagane %u Hz",
             (unsigned) bw, (unsigned) needed, (unsigned) needed);
  }

  if (this->payload_length_ >= 255) {
    ESP_LOGI(TAG, "  payload_length: 255 -> full capture; host trims / pelne przechwycenie, "
                  "host przycina");
  } else {
    ESP_LOGW(TAG, "  payload_length: %u -> RISK(!): frames longer than this are truncated; "
                  "NES telegrams arrive as 245 raw bytes / dluzsze ramki beda ucinane, "
                  "telegramy NES maja 245 bajtow surowych",
             (unsigned) this->payload_length_);
  }

  ESP_LOGI(TAG, "  rx_boosted: %s%s", this->rx_boosted_ ? "true" : "false",
           this->rx_boosted_ ? " -> +2 dB for ~2 mA / +2 dB kosztem ~2 mA"
                             : " -> 2 dB of sensitivity left on the table / oddane 2 dB czulosci");

  if (this->listen_mode_ == LISTEN_MODE_S1) {
    ESP_LOGI(TAG, "  listen_mode: s1 -> 32768 b/s, sync 0x54 0x76 0x96 (24 bit), 868.300 MHz. "
                  "Measured working; margin sensitivity unknown / zmierzone jako dzialajace, "
                  "czulosc na granicy niezbadana");
  }

  // The known-benign signature, measured on this board: the flag latches while
  // entering STDBY_XOSC, both calibrations that follow are clean, and reception
  // works. Saying that in three warning lines on every single boot is how a log
  // teaches people to stop reading warnings. One calm line is enough.
  const bool benign_xosc = this->boot_errors_ == ERR_HF_XOSC_START &&
                           this->errors_after_image_ == 0 && this->errors_after_calibrate_ == 0;
  if (benign_xosc) {
    ESP_LOGI(TAG, "  Calibration stages: XOSC=0x%04X IMAGE=0x0000 ALL=0x0000 - HF_XOSC_START "
                  "latched at XOSC entry, calibrations clean. Known transient on this board, "
                  "not a fault.",
             (unsigned) this->errors_after_xosc_);
    return;
  }

  ESP_LOGI(TAG, "  Calibration stages: XOSC=0x%04X IMAGE=0x%04X ALL=0x%04X",
           (unsigned) this->errors_after_xosc_, (unsigned) this->errors_after_image_,
           (unsigned) this->errors_after_calibrate_);
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
