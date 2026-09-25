// SPDX-License-Identifier: GPL-3.0-or-later
#include "transceiver_lr1121.h"

#ifdef USE_WMBUS_RADIO_LR1121

#include "frame_length.h"

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
static constexpr uint16_t OC_READ_REGMEM32      = 0x0106;
static constexpr uint16_t OC_WRITE_REGMEM32_MASK = 0x010C;
static constexpr uint16_t OC_CFG_LFCLK          = 0x0116;
static constexpr uint16_t OC_SET_TCXO_MODE      = 0x0117;
static constexpr uint16_t OC_SET_STANDBY        = 0x011C;

// Read-only probe of four undocumented addresses, to establish whether they
// respond on this part at all before anything is ever written to one.
//   0x00F20384, 0x00F20368 - the position counter / end-of-packet pair that
//     Semtech's own Sidewalk driver (Lora-net/SWDR007, SWSD006,
//     lr11xx_radio_fsk.c) polls and writes on LR11xx, with no named macro
//     anywhere in the public SDK.
//   0x00F30028, 0x00F30030 - Rx FIFO base address and size on the LR20xx
//     successor, documented in the LR2021 datasheet rev 2.2 Tables 5-2/5-3.
//     Whether LR1121 has anything wired up there is exactly the question.
// Nothing here writes. A value that never changes, or reads as all-zero or
// all-ones, is a result and not a failure.
static constexpr uint32_t PROBE_ADDR[4] = {0x00F20384, 0x00F20368, 0x00F30028, 0x00F30030};

// Expected-packet-length register, established on hardware 2026-09-22: its
// [31:20] field read 255 with the radio configured for 255 and NOTHING yet
// received, then 64 after payload_length was changed to 64 - so it mirrors
// SetPacketParams rather than holding the last packet's length. The field is
// 12-bit and can express 4095, far past the 8-bit pld_len_in_bytes of the
// public API. Same register Semtech's Sidewalk driver patches mid-reception.
static constexpr uint32_t REG_EXPECTED_LEN = 0x00F20368;
static constexpr uint32_t REG_EXPECTED_LEN_MASK = 0xFFF00000;
static constexpr uint8_t REG_EXPECTED_LEN_SHIFT = 20;

// The override is verified against exactly one radio firmware. An undocumented
// register is a property of the firmware image, not a promise, so a different
// image must disable the path loudly rather than write blind (see the
// firmware-pinning rule in the investigation note).
static constexpr uint16_t VERIFIED_RADIO_FW = 0x0101;

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
// Sync word detected, UM 2.2 pp.37-39. Bit 5.
//
// This constant said (1UL << 2) until 2026-09-22. Bit 2 is TX_DONE, which in a
// receive-only driver never fires, so the S1 mask that included it has been
// carrying a dead bit and the "sync matched but no packet" diagnostic behind it
// has never once run. Correcting the number is therefore not a behaviour change
// for S1 - but *enabling* the real bit there would be, and the S1 receive
// dispatcher is not ready for an early interrupt (it clears the whole IRQ latch
// on this path, which would take RX_DONE with it). So S1 no longer asks for it
// at all, and T1/C1 ask only when lr1121_sync_probe is set.
static constexpr uint32_t IRQ_SYNC_WORD_VALID = (1UL << 5);
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
    if ((uint32_t) (millis() - start) > timeout_ms) {
      this->busy_timeouts_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
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
  const uint8_t stat1 = this->delegate_->transfer((uint8_t) (opcode >> 8));
  const uint8_t stat2 = this->delegate_->transfer((uint8_t) (opcode & 0xFF));
  this->observe_stat1_(stat1);
  this->last_status_.store(((uint32_t) stat1 << 8) | stat2, std::memory_order_relaxed);
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
  this->observe_stat1_(this->delegate_->transfer((uint8_t) 0x00));
  for (size_t i = 0; i < out_len; i++)
    out[i] = this->delegate_->transfer((uint8_t) 0x00);
  this->delegate_->end_transaction();
  return true;
}

// ReadRegMem32, one word. Wire format taken from SWDR001 2.4.1 lr11xx_regmem.c:
// opcode 0x0106, then the address big-endian, then the word count; the response
// words come back big-endian too.
uint32_t LR1121::read_regmem32_(uint32_t address) {
  uint8_t raw[4]{};
  this->cmd_read_(OC_READ_REGMEM32,
                  {(uint8_t) (address >> 24), (uint8_t) (address >> 16), (uint8_t) (address >> 8),
                   (uint8_t) (address >> 0), 1},
                  raw, sizeof(raw));
  return ((uint32_t) raw[0] << 24) | ((uint32_t) raw[1] << 16) | ((uint32_t) raw[2] << 8) |
         (uint32_t) raw[3];
}

