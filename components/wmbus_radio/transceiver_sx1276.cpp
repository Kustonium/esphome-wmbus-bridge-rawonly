// SPDX-License-Identifier: GPL-3.0-or-later
#include "transceiver_sx1276.h"

#include "esphome/core/log.h"
#include <esp_timer.h>
#include <algorithm>

#define F_OSC (32000000)

namespace esphome {
namespace wmbus_radio {
static const char *TAG = "SX1276";

static constexpr uint8_t REG_FIFO         = 0x00;
static constexpr uint8_t REG_OP_MODE      = 0x01;
static constexpr uint8_t REG_IRQ_FLAGS2   = 0x3F;
static constexpr uint8_t REG_RSSI_VALUE   = 0x11;
static constexpr uint8_t REG_FIFO_THRESH  = 0x35;
static constexpr uint8_t REG_DIO_MAPPING1 = 0x40;
static constexpr uint8_t REG_VERSION      = 0x42;

// Read only by dump_debug_status(); the setup sequence writes them as literals
// in datasheet order and the table above the setup() body is the key for those.
static constexpr uint8_t REG_RX_CONFIG       = 0x0D;
static constexpr uint8_t REG_RX_BW           = 0x12;
static constexpr uint8_t REG_PREAMBLE_DETECT = 0x1F;
static constexpr uint8_t REG_SYNC_CONFIG     = 0x27;
static constexpr uint8_t REG_IRQ_FLAGS1      = 0x3E;
static constexpr uint8_t REG_AFC_MSB         = 0x1B;
static constexpr uint8_t REG_FEI_MSB         = 0x1D;

// Frequency synthesiser step, FXOSC / 2^19 = 61.03515625 Hz. Both RegAfc and
// RegFei count in these steps, as signed 16-bit values.
//
// Read in millihertz-free integer arithmetic: value * 61035 / 1000 gives Hz
// with an error under 0.01 % over the whole 16-bit range, which is far below
// anything worth reporting here.
static int32_t sx1276_steps_to_hz_(uint8_t msb, uint8_t lsb) {
  const int32_t raw = (int32_t) (int16_t) (((uint16_t) msb << 8) | (uint16_t) lsb);
  return (int32_t) ((int64_t) raw * 61035 / 1000);
}

// RegIrqFlags1 bits, FSK/OOK mode. Only the two that answer "is anything
// arriving at all" are named; the rest are printed as hex.
//
// ModeReady and RxReady are deliberately not named and not tested. On this chip
// in FSK they read clear on a receiver that is demonstrably working - see the
// note in dump_debug_status().
static constexpr uint8_t FLAG1_PREAMBLE_DETECT = (1 << 1);
static constexpr uint8_t FLAG1_SYNC_MATCH      = (1 << 0);

// RegOpMode bits 2:0 in FSK/OOK mode. FSRX is the trap worth naming: the
// synthesiser is locked to the receive frequency but the receiver itself is
// off, so the chip looks configured and hears nothing.
static const char *sx1276_mode_name_(uint8_t op_mode) {
  switch (op_mode & 0x07) {
    case 0x00: return "SLEEP";
    case 0x01: return "STDBY";
    case 0x02: return "FSTX";
    case 0x03: return "TX";
    case 0x04: return "FSRX";
    case 0x05: return "RX";
    default: return "?";
  }
}

static constexpr uint8_t FLAG2_FIFO_EMPTY   = (1 << 6);
static constexpr uint8_t FLAG2_FIFO_LEVEL   = (1 << 5);
static constexpr uint8_t FLAG2_FIFO_OVERRUN = (1 << 4);

void SX1276::spi_read_burst_(uint8_t address, uint8_t *dst, size_t len) {
  this->delegate_->begin_transaction();
  this->delegate_->transfer(address & 0x7F);
  for (size_t i = 0; i < len; i++) {
    dst[i] = this->delegate_->transfer(0x00);
  }
  this->delegate_->end_transaction();
}

// ---------------------------------------------------------------------------
// latch_frame_metrics_: sample everything that describes THIS frame, once, at
// the moment its first bytes reach the FIFO.
//
// RSSI has to be read here because after the transmission it measures the empty
// channel. RegAfc and RegFei have the same problem for the opposite reason:
// they are latched during the preamble/AFC phase, so by the time the first FIFO
// byte arrives they already describe this frame - but they keep being
// overwritten by every later preamble detection, including ones caused by noise.
//
// Reading them anywhere else produces numbers that look authoritative and
// describe nothing. Measured 2026-08-01: a dump taken on a receive-wait timeout
// reported +23.5 kHz two seconds before a frame, -17.6 kHz corrected with a
// +68.5 kHz residual one minute later on a preamble that never matched sync,
// and -16.1 kHz three seconds before the next frame - none of them from the
// transmitter that was actually being decoded every 123 seconds.
// ---------------------------------------------------------------------------
void SX1276::latch_frame_metrics_() {
  this->last_rssi_dbm_ = (int8_t) (-(int) this->spi_read(REG_RSSI_VALUE) / 2);
  // Sticky copy: restart_rx() resets last_rssi_dbm_ to the not-measured
  // sentinel on every re-arm, which is correct for the value handed to the
  // packet but would leave the diagnostic reporting -127 for a frame it did
  // measure.
  this->last_frame_rssi_dbm_ = this->last_rssi_dbm_;
  this->last_afc_msb_ = this->spi_read(REG_AFC_MSB);
  this->last_afc_lsb_ = this->spi_read(REG_AFC_MSB + 1);
  this->last_fei_msb_ = this->spi_read(REG_FEI_MSB);
  this->last_fei_lsb_ = this->spi_read(REG_FEI_MSB + 1);
  this->frame_metrics_us_ = esp_timer_get_time();
  this->rssi_captured_ = true;
  // One flag assignment, deliberately no logging here. This function runs
  // inside the FIFO drain, and at 100 kchip/s the 64-byte FIFO refills in
  // ~7.7 ms - less than a network log line can take. The report is emitted by
  // the component after the frame is complete; see take_frame_freq_error().
  this->freq_error_fresh_ = true;
}

// take_frame_freq_error: hand the latched offset to the component, once.
//
// Only this chip can answer. The SX126x and LR1121 have no AFC in GFSK at all -
// there is no register for it in either map - so on those radios the base
// implementation returns false and nothing is logged.
bool SX1276::take_frame_freq_error(int32_t *afc_hz, int32_t *fei_hz) {
  if (!this->freq_error_fresh_)
    return false;
  this->freq_error_fresh_ = false;
  if (afc_hz != nullptr)
    *afc_hz = sx1276_steps_to_hz_(this->last_afc_msb_, this->last_afc_lsb_);
  if (fei_hz != nullptr)
    *fei_hz = sx1276_steps_to_hz_(this->last_fei_msb_, this->last_fei_lsb_);
  return true;
}

optional<uint8_t> SX1276::drain_fifo_once_() {
  const uint8_t irq2 = this->spi_read(REG_IRQ_FLAGS2);

  if (irq2 & FLAG2_FIFO_OVERRUN) {
    this->spi_write(REG_IRQ_FLAGS2, (uint8_t) FLAG2_FIFO_OVERRUN);
    this->chunk_len_ = 0;
    this->chunk_idx_ = 0;
    this->frame_active_ = false;
    this->rssi_captured_ = false;
    this->last_rssi_dbm_ = -127;
    this->abort_requested_ = true;
    this->fifo_overrun_count_++;
    ESP_LOGW(TAG, "FIFO overrun / przepelnienie FIFO");
    return {};
  }

  // Safe burst path: FifoLevel guarantees >= SX1276_CHUNK_SIZE bytes in FIFO.
  if (irq2 & FLAG2_FIFO_LEVEL) {
    if (!this->rssi_captured_)
      this->latch_frame_metrics_();

    this->spi_read_burst_(REG_FIFO, this->chunk_buffer_.data(), SX1276_CHUNK_SIZE);
    this->chunk_len_ = SX1276_CHUNK_SIZE;
    this->chunk_idx_ = 0;
    this->frame_active_ = true;
    return this->chunk_buffer_[this->chunk_idx_++];
  }

  // Tail path: less than threshold left, so only a single-byte read is safe.
  if (!(irq2 & FLAG2_FIFO_EMPTY)) {
    if (!this->rssi_captured_)
      this->latch_frame_metrics_();

    this->frame_active_ = true;
    return this->spi_read(REG_FIFO);
  }

  return {};
}

// ---------------------------------------------------------------------------
// Register map used by setup() below. Addresses are written as raw literals to
// keep the configuration sequence readable in datasheet order; this table is the
// key. Source: Semtech SX1276/77/78/79 datasheet, FSK/OOK mode register table
// (the LoRa-mode table maps the same addresses to different registers, so read
// it in the FSK column).
//
//   0x02,0x03  RegBitrateMsb/Lsb     bit rate (100 kbps T1/C1, 32.768 kbps S1)
//   0x04,0x05  RegFdevMsb/Lsb        frequency deviation (50 kHz T1, 45 kHz C1)
//   0x06..0x08 RegFrfMsb/Mid/Lsb     carrier frequency, step = F_OSC / 2^19
//   0x0C       RegLna                LNA gain + high-frequency LNA boost
//   0x0D       RegRxConfig           AFC/AGC auto-on + RX trigger source
//   0x0E       RegRssiConfig         RSSI smoothing
//   0x12,0x13  RegRxBw / RegAfcBw    receiver and AFC bandwidth
//   0x1F       RegPreambleDetect     detector on, size, tolerance
//   0x24       RegOsc                CLKOUT divider (0b111 = CLKOUT off)
//   0x25,0x26  RegPreambleMsb/Lsb    preamble length
//   0x27       RegSyncConfig         sync on/off, fill condition, sync size
//   0x28..0x2F RegSyncValue1..8      sync word bytes (0x54 0x3D T1, 0x54 0xCD C1)
//   0x30       RegPacketConfig1      packet format, CRC, whitening (0 = raw)
//   0x32       RegPayloadLength      0 with fixed-length = unlimited packet mode
//   0x35       RegFifoThresh         FIFO level threshold driving DIO1
//   0x3F       RegIrqFlags2          FIFO empty/level/overrun status bits
//   0x40       RegDioMapping1        DIO1 function select
//   0x42       RegVersion            silicon revision (0x11..0x13 expected)
//   0x5D       RegBitrateFrac        fractional part of the bit rate divider
//
// The named constants above (REG_FIFO, REG_OP_MODE, ...) cover the registers
// touched outside setup(); everything else is listed here rather than duplicated.
// ---------------------------------------------------------------------------
void SX1276::setup() {
  // Original driver used DIO1=FifoEmpty with falling edge.
  // Here DIO1 is remapped to FifoLevel, which is active-high, so wake on rising edge.
  this->irq_edge_ = gpio::INTERRUPT_RISING_EDGE;

  if (this->tcxo_pin_ != nullptr) {
    this->tcxo_pin_->setup();
    this->tcxo_pin_->digital_write(true);
    delay(10);
    ESP_LOGI(TAG, "TCXO enable pin set HIGH before radio init / pin TCXO ustawiony HIGH przed inicjalizacja radia");
  }

  this->common_setup();
  ESP_LOGV(TAG, "Setup");
  {
    const char *lm = (this->listen_mode_ == LISTEN_MODE_T1) ? "T1 only"
                   : (this->listen_mode_ == LISTEN_MODE_C1) ? "C1 only"
                   : (this->listen_mode_ == LISTEN_MODE_S1) ? "S1 only"
                   : "T1+C1 (both, 3:1 bias)";
    ESP_LOGI(TAG, "Listen mode / tryb nasluchu: %s", lm);
  }
  this->reset();

  const uint8_t revision = this->spi_read(0x42);
  if (revision < 0x11 || revision > 0x13) {
    ESP_LOGE(TAG, "Invalid silicon revision / nieprawidlowa rewizja ukladu: %02X", revision);
    return;
  }

  const uint32_t frf = ((uint64_t) this->configured_frequency_hz_ * (1 << 19)) / F_OSC;
  this->spi_write(0x06, {BYTE(frf, 2), BYTE(frf, 1), BYTE(frf, 0)});

  // RegRxBw / RegAfcBw:
  // 0x02 = ~125 kHz (T1/default), 0x09 = ~200 kHz (C1), 0x01 = ~250 kHz AFC BW.
  const uint8_t rxbw_val = (this->listen_mode_ == LISTEN_MODE_C1) ? (uint8_t) 0x09 : (uint8_t) 0x02;
  const uint8_t afcbw_val = (this->listen_mode_ == LISTEN_MODE_C1) ? (uint8_t) 0x01 : (uint8_t) 0x02;
  this->spi_write(0x12, {rxbw_val, afcbw_val});

  const uint16_t freq_dev = (this->listen_mode_ == LISTEN_MODE_C1) ? 45000 : 50000;
  {
    char buf[112];
    snprintf(buf, sizeof(buf), "freq=%.3fMHz fdev=%ukHz RxBW=%s AfcBW=%s",
             this->configured_frequency_hz_ / 1000000.0f,
             (unsigned) (freq_dev / 1000),
             (this->listen_mode_ == LISTEN_MODE_C1) ? "200kHz" : "125kHz",
             (this->listen_mode_ == LISTEN_MODE_C1) ? "250kHz" : "125kHz");
    this->rf_params_str_ = buf;
  }
  const uint16_t frd = ((uint64_t) freq_dev * (1 << 19)) / F_OSC;
  this->spi_write(0x04, {BYTE(frd, 1), BYTE(frd, 0)});

  const uint32_t bitrate = (this->listen_mode_ == LISTEN_MODE_S1) ? 32768UL : 100000UL;
  uint32_t br = (F_OSC << 4) / bitrate;
  this->spi_write(0x5D, (uint8_t) (br & 0x0F));
  br >>= 4;
  this->spi_write(0x02, {BYTE(br, 1), BYTE(br, 0)});

  const uint16_t preamble_length = 32 / 8;
  this->spi_write(0x25, {BYTE(preamble_length, 1), BYTE(preamble_length, 0)});

  // RegPreambleDetect: bit7 on/off, bits 6:5 size in bytes, bits 4:0 tolerance.
  // Size was pinned at 2 bytes (16 bits) until 2026-09-01; it is now the
  // min_preamble_bits option, shared with SX1262 and LR1121.
  {
    const uint8_t tol = this->preamble_tolerance_;
    uint8_t v = 0;
    if (this->min_preamble_bits_ == 0) {
      v = tol;  // detector off
    } else {
      const uint8_t size = (uint8_t) ((this->min_preamble_bits_ / 8) - 1);  // 8->0, 16->1, 24->2
      v = (uint8_t) ((1 << 7) | (size << 5) | tol);
    }
    this->spi_write(0x1F, v);
  }

  // RegLna: maximum gain (G1) with the high-frequency LNA boost on.
  //
  // 0x23 is Semtech's own value - it is what LoRaMac-node writes into RegLna in
  // RADIO_INIT_REGISTERS_VALUE for every FSK board. This driver never wrote the
  // register at all, so it ran on the reset default 0x20: same G1 gain, but
  // LnaBoostHf = 00, boost off. Confirmed on hardware 2026-08-01, register dump
  // of a LilyGO T3-S3 read back 0x0C = 0x20.
  //
  // The gain field is the less interesting half: AgcAutoOn is set on the next
  // line, so the AGC drives LnaGain itself and whatever is written here is
  // overridden as soon as a signal arrives. LnaBoostHf is the part that stays -
  // it raises the LNA current by 50% for a better noise figure, applies only
  // above 525 MHz, and is ignored at lower frequencies rather than harmful.
  this->spi_write(0x0C, (uint8_t) 0x23);

  this->spi_write(0x0D, (uint8_t) ((1 << 4) | (1 << 3) | 0b110));
  this->spi_write(0x24, (uint8_t) 0b111);

  const uint8_t sync_len = (this->listen_mode_ == LISTEN_MODE_S1) ? 3 : 2;
  const uint8_t sync_cfg = (1 << 5) | (1 << 4) | (sync_len - 1);
  if (this->listen_mode_ == LISTEN_MODE_S1)
    this->spi_write(0x27, {sync_cfg, 0x54, 0x76, 0x96});
  else
    this->spi_write(0x27, {sync_cfg, 0x54, 0x3D});

  this->spi_write(0x30, (uint8_t) 0);  // no hardware CRC
  this->spi_write(0x32, (uint8_t) 0);  // unlimited packet mode

  // Threshold = CHUNK_SIZE - 1, so FifoLevel means FIFO has at least CHUNK_SIZE bytes.
  this->spi_write(REG_FIFO_THRESH, (uint8_t) (SX1276_CHUNK_SIZE - 1));

  // DIO1 = FifoLevel in FSK mode.
  // bits[5:4] = 00 -> FifoLevel
  this->spi_write(REG_DIO_MAPPING1, (uint8_t) (0b00 << 4));

  this->spi_write(0x0E, (uint8_t) 0b111);  // RSSI smoothing

  this->chunk_len_ = 0;
  this->chunk_idx_ = 0;
  this->frame_active_ = false;

  ESP_LOGV(TAG, "SX1276 setup done (burst + tail-gap bridge)");
}

void SX1276::log_reg_status() {
  const uint8_t reg_version    = this->spi_read(REG_VERSION);
  const uint8_t reg_op_mode    = this->spi_read(REG_OP_MODE);
  const uint8_t reg_irq2       = this->spi_read(REG_IRQ_FLAGS2);
  const uint8_t reg_rssi       = this->spi_read(REG_RSSI_VALUE);
  const uint8_t reg_dio        = this->spi_read(REG_DIO_MAPPING1);
  const uint8_t reg_fifo_thresh = this->spi_read(REG_FIFO_THRESH);

  ESP_LOGI(TAG, "RegVersion=0x%02X RegOpMode=0x%02X RegIrqFlags2=0x%02X RegRssiValue=0x%02X RegDioMapping1=0x%02X RegFifoThresh=0x%02X",
           reg_version, reg_op_mode, reg_irq2, reg_rssi, reg_dio, reg_fifo_thresh);

  if (reg_version == 0x00 && reg_op_mode == 0x00 && reg_irq2 == 0x00 &&
      reg_rssi == 0x00 && reg_dio == 0x00 && reg_fifo_thresh == 0x00) {
    ESP_LOGE(TAG, "SX1276 not responding over SPI / SX1276 nie odpowiada po SPI. "
                  "Check VCC/GND/SCK/MOSI/MISO/NSS/RESET.");
  }

  if (this->diag_verbose_)
    this->dump_register_bank_();
}

// ---------------------------------------------------------------------------
// dump_register_bank_: the whole FSK register bank, once, as hex.
//
// This exists because reasoning about which registers differ between two
// listen modes turned out to be a poor substitute for reading them. The setup
// sequence is a hundred lines of literals; deriving from it that "only the bit
// rate and the sync length can differ" is an argument, and arguments have been
// wrong repeatedly here. Dumping the bank in one mode and in the other and
// diffing the two logs is not an argument.
//
// 0x00 is deliberately skipped: it is RegFifo, and reading it pops a byte off
// the receive FIFO. Printed as "--" so the column alignment still matches the
// datasheet's address grid.
//
// Runs from log_reg_status(), i.e. from the main task at boot, under verbose
// diagnostics only. Five lines per boot.
// ---------------------------------------------------------------------------
void SX1276::dump_register_bank_() {
  ESP_LOGI(TAG, "Register bank / bank rejestrow (FSK, 0x00-0x4F; 0x00 = RegFifo, not read):");
  for (uint8_t base = 0x00; base < 0x50; base = (uint8_t) (base + 0x10)) {
    char line[80];
    int pos = snprintf(line, sizeof(line), "  %02X:", base);
    for (uint8_t off = 0; off < 0x10; off++) {
      const uint8_t addr = (uint8_t) (base + off);
      if (addr == REG_FIFO) {
        pos += snprintf(line + pos, sizeof(line) - pos, " --");
        continue;
      }
      pos += snprintf(line + pos, sizeof(line) - pos, " %02X", this->spi_read(addr));
    }
    ESP_LOGI(TAG, "%s", line);
  }
  ESP_LOGI(TAG, "  5D: %02X  (RegBitrateFrac)", this->spi_read(0x5D));
}

// ---------------------------------------------------------------------------
// dump_debug_status: re-read the receive chain while it is running.
//
// log_reg_status() runs once at boot, which is enough to prove the chip answers
// over SPI and nothing else. When a node stops receiving, the questions are all
// about the current state - is the radio in RX or did it stop at FSRX, is the
// preamble detector seeing anything, is the sync word still the one that was
// programmed - and a boot-time snapshot cannot answer any of them.
//
// Called from Radio::receive_frame() on an interrupt timeout when diagnostics
// are verbose, so this prints roughly once a minute on a silent node and never
// on a busy one.
// ---------------------------------------------------------------------------
void SX1276::dump_debug_status(const char *reason) {
  const uint8_t op_mode  = this->spi_read(REG_OP_MODE);
  const uint8_t irq1     = this->spi_read(REG_IRQ_FLAGS1);
  const uint8_t irq2     = this->spi_read(REG_IRQ_FLAGS2);
  const uint8_t rssi     = this->spi_read(REG_RSSI_VALUE);
  const uint8_t rx_cfg   = this->spi_read(REG_RX_CONFIG);
  const uint8_t rx_bw    = this->spi_read(REG_RX_BW);
  const uint8_t pre_det  = this->spi_read(REG_PREAMBLE_DETECT);
  const uint8_t sync_cfg = this->spi_read(REG_SYNC_CONFIG);
  const uint8_t sync1    = this->spi_read(0x28);
  const uint8_t sync2    = this->spi_read(0x29);
  const uint8_t sync3    = this->spi_read(0x2A);

  const int irq_level = (this->irq_pin_ != nullptr) ? (int) this->irq_pin_->digital_read() : -1;

  ESP_LOGI(TAG,
           "DEBUG [%s]: OpMode=0x%02X (%s) IrqFlags1=0x%02X IrqFlags2=0x%02X RssiValue=0x%02X (%ddBm) "
           "RxConfig=0x%02X RxBw=0x%02X PreambleDetect=0x%02X SyncConfig=0x%02X Sync=%02X%02X%02X DIO1=%d",
           reason != nullptr ? reason : "?", op_mode, sx1276_mode_name_(op_mode), irq1, irq2, rssi,
           -((int) rssi) / 2, rx_cfg, rx_bw, pre_det, sync_cfg, sync1, sync2, sync3, irq_level);

  // Frequency error, reported from the values latched by latch_frame_metrics_()
  // when the last frame's first bytes arrived - deliberately NOT re-read here.
  //
  // RegFei is what the receiver measured; RegAfc is what the AFC corrected by.
  // With AfcAutoOn set, that correction is work an SX126x cannot do at all: it
  // has no AFC in GFSK, so whatever offset shows up here is error the other
  // chip has to swallow whole.
  //
  // The age matters as much as the value. This dump runs on a receive-wait
  // timeout, i.e. precisely when nothing has arrived for a minute, so a live
  // read of these registers returns whatever noise last tripped the preamble
  // detector. The age says how long ago the reading came from a real frame.
  if (this->frame_metrics_us_ != 0) {
    const long age_s = (long) ((esp_timer_get_time() - this->frame_metrics_us_) / 1000000LL);
    ESP_LOGI(TAG,
             "DEBUG [%s]: last frame %lds ago: RegAfc=%02X%02X (%ld Hz) RegFei=%02X%02X (%ld Hz) "
             "RSSI=%ddBm / blad czestotliwosci ostatniej ramki",
             reason != nullptr ? reason : "?", age_s,
             this->last_afc_msb_, this->last_afc_lsb_,
             (long) sx1276_steps_to_hz_(this->last_afc_msb_, this->last_afc_lsb_),
             this->last_fei_msb_, this->last_fei_lsb_,
             (long) sx1276_steps_to_hz_(this->last_fei_msb_, this->last_fei_lsb_),
             (int) this->last_frame_rssi_dbm_);
  } else {
    ESP_LOGI(TAG, "DEBUG [%s]: no frame received since boot, no frequency error to report",
             reason != nullptr ? reason : "?");
  }

  // The two bits that say whether anything is arriving, restated in words -
  // they are one bit each in the middle of a hex byte.
  //
  // OpMode is printed above but deliberately not turned into a claim about
  // whether the receiver is running. On the SX127x in FSK, RegOpMode reads back
  // 0b100 (FSRX) after a successful write of 0b101 (RX), and IrqFlags1 reports
  // ModeReady and RxReady clear at the same time. RadioLib works around exactly
  // this - SX127x::setMode() masks the low mode bit out of its write
  // verification for FSK RX, "as it sometimes seem to fail (#276)".
  //
  // Measured here 2026-08-01: a node reading OpMode=0x04 with ModeReady=no and
  // RxReady=no decoded three T1 frames at -75, -91 and -95 dBm in the same
  // second. An earlier version of this function called that state "not in RX -
  // nothing can be received", which was false and was believed for hours.
  ESP_LOGI(TAG, "DEBUG [%s]: preamble_detected=%s sync_matched=%s (OpMode/ModeReady unreliable in FSK RX)",
           reason != nullptr ? reason : "?",
           (irq1 & FLAG1_PREAMBLE_DETECT) ? "yes" : "no",
           (irq1 & FLAG1_SYNC_MATCH) ? "yes" : "no");
}

optional<uint8_t> SX1276::read() {
  // First serve already buffered burst bytes from RAM.
  if (this->chunk_idx_ < this->chunk_len_) {
    return this->chunk_buffer_[this->chunk_idx_++];
  }

  // Normal fast path.
  if (auto byte = this->drain_fifo_once_(); byte.has_value()) {
    return byte;
  }

  // Critical fix versus naive FifoLevel-only design:
  // when draining the tail of a frame, FIFO can become temporarily empty before
  // the next tail byte arrives. That does NOT necessarily mean EOF, and because
  // DIO1 now signals FifoLevel, there may be no new IRQ for the remaining <16 B.
  // So after a frame has started, briefly poll for more bytes before returning {}.
  if (this->frame_active_) {
    const int64_t deadline = esp_timer_get_time() + SX1276_TAIL_GAP_US;
    while (esp_timer_get_time() < deadline) {
      if (auto byte = this->drain_fifo_once_(); byte.has_value()) {
        return byte;
      }
    }

    // No more bytes within the short intra-frame grace period -> frame ended.
    this->frame_active_ = false;
  }

  return {};
}

void SX1276::restart_rx() {
  if (this->listen_mode_ == LISTEN_MODE_S1) {
    this->spi_write(REG_OP_MODE, (uint8_t) 0b001);  // standby
    this->spi_write(0x28, {0x54, 0x76, 0x96});
    this->spi_write(REG_IRQ_FLAGS2, (uint8_t) FLAG2_FIFO_OVERRUN);
    this->chunk_len_ = 0;
    this->chunk_idx_ = 0;
    this->frame_active_ = false;
    this->rssi_captured_ = false;
    this->last_rssi_dbm_ = -127;
    this->abort_requested_ = false;
    this->spi_write(REG_OP_MODE, (uint8_t) 0b101);  // RX
    return;
  }

  uint8_t sync2;
  if (this->listen_mode_ == LISTEN_MODE_T1) {
    sync2 = 0x3D;
  } else if (this->listen_mode_ == LISTEN_MODE_C1) {
    // C1 exists with both second sync-byte variants (0x3D / 0xCD).
    // Bias 3:1 towards 0x3D, same as LISTEN_MODE_BOTH, so C1-only
    // does not accidentally exclude the more common variant.
    sync2 = (this->sync_cycle_ == 3) ? 0xCD : 0x3D;
    this->sync_cycle_ = (uint8_t) ((this->sync_cycle_ + 1) & 0x03);
  } else {
    sync2 = (this->sync_cycle_ == 3) ? 0xCD : 0x3D;
    this->sync_cycle_ = (uint8_t) ((this->sync_cycle_ + 1) & 0x03);
  }

  this->spi_write(REG_OP_MODE, (uint8_t) 0b001);  // standby
  this->spi_write(0x28, {0x54, sync2});

  // Clear FIFO overrun flag.
  this->spi_write(REG_IRQ_FLAGS2, (uint8_t) FLAG2_FIFO_OVERRUN);

  this->chunk_len_ = 0;
  this->chunk_idx_ = 0;
  this->frame_active_ = false;
  this->rssi_captured_ = false;
  this->last_rssi_dbm_ = -127;
  this->abort_requested_ = false;

  this->spi_write(REG_OP_MODE, (uint8_t) 0b101);  // RX
}

int8_t SX1276::get_rssi() {
  return this->last_rssi_dbm_;
}

// RegRssiValue is the live receiver reading; in FSK mode the register holds
// -2x the dBm value. Harmless to read while RX is running - it is a plain
// register fetch, not a mode change.
bool SX1276::read_channel_rssi_dbm(int8_t *out) {
  const int rssi = -(int) this->spi_read(REG_RSSI_VALUE) / 2;
  if (rssi >= 0 || rssi < -127)
    return false;
  if (out != nullptr)
    *out = (int8_t) rssi;
  return true;
}

bool SX1276::consume_rx_abort_request() {
  const bool abort = this->abort_requested_;
  this->abort_requested_ = false;
  return abort;
}

uint32_t SX1276::take_fifo_overrun_count() {
  const uint32_t count = this->fifo_overrun_count_;
  this->fifo_overrun_count_ = 0;
  return count;
}

const char *SX1276::get_name() { return TAG; }

}  // namespace wmbus_radio
}  // namespace esphome