// Copy out everything received up to `target`, wrap-aware. Only ever moves
// forward: drain_len_ is how much of this frame is already in hand.
void LR1121::drain_up_to_(uint32_t target) {
  uint8_t status[2]{};
  this->cmd_read_(OC_GET_RXBUFFER_STATUS, {}, status, sizeof(status));
  if (target > DRAIN_CAP) target = DRAIN_CAP;
  while (this->drain_len_ < target) {
    const uint16_t off = (uint16_t) (this->drain_len_ % 256);
    uint16_t chunk = (uint16_t) (target - this->drain_len_);
    if (chunk > (uint16_t) (256 - off)) chunk = (uint16_t) (256 - off);  // stop at the ring seam
    if (chunk > 255) chunk = 255;                                        // ReadBuffer8 length is 8-bit
    this->drain_sample_.trace_total++;
    if (this->drain_sample_.trace_count < DRAIN_TRACE_CAP) {
      auto &trace = this->drain_sample_.trace[this->drain_sample_.trace_count++];
      trace = {micros() - this->drain_started_us_, (uint16_t) target, this->drain_len_,
               status[0], status[1], (uint8_t) off, (uint8_t) chunk};
    }
    uint8_t tmp[255];
    this->cmd_read_(OC_READ_BUFFER8, {(uint8_t) off, (uint8_t) chunk}, tmp, chunk);
    for (uint16_t i = 0; i < chunk; i++) this->drain_buf_[this->drain_len_ + i] = tmp[i];
    this->drain_len_ = (uint16_t) (this->drain_len_ + chunk);
  }
}

void LR1121::probe_registers_(uint32_t out[4]) {
  for (size_t i = 0; i < 4; i++) out[i] = this->read_regmem32_(PROBE_ADDR[i]);
}

// WriteRegMem32Mask, opcode 0x010C: address, mask and data, all big-endian,
// no response. Format from SWDR001 2.4.1 lr11xx_regmem.c.
void LR1121::write_regmem32_mask_(uint32_t address, uint32_t mask, uint32_t data) {
  const uint8_t args[12] = {
      (uint8_t) (address >> 24), (uint8_t) (address >> 16), (uint8_t) (address >> 8), (uint8_t) address,
      (uint8_t) (mask >> 24),    (uint8_t) (mask >> 16),    (uint8_t) (mask >> 8),    (uint8_t) mask,
      (uint8_t) (data >> 24),    (uint8_t) (data >> 16),    (uint8_t) (data >> 8),    (uint8_t) data};
  this->cmd_write_buf_(OC_WRITE_REGMEM32_MASK, args, sizeof(args));
}

// Writes the expected-packet-length field. Callers outside setup must check
// boot_fw_ themselves - an undocumented register is a property of one firmware
// image, not a promise.
void LR1121::write_expected_len_(uint16_t len) {
  this->write_regmem32_mask_(REG_EXPECTED_LEN, REG_EXPECTED_LEN_MASK,
                             ((uint32_t) len) << REG_EXPECTED_LEN_SHIFT);
}

void LR1121::apply_expected_len_override_() {
  // Runs in the receiver task, so it does NOT log: output from that task never
  // reaches the API log stream (see RadioTransceiver::RssiDiag). Saying
  // "EXPERIMENT ACTIVE" from here printed nothing at all, which is the exact
  // opposite of what an undocumented-register write must do. The loud line
  // lives in the YAML sanity block instead, on the main task.
  // auto_length_ arms the engine with a ceiling instead of a fixed length; the
  // real length replaces it mid-frame once the L-field has been read.
  const uint16_t value = this->auto_length_ ? AUTO_LEN_CEILING : this->expected_len_override_;
  if (value == 0) return;
  if (this->boot_fw_ != VERIFIED_RADIO_FW) return;
  this->write_expected_len_(value);
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
  this->observe_stat1_(r[0]);
  this->last_status_.store(((uint32_t) r[0] << 8) | r[1], std::memory_order_relaxed);
  const uint32_t irq = ((uint32_t) r[2] << 24) | ((uint32_t) r[3] << 16) | ((uint32_t) r[4] << 8) | r[5];
  this->last_irq_.store(irq, std::memory_order_relaxed);
  return irq;
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
  this->packet_samples_.fetch_add(1, std::memory_order_relaxed);
  this->last_packet_status_.store(((uint32_t) r[2] << 8) | r[3], std::memory_order_relaxed);
  if (r[3] & 0x02) this->packet_received_.fetch_add(1, std::memory_order_relaxed);
  if (r[3] & 0x04) this->packet_abort_.fetch_add(1, std::memory_order_relaxed);
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
  this->raw_sample_queue_ = xQueueCreate(2, sizeof(RawRxSample));
  if (this->drain_) {
    this->drain_sample_queue_ = xQueueCreate(1, sizeof(DrainSample));
    if (this->drain_sample_queue_ == nullptr)
      ESP_LOGW(TAG, "Drain diagnostic queue allocation failed");
  }
  if (this->raw_sample_queue_ == nullptr)
    ESP_LOGW(TAG, "Raw diagnostic queue unavailable; reception remains enabled");
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

  // At-rest baseline, deliberately taken after the radio is configured but
  // before RX is ever armed: nothing has been received yet, so whatever these
  // addresses hold now cannot be a position counter's value. Every later probe
  // in the FIFO samples is only interpretable against this line. Read-only.
  //
  // Only when something actually uses those registers. On an ordinary node the
  // baseline calibrates nothing, so reading four undocumented addresses there
  // buys nobody anything.
  if (this->undocumented_register_work_()) {
    this->probe_registers_(this->probe_baseline_);
    ESP_LOGI(TAG, "Register probe baseline (pre-RX, read-only): "
                  "%08X=0x%08X %08X=0x%08X %08X=0x%08X %08X=0x%08X",
             (unsigned) PROBE_ADDR[0], (unsigned) this->probe_baseline_[0],
             (unsigned) PROBE_ADDR[1], (unsigned) this->probe_baseline_[1],
             (unsigned) PROBE_ADDR[2], (unsigned) this->probe_baseline_[2],
             (unsigned) PROBE_ADDR[3], (unsigned) this->probe_baseline_[3]);
  }

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
  // Step A of the long-packet work: ask for an early wake so the live position
  // counter can be sampled while a frame is still arriving. Draining a frame
  // longer than the buffer has to start before the ring wraps, and nothing in
  // the normal path wakes early enough to even look.
  if (this->sync_probe_) mask |= IRQ_SYNC_WORD_VALID;
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
  // S1 returns early because its sync word is three bytes rather than two. Every
  // other step below has to be repeated here by hand, which is exactly how the
  // expected-length register came to be skipped in this mode until 2026-09-23:
  // the override was written on the path below and nowhere else, so in S1 the
  // engine kept stopping at payload_length_ while the drain was told to expect
  // more. Symptom was specific and readable - ptr_max stuck at 254 on a
  // 580-byte frame while drain_bytes_last said 580, i.e. the counter stopped at
  // the real end and the tail read was blind. Anything added below belongs here
  // too.
  if (this->listen_mode_ == LISTEN_MODE_S1) {
    this->set_s1_sync_word_();
    this->cmd_write_(OC_CLEAR_IRQ, {(uint8_t) (IRQ_ALL >> 24), (uint8_t) (IRQ_ALL >> 16),
                                    (uint8_t) (IRQ_ALL >> 8), (uint8_t) (IRQ_ALL >> 0)});
    this->cmd_write_(OC_SET_STANDBY, {STANDBY_XOSC});
    // In standby, after SetPacketParams has had its say and before RX is armed -
    // same placement and same reason as on the path below.
    this->apply_expected_len_override_();
    this->cmd_write_buf_(OC_SET_RX, RX_CONTINUOUS, sizeof(RX_CONTINUOUS));
    this->rx_loaded_ = false;
    this->rx_idx_ = 0;
    this->rx_len_ = 0;
    this->drain_ready_ = 0;
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
  // In standby, after SetPacketParams has had its say and before RX is armed.
  this->apply_expected_len_override_();
  this->cmd_write_buf_(OC_SET_RX, RX_CONTINUOUS, sizeof(RX_CONTINUOUS));

  this->rx_loaded_ = false;
  this->rx_idx_ = 0;
  this->rx_len_ = 0;
  this->drain_ready_ = 0;
  this->last_rssi_dbm_ = RSSI_NOT_MEASURED;
}

bool LR1121::load_rx_buffer_() {
  // Cleared on entry, not only in restart_rx(): serving the decoder a stale
  // drain from the previous frame would be indistinguishable from a decode
  // failure, so this must not depend on who called us or in what order.
  this->drain_ready_ = 0;
  const bool sample_due = this->raw_sample_queue_ != nullptr &&
      (uint32_t) (millis() - this->last_raw_sample_ms_) >= 5000;
  const bool verify_requested = this->verify_buffer_ && sample_due &&
      (this->last_irq_.load() & IRQ_RX_DONE) != 0;
  const uint32_t busy_before = this->busy_timeouts_.load();
  const uint32_t fail_before = this->status_fail_.load();
  const uint32_t perr_before = this->status_perr_.load();
  bool frozen = false;
  if (verify_requested) {
    // UM 2.2 pp.16,35,88: stop RX, then read the addressable retained RAM.
    // Unlike an ordinary passive sample, this deliberately changes RX timing.
    this->cmd_write_(OC_SET_STANDBY, {STANDBY_XOSC});
    (void) this->get_irq_status_();
    const uint32_t status = this->last_status_.load();
    frozen = ((status >> 1) & 7) == 2 && ((status >> 9) & 7) == 2 &&
        this->busy_timeouts_.load() == busy_before;
  }
  uint8_t st[2]{};
  if (!this->cmd_read_(OC_GET_RXBUFFER_STATUS, {}, st, sizeof(st)))
    return false;

  uint8_t payload_len = st[0];
  uint8_t start_ptr = st[1];
  if (payload_len == 0 && this->sync_probe_) {
    // The sync-word wake: a frame is arriving and nothing has landed yet.
    //
    // S1 is included since 2026-09-23, as a measurement rather than a fix. In
    // S1 the sync word matches and RX_DONE never arrives, and nothing so far
    // could tell "the modem hears nothing" from "the modem hears it and only
    // the completion is missing". Polling 0x00F20384 here separates the two:
    // a counter that advances means bytes are landing in the buffer and only
    // the packet engine's end condition is absent, which is a different bug
    // with a different fix. Capture behaviour is unchanged - the attempt still
    // ends in the same failure it did before, just with the counter recorded.
    //
    // This must NOT hand a failed attempt back to the caller. receive_frame()
    // begins every attempt with restart_rx() - SetStandby(XOSC) + SetRx - so
    // returning here aborts the frame that is still on air. Measured twice:
    // 59 sync wakes per minute, zero captures, because every early wake threw
    // away the packet it had just announced. The handoff of 2026-09-06 called
    // this exactly ("przedwczesny odczyt/restart") and it was enabled anyway.
    //
    // So the wake is absorbed here: poll the position counter until the packet
    // completes, then fall through into the ordinary capture. The caller never
    // learns an early wake happened. This is also the shape the eventual drain
    // needs, so it is not scaffolding.
    this->sync_wakes_.fetch_add(1, std::memory_order_relaxed);
    this->cmd_write_(OC_CLEAR_IRQ, {(uint8_t) (IRQ_SYNC_WORD_VALID >> 24), (uint8_t) (IRQ_SYNC_WORD_VALID >> 16),
                                    (uint8_t) (IRQ_SYNC_WORD_VALID >> 8), (uint8_t) (IRQ_SYNC_WORD_VALID >> 0)});

    // With auto_length_ the engine was armed with the ceiling, so that is what
    // to expect until the L-field says otherwise. `expect` stops being const:
    // resolving the length is the whole point.
    uint32_t expect = this->auto_length_        ? AUTO_LEN_CEILING
                      : this->expected_len_override_ != 0 ? this->expected_len_override_
                                                          : this->payload_length_;
    bool len_resolved = !this->auto_length_;
    // Same substitution restart_rx() makes: S1 runs at 32768 b/s unless the
    // YAML overrode it, and bitrate_bps_ still holds the T-mode default. Using
    // it unadjusted would give a deadline three times too short and turn every
    // S1 frame into a timeout that says nothing.
    uint32_t eff_bitrate = this->bitrate_bps_ != 0 ? this->bitrate_bps_ : 100000UL;
    if (this->listen_mode_ == LISTEN_MODE_S1 && eff_bitrate == 100000UL) eff_bitrate = 32768UL;
    const uint32_t air_ms = (expect * 8UL * 1000UL) / eff_bitrate;
    const uint32_t deadline = millis() + air_ms + 50;
    uint32_t irq_now = 0;
    this->drain_len_ = 0;
    this->drain_ready_ = 0;
    this->drain_started_us_ = micros();
    this->drain_sample_.trace_count = 0;
    this->drain_sample_.trace_total = 0;
    while ((int32_t) (millis() - deadline) < 0) {
      const uint32_t pos = (this->read_regmem32_(PROBE_ADDR[0]) & 0x0FFF0000) >> 16;
      this->sync_polls_.fetch_add(1, std::memory_order_relaxed);
      this->sync_ptr_last_.store(pos, std::memory_order_relaxed);
      uint32_t prev = this->sync_ptr_max_.load(std::memory_order_relaxed);
      while (pos > prev && !this->sync_ptr_max_.compare_exchange_weak(prev, pos)) {
      }
      // The counter is an absolute count of bytes received in this frame: it
      // reached 325 on a 326-byte frame, so it does not wrap at 256 even though
      // the buffer does. Byte k therefore sits at buffer position k % 256 and
      // survives until byte k+256 arrives. Measured margin at 100 kb/s: one
      // poll every ~2.8 ms against a 20.5 ms overwrite deadline.
      if (this->drain_) this->drain_up_to_(pos);
      if (!len_resolved && this->drain_len_ >= 4) {
        // Read the L-field out of what has landed and tell the engine where the
        // frame really ends. This is how Semtech's own Sidewalk driver uses
        // this register - it writes an end-of-packet value mid-reception - and
        // it is the only way a fixed-length engine can stop at a length it
        // could not know when RX was armed.
        const std::vector<uint8_t> head(this->drain_buf_,
                                        this->drain_buf_ + (this->drain_len_ < 32 ? this->drain_len_ : 32));
        size_t n = 0;
        switch (this->listen_mode_) {
          case LISTEN_MODE_S1: n = expected_raw_len_s1(head); break;
          case LISTEN_MODE_C1: n = expected_raw_len_c1(head); break;
          default:
            // `both` listens on the C-mode sync words too, so a capture here can
            // be either; the mode-C indicator in the first bytes is what tells
            // them apart, and expected_raw_len_c1() returns 0 when it is absent.
            n = expected_raw_len_c1(head);
            if (n == 0) n = expected_raw_len_t1(head);
            break;
        }
        // Never ask for a length already passed: the engine would be told the
        // packet ended before it did, and there is no recovering that.
        if (n > this->drain_len_ && n <= AUTO_LEN_CEILING) {
          this->write_expected_len_((uint16_t) n);
          expect = (uint32_t) n;
          len_resolved = true;
          this->auto_len_resolved_.fetch_add(1, std::memory_order_relaxed);
          this->auto_len_last_.store((uint32_t) n, std::memory_order_relaxed);
        } else if (this->drain_len_ >= AUTO_LEN_GIVEUP) {
          // No length from the header. Fall back to payload_length_, which is
          // what the board would have captured anyway without this path - so a
          // failed derivation is never worse than not having tried.
          if (this->payload_length_ > this->drain_len_) {
            this->write_expected_len_(this->payload_length_);
            expect = this->payload_length_;
          }
          len_resolved = true;
          this->auto_len_fallback_.fetch_add(1, std::memory_order_relaxed);
        }
      }
      irq_now = this->get_irq_status_();
      if ((irq_now & IRQ_RX_DONE) != 0) break;
    }
    // RX_DONE fires at exactly the declared length in fixed-length mode, and
    // the counter reads 0 once it has, so the tail is taken on that basis
    // rather than from a final reading that no longer exists.
    if (this->drain_ && (irq_now & IRQ_RX_DONE) != 0) {
      this->drain_up_to_(expect);
      // Only a drain that reached the declared length is a whole frame. Short
      // of that (DRAIN_CAP, or polls that fell behind the write pointer) the
      // bytes are a fragment, and a fragment handed to the decoder would read
      // as a corrupt frame rather than as a failed drain.
      if (expect != 0 && this->drain_len_ >= expect) this->drain_ready_ = (uint16_t) expect;
      // Snapshot for publication. Above 255 the post-RX_DONE read is no longer
      // a reference - the start of the frame is gone from the buffer - so the
      // drained bytes have to leave the chip to be checked at all.
      this->drain_sample_.len = this->drain_len_;
      for (uint16_t i = 0; i < this->drain_len_; i++) this->drain_sample_.raw[i] = this->drain_buf_[i];
      this->drain_sample_.seq++;
      if (this->drain_sample_queue_ != nullptr)
        xQueueOverwrite(this->drain_sample_queue_, &this->drain_sample_);
    }
    if ((irq_now & IRQ_RX_DONE) == 0) {
      this->sync_timeouts_.fetch_add(1, std::memory_order_relaxed);
      // In S1 this is the expected path for now, and it must not leave the line
      // asserted: DIO1 stays high while any unmasked IRQ is latched and the pin
      // is read on the rising edge, so an uncleared latch blocks the next
      // frame's edge. The non-probe S1 path below clears everything for exactly
      // this reason; returning from here would otherwise skip it.
      if (this->listen_mode_ == LISTEN_MODE_S1)
        this->cmd_write_(OC_CLEAR_IRQ, {(uint8_t) (IRQ_ALL >> 24), (uint8_t) (IRQ_ALL >> 16),
                                        (uint8_t) (IRQ_ALL >> 8), (uint8_t) (IRQ_ALL >> 0)});
      return false;
    }
    uint8_t st2[2]{};
    if (!this->cmd_read_(OC_GET_RXBUFFER_STATUS, {}, st2, sizeof(st2))) return false;
    payload_len = st2[0];
    start_ptr = st2[1];
  }
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

  // Self-check, and the whole reason the drain is tested first on a frame that
  // does NOT wrap: the ordinary post-RX_DONE read is then a complete and
  // correct copy of the same bytes, so the drain can be judged against it with
  // no offline reconstruction at all. Once a frame wraps this reference is
  // destroyed and the comparison stops meaning anything, which is precisely
  // why the drain has to be proven here before it is trusted there.
  if (this->drain_ && !this->rx_buffer_.empty() &&
      this->drain_len_ >= start_ptr + this->rx_buffer_.size()) {
    this->drain_frames_.fetch_add(1, std::memory_order_relaxed);
    this->drain_bytes_last_.store(this->drain_len_, std::memory_order_relaxed);
    uint16_t diff = 0, first = 0xFFFF;
    for (size_t i = 0; i < this->rx_buffer_.size(); i++) {
      if (this->drain_buf_[start_ptr + i] != this->rx_buffer_[i]) {
        if (diff == 0) first = (uint16_t) i;
        diff++;
      }
    }
    this->drain_diff_last_.store(diff, std::memory_order_relaxed);
    this->drain_first_diff_.store(first, std::memory_order_relaxed);
    if (diff == 0) this->drain_match_.fetch_add(1, std::memory_order_relaxed);
    else this->drain_mismatch_.fetch_add(1, std::memory_order_relaxed);
  }

  uint8_t verify_result = verify_requested ? 1 : 0;
  uint16_t differences = 0, first_difference = 255;
  if (frozen) {
    uint8_t second[255]{};
    this->cmd_read_(OC_READ_BUFFER8, {start_ptr, payload_len}, second, payload_len);
    uint8_t after[2]{};
    this->cmd_read_(OC_GET_RXBUFFER_STATUS, {}, after, sizeof(after));
    (void) this->get_irq_status_();
    const uint32_t status = this->last_status_.load();
    const bool stable = this->busy_timeouts_.load() == busy_before &&
        this->status_fail_.load() == fail_before && this->status_perr_.load() == perr_before &&
        ((status >> 1) & 7) == 2 && (((status >> 9) & 7) == 2 || ((status >> 9) & 7) == 3) &&
        after[0] == payload_len && after[1] == start_ptr;
    if (stable) {
      for (uint16_t i = 0; i < payload_len; ++i) {
        if (second[i] != this->rx_buffer_[i]) {
          if (differences == 0) first_difference = i;
          ++differences;
        }
      }
      verify_result = differences == 0 ? 2 : 3;
    }
  }

  // Step D: hand the decoder the drained frame instead of the buffer read.
  //
  // Past 255 bytes the post-RX_DONE read above cannot be the frame: the buffer
  // is a ring, GetRxBufferStatus reports `expect mod 256` (70 on a 326-byte
  // frame), and the start of the telegram has already been overwritten by its
  // own tail. The drained copy is the only complete one.
  //
  // Deliberately placed AFTER the self-check and the verify block, which both
  // compare against rx_buffer_ as read over SPI. Substituting earlier would
  // make drain_match_ compare the drain against itself - an instrument that
  // reports success by construction is worse than no instrument.
  //
  // Below 255 this changes nothing observable: the two are the same bytes,
  // measured 57/57 byte-for-byte on non-wrapping frames before this was
  // enabled. The substitution is unconditional on length so that the path the
  // long frames take is the path the short frames exercise every day.
  if (this->drain_ready_ != 0) {
    this->rx_buffer_.assign(this->drain_buf_, this->drain_buf_ + this->drain_ready_);
    this->rx_idx_ = 0;
    this->rx_len_ = this->rx_buffer_.size();
    this->drain_served_.fetch_add(1, std::memory_order_relaxed);
  }

  // Copy the actual FIFO before the upper pipeline trims/probes it. No extra
  // SPI transaction. Never wait for the diagnostic consumer or change RX.
  const uint32_t sample_now = millis();
  if (sample_due) {
    this->last_raw_sample_ms_ = sample_now;
    RawRxSample sample{};
    sample.captured_ms = sample_now;
    sample.irq = this->last_irq_.load();
    sample.rssi = this->last_rssi_dbm_;
    sample.verify = verify_result;
    sample.differing_bytes = differences;
    sample.first_difference = first_difference;
    // The read above takes payload_len bytes from start_ptr - payload_len is the
    // length the packet engine *declares*, so that read can never show whether
    // the engine kept writing past it. Sample the whole buffer instead
    // (UM 2.2 p.35 ReadBuffer8, p.88 RX RAM addressable outside sleep): with
    // payload_length below 255, bytes past it are the only place an answer can
    // appear. One extra 255-byte SPI read, at most once per 5 s, after RX_DONE.
    // This stays a picture of the chip's buffer, which is no longer what the
    // decoder gets once a drain has been served - compare it against lr_drain,
    // not against what was decoded.
    sample.fifo_dump = 1;
    sample.length = 255;
    sample.packet_start = start_ptr;
    sample.packet_len = payload_len;
    // cmd_read_ always returns true, so there is no status to branch on here.
    this->cmd_read_(OC_READ_BUFFER8, {0x00, 0xFF}, sample.bytes, sizeof(sample.bytes));
    // Taken after RX_DONE, i.e. after the engine finished writing. A live
    // position counter should differ from the at-rest baseline logged at boot,
    // and should differ between captures of different lengths.
    this->probe_registers_(sample.probe);
    (void) xQueueSend(this->raw_sample_queue_, &sample, 0);
  }

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
    // Observe before buffer access / the next ClearIrq. Do not change capture
    // decisions in this diagnostic patch. Repeated latches are observations.
    const uint32_t irq = this->get_irq_status_();
    this->irq_samples_.fetch_add(1, std::memory_order_relaxed);
    if (irq & IRQ_RX_DONE) this->irq_done_.fetch_add(1, std::memory_order_relaxed);
    else this->read_without_done_.fetch_add(1, std::memory_order_relaxed);
    if (irq & IRQ_TIMEOUT) this->irq_timeout_.fetch_add(1, std::memory_order_relaxed);
    if (irq & IRQ_FSK_LEN_ERROR) this->irq_len_error_.fetch_add(1, std::memory_order_relaxed);
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

bool LR1121::take_raw_rx_sample(RawRxSample &out) {
  return this->raw_sample_queue_ != nullptr && xQueueReceive(this->raw_sample_queue_, &out, 0) == pdTRUE;
}

void LR1121::observe_stat1_(uint8_t stat1) {
  this->status_samples_.fetch_add(1, std::memory_order_relaxed);
  const uint8_t command_status = (stat1 >> 1) & 7;
  if (command_status == 0) this->status_fail_.fetch_add(1, std::memory_order_relaxed);
  if (command_status == 1) this->status_perr_.fetch_add(1, std::memory_order_relaxed);
}

std::string LR1121::runtime_diag_json() {
  const uint32_t now = millis();
  if ((uint32_t) (now - this->last_runtime_report_ms_) < 60000) return {};
  this->last_runtime_report_ms_ = now;
  // Individual atomic samples, not a transactionally coherent snapshot.
  const uint32_t status = this->last_status_.load(std::memory_order_relaxed);
  const uint32_t packet = this->last_packet_status_.load(std::memory_order_relaxed);
  char out[1024];
  snprintf(out, sizeof(out),
    "{\"schema\":1,\"radio\":\"LR1121\",\"uptime_ms\":%u,"
    "\"busy_timeouts\":%u,\"status_samples\":%u,\"cmd_fail_observations\":%u,\"cmd_perr_observations\":%u,"
    "\"irq_samples\":%u,\"rx_done_observations\":%u,\"timeout_observations\":%u,\"len_error_observations\":%u,"
    "\"read_without_rx_done\":%u,\"last_irq\":%u,\"stat1\":%u,\"stat2\":%u,\"chip_mode\":%u,\"reset_status\":%u,"
    "\"packet_samples\":%u,\"packet_received_observations\":%u,\"packet_abort_observations\":%u,\"packet_length\":%u,\"packet_flags\":%u}",
    (unsigned) now, (unsigned) this->busy_timeouts_.load(), (unsigned) this->status_samples_.load(),
    (unsigned) this->status_fail_.load(), (unsigned) this->status_perr_.load(),
    (unsigned) this->irq_samples_.load(), (unsigned) this->irq_done_.load(),
    (unsigned) this->irq_timeout_.load(), (unsigned) this->irq_len_error_.load(),
    (unsigned) this->read_without_done_.load(), (unsigned) this->last_irq_.load(),
    (unsigned) (status >> 8), (unsigned) (status & 255), (unsigned) ((status >> 1) & 7),
    (unsigned) ((status >> 4) & 15), (unsigned) this->packet_samples_.load(),
    (unsigned) this->packet_received_.load(), (unsigned) this->packet_abort_.load(),
    (unsigned) (packet >> 8), (unsigned) (packet & 255));
  return out;
}

// Kept out of runtime_diag_json deliberately: appending it there pushed that
// line past the logger's buffer, so the log showed a JSON object cut off
// mid-key while MQTT carried the whole thing. A diagnostic that is silently
// truncated in one of its two outputs is worse than one that is split in two.
// Last completed drain, as hex. Main task, no SPI: it formats a buffer the
// receiver task filled. The two are not interlocked, so a snapshot taken while
// the next frame is being drained can tear - seq says which attempt it came
// from, and a torn sample shows up immediately as a correlation failure rather
// than as plausible wrong bytes.
std::string LR1121::drain_sample_json() {
  DrainSample sample;
  if (this->drain_sample_queue_ == nullptr ||
      xQueueReceive(this->drain_sample_queue_, &sample, 0) != pdTRUE) return {};
  const uint16_t len = sample.len;
  std::string out;
  out.reserve((size_t) len * 2 + 64);
  char head[64];
  snprintf(head, sizeof(head), "{\"schema\":2,\"seq\":%u,\"len\":%u,\"raw\":\"",
           (unsigned) sample.seq, (unsigned) len);
  out += head;
  static const char HEX[] = "0123456789ABCDEF";
  for (uint16_t i = 0; i < len; i++) {
    out += HEX[sample.raw[i] >> 4];
    out += HEX[sample.raw[i] & 0x0F];
  }
  out += "\",\"trace_fields\":[\"us\",\"target\",\"copied\",\"packet_len\",\"start\",\"offset\",\"size\"],\"trace\":[";
  for (uint8_t i = 0; i < sample.trace_count; i++) {
    const auto &t = sample.trace[i];
    char row[96];
    snprintf(row, sizeof(row), "%s[%u,%u,%u,%u,%u,%u,%u]", i == 0 ? "" : ",",
             (unsigned) t.us, (unsigned) t.target, (unsigned) t.copied,
             (unsigned) t.packet_len, (unsigned) t.start, (unsigned) t.offset, (unsigned) t.size);
    out += row;
  }
  snprintf(head, sizeof(head), "],\"trace_total\":%u}", (unsigned) sample.trace_total);
  out += head;
  return out;
}

std::string LR1121::sync_probe_json() {
  if (!this->sync_probe_) return {};
  char out[480];
  snprintf(out, sizeof(out),
           "{\"schema\":3,\"sync_wakes\":%u,\"sync_polls\":%u,\"sync_timeouts\":%u,\"ptr_last\":%u,\"ptr_max\":%u,"
           "\"drain_frames\":%u,\"drain_match\":%u,\"drain_mismatch\":%u,"
           "\"drain_bytes_last\":%u,\"drain_diff_last\":%u,\"drain_first_diff\":%u,"
           "\"drain_served\":%u,\"auto_len_resolved\":%u,\"auto_len_fallback\":%u,"
           "\"auto_len_last\":%u}",
           (unsigned) this->sync_wakes_.load(), (unsigned) this->sync_polls_.load(),
           (unsigned) this->sync_timeouts_.load(), (unsigned) this->sync_ptr_last_.load(),
           (unsigned) this->sync_ptr_max_.load(),
           (unsigned) this->drain_frames_.load(), (unsigned) this->drain_match_.load(),
           (unsigned) this->drain_mismatch_.load(), (unsigned) this->drain_bytes_last_.load(),
           (unsigned) this->drain_diff_last_.load(), (unsigned) this->drain_first_diff_.load(),
           (unsigned) this->drain_served_.load(), (unsigned) this->auto_len_resolved_.load(),
           (unsigned) this->auto_len_fallback_.load(), (unsigned) this->auto_len_last_.load());
  return out;
}

std::string LR1121::probe_baseline_json() {
  // Gated like sync_probe_json() below, which it should have been from the
  // start. Without a gate this published four raw undocumented register values
  // to the log and to MQTT on every LR1121 node with diagnostics on, including
  // the ones running none of the experiments those values calibrate.
  if (!this->undocumented_register_work_()) return {};
  char out[160];
  snprintf(out, sizeof(out),
           "{\"schema\":1,\"F20384\":%u,\"F20368\":%u,\"F30028\":%u,\"F30030\":%u}",
           (unsigned) this->probe_baseline_[0], (unsigned) this->probe_baseline_[1],
           (unsigned) this->probe_baseline_[2], (unsigned) this->probe_baseline_[3]);
  return out;
}

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

  if (this->expected_len_override_ != 0) {
    if (this->boot_fw_ != VERIFIED_RADIO_FW) {
      ESP_LOGE(TAG, "  lr1121_expected_len_override: %u -> IGNORED. Verified only on radio "
                    "firmware 0x%04X, this chip reports 0x%04X. Refusing to write an "
                    "undocumented register on an unverified image / ODRZUCONE, niezweryfikowany "
                    "firmware radia",
               (unsigned) this->expected_len_override_, (unsigned) VERIFIED_RADIO_FW,
               (unsigned) this->boot_fw_);
    } else {
      ESP_LOGW(TAG, "  lr1121_expected_len_override: %u -> EXPERIMENT ACTIVE(!): writing it into "
                    "undocumented register 0x%08X [31:20] before every SetRx, overriding the "
                    "configured payload_length %u. Reception will not behave normally. Bench "
                    "work, not a supported configuration / EKSPERYMENT, odbior nie bedzie "
                    "dzialal normalnie",
               (unsigned) this->expected_len_override_, (unsigned) REG_EXPECTED_LEN,
               (unsigned) this->payload_length_);
    }
  }

  if (this->auto_length_) {
    if (this->boot_fw_ != VERIFIED_RADIO_FW) {
      ESP_LOGE(TAG, "  lr1121_auto_length: IGNORED. Verified only on radio firmware 0x%04X, this "
                    "chip reports 0x%04X. Refusing to write an undocumented register on an "
                    "unverified image; reception falls back to payload_length %u / ODRZUCONE, "
                    "niezweryfikowany firmware radia",
               (unsigned) VERIFIED_RADIO_FW, (unsigned) this->boot_fw_,
               (unsigned) this->payload_length_);
    } else {
      ESP_LOGW(TAG, "  lr1121_auto_length: ON -> the frame's own L-field sets where the packet "
                    "engine stops, written into undocumented register 0x%08X [31:20] while the "
                    "frame is still arriving. Lifts the %u-byte ceiling (max is %u), and a header "
                    "that will not decode falls back to that ceiling. Uses an undocumented "
                    "register / dlugosc z pola L ramki, rejestr niezadokumentowany",
               (unsigned) REG_EXPECTED_LEN, (unsigned) this->payload_length_,
               (unsigned) AUTO_LEN_CEILING);
    }
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
