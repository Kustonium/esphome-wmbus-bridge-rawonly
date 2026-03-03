#include "transceiver_sx1262.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace wmbus_radio {

static const char *TAG = "SX1262";

// SX126x commands (subset)
static constexpr uint8_t CMD_SET_STANDBY = 0x80;
static constexpr uint8_t CMD_SET_PACKET_TYPE = 0x8A;
static constexpr uint8_t CMD_SET_RF_FREQUENCY = 0x86;
static constexpr uint8_t CMD_SET_BUFFER_BASE_ADDRESS = 0x8F;
static constexpr uint8_t CMD_SET_MODULATION_PARAMS = 0x8B;
static constexpr uint8_t CMD_SET_PACKET_PARAMS = 0x8C;
static constexpr uint8_t CMD_SET_DIO_IRQ_PARAMS = 0x08;
static constexpr uint8_t CMD_SET_RX = 0x82;
static constexpr uint8_t CMD_GET_IRQ_STATUS = 0x12;
static constexpr uint8_t CMD_CLEAR_IRQ_STATUS = 0x02;
static constexpr uint8_t CMD_GET_RX_BUFFER_STATUS = 0x13;
static constexpr uint8_t CMD_READ_BUFFER = 0x1E;
static constexpr uint8_t CMD_GET_PACKET_STATUS = 0x14;
static constexpr uint8_t CMD_GET_RSSI_INST = 0x15;
static constexpr uint8_t CMD_GET_STATS = 0x10;
static constexpr uint8_t CMD_GET_DEVICE_ERRORS = 0x17;
static constexpr uint8_t CMD_CLEAR_DEVICE_ERRORS = 0x07;
static constexpr uint8_t CMD_SET_DIO2_AS_RF_SWITCH_CTRL = 0x9D;
static constexpr uint8_t CMD_SET_DIO3_AS_TCXO_CTRL = 0x97;
static constexpr uint8_t CMD_CALIBRATE_IMAGE = 0x98;
static constexpr uint8_t CMD_WRITE_REGISTER = 0x0D;
static constexpr uint8_t CMD_READ_REGISTER = 0x1D;

// SX126x constants (subset)
static constexpr uint8_t STANDBY_RC = 0x00;
static constexpr uint8_t STANDBY_XOSC = 0x01;

static constexpr uint8_t PACKET_TYPE_GFSK = 0x00;

// GFSK settings
static constexpr uint8_t GFSK_PULSE_SHAPE_BT_0_5 = 0x09;
static constexpr uint8_t GFSK_RX_BW_312_0 = 0x19;
static constexpr uint8_t GFSK_RX_BW_234_3 = 0x0A;// legacy
static constexpr uint8_t GFSK_PREAMBLE_DETECT_16 = 0x05;
static constexpr uint8_t GFSK_ADDRESS_FILT_OFF = 0x00;

// SX126x GFSK header type:
// 0x00 = FIX_LEN (no length byte in-air)
// 0x01 = VAR_LEN (first payload byte is length in-air)
// WMBus does NOT carry a "GFSK length byte" at the start of the payload, so we must use FIX_LEN.
static constexpr uint8_t GFSK_PACKET_FIX_LEN = 0x00;
static constexpr uint8_t GFSK_PACKET_VAR_LEN = 0x01;

static constexpr uint8_t GFSK_CRC_OFF = 0x01;
static constexpr uint8_t GFSK_WHITENING_OFF = 0x00;

// DIO3 TCXO voltage
static constexpr uint8_t DIO3_OUTPUT_3_0 = 0x06;

// IRQ mask bits
static constexpr uint16_t IRQ_RX_DONE = 0x0002;
static constexpr uint16_t IRQ_SYNC_WORD_VALID = 0x0008;  // SyncWordValid
static constexpr uint16_t IRQ_TIMEOUT = 0x0200;   // RxTxTimeout
static constexpr uint16_t IRQ_CRC_ERROR = 0x0040; // CRC error

// Sync word base register
static constexpr uint16_t REG_SYNC_WORD_0 = 0x06C0;

// Rx gain register (datasheet SX1261/2 Rev 2.2)
static constexpr uint16_t REG_RX_GAIN = 0x08AC;
static constexpr uint8_t RX_GAIN_POWER_SAVING = 0x94;
static constexpr uint8_t RX_GAIN_BOOSTED = 0x96;

// Semtech AN1200.53 long packet reception registers
static constexpr uint16_t REG_RX_ADDR_PTR = 0x0803;
static constexpr uint16_t REG_RXTX_PAYLOAD_LEN = 0x06BB;

// RF frequency step for SX126x: 32e6 / 2^25 (Hz)
static constexpr uint32_t XTAL_FREQ = 32000000UL;

static inline void u16_to_be(uint16_t v, uint8_t &msb, uint8_t &lsb) {
  msb = (uint8_t)((v >> 8) & 0xFF);
  lsb = (uint8_t)(v & 0xFF);
}

void SX1262::wait_while_busy_() {
  if (this->busy_pin_ == nullptr)
    return;
  const uint32_t start = millis();
  while (this->busy_pin_->digital_read()) {
    if ((millis() - start) > 200) {
      ESP_LOGW(TAG, "BUSY stuck high (>200ms)");
      break;
    }
    delay(1);
  }
}

void SX1262::cmd_write_(uint8_t cmd, std::initializer_list<uint8_t> args) {
  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(cmd);
  for (auto b : args)
    this->delegate_->transfer(b);
  this->delegate_->end_transaction();
  this->wait_while_busy_();
}

void SX1262::cmd_read_(uint8_t cmd, std::initializer_list<uint8_t> args, uint8_t *out, size_t out_len) {
  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(cmd);
  for (auto b : args)
    this->delegate_->transfer(b);
  (void) this->delegate_->transfer(0x00);  // status
  for (size_t i = 0; i < out_len; i++)
    out[i] = this->delegate_->transfer(0x00);
  this->delegate_->end_transaction();
  this->wait_while_busy_();
}

void SX1262::write_register_(uint16_t addr, std::initializer_list<uint8_t> data) {
  uint8_t msb, lsb;
  u16_to_be(addr, msb, lsb);

  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(CMD_WRITE_REGISTER);
  this->delegate_->transfer(msb);
  this->delegate_->transfer(lsb);
  for (auto b : data)
    this->delegate_->transfer(b);
  this->delegate_->end_transaction();
  this->wait_while_busy_();
}


uint8_t SX1262::read_register8_(uint16_t addr) {
  uint8_t msb, lsb;
  u16_to_be(addr, msb, lsb);

  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(CMD_READ_REGISTER);
  this->delegate_->transfer(msb);
  this->delegate_->transfer(lsb);
  this->delegate_->transfer(0x00);  // dummy
  uint8_t v = this->delegate_->transfer(0x00);
  this->delegate_->end_transaction();
  this->wait_while_busy_();
  return v;
}



uint16_t SX1262::get_irq_status_() {
  uint8_t st[2]{};
  this->cmd_read_(CMD_GET_IRQ_STATUS, {0x00}, st, sizeof(st));
  return (uint16_t(st[0]) << 8) | uint16_t(st[1]);
}


void SX1262::read_buffer_(uint8_t offset, uint8_t *out, size_t out_len) {
  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(CMD_READ_BUFFER);
  this->delegate_->transfer(offset);
  (void) this->delegate_->transfer(0x00);  // status
  for (size_t i = 0; i < out_len; i++)
    out[i] = this->delegate_->transfer(0x00);
  this->delegate_->end_transaction();
  this->wait_while_busy_();
}

void SX1262::set_rf_frequency_(uint32_t freq_hz) {
  uint64_t rf = ((uint64_t) freq_hz << 25) / XTAL_FREQ;
  cmd_write_(CMD_SET_RF_FREQUENCY,
             {(uint8_t) ((rf >> 24) & 0xFF), (uint8_t) ((rf >> 16) & 0xFF), (uint8_t) ((rf >> 8) & 0xFF),
              (uint8_t) (rf & 0xFF)});
}

void SX1262::set_sync_word_(uint8_t sync2) {
  this->write_register_(REG_SYNC_WORD_0, {0x54, sync2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
}

bool SX1262::has_rx_done_() {
  uint8_t irq[2]{};
  this->cmd_read_(CMD_GET_IRQ_STATUS, {0x00}, irq, sizeof(irq));
  const uint16_t flags = ((uint16_t) irq[0] << 8) | irq[1];
  return (flags & IRQ_RX_DONE) != 0;
}

bool SX1262::load_rx_buffer_() {
  if (!this->has_rx_done_())
    return false;

  uint8_t st[2]{};
  this->cmd_read_(CMD_GET_RX_BUFFER_STATUS, {0x00}, st, sizeof(st));
  const uint8_t payload_len = st[0];
  const uint8_t start_ptr = st[1];

  if (payload_len == 0) {
    this->cmd_write_(CMD_CLEAR_IRQ_STATUS, {0xFF, 0xFF});
    return false;
  }

  this->rx_buffer_.assign(payload_len, 0);

  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(CMD_READ_BUFFER);
  this->delegate_->transfer(start_ptr);
  (void) this->delegate_->transfer(0x00);
  for (size_t i = 0; i < this->rx_buffer_.size(); i++)
    this->rx_buffer_[i] = this->delegate_->transfer(0x00);
  this->delegate_->end_transaction();
  this->wait_while_busy_();

  // Cache RSSI while the packet context is still valid.
  {
    uint8_t ps[3]{};
    this->cmd_read_(CMD_GET_PACKET_STATUS, {0x00}, ps, sizeof(ps));
    this->last_rssi_dbm_ = (int8_t)(-((int) ps[2]) / 2);

    // Some boards/flows return zeros from GetPacketStatus (especially after RX context changes).
    // Fall back to instantaneous RSSI, which is available during RX / right after RX done.
    uint8_t ri{};
    this->cmd_read_(CMD_GET_RSSI_INST, {0x00}, &ri, 1);
    if (ri != 0) {
      this->last_rssi_dbm_ = (int8_t)(-((int) ri) / 2);
    }
  }

  this->cmd_write_(CMD_CLEAR_IRQ_STATUS, {0xFF, 0xFF});

  this->rx_idx_ = 0;
  this->rx_len_ = this->rx_buffer_.size();
  this->rx_loaded_ = true;
  return true;
}


bool SX1262::capture_rx_stream_() {
  // Semtech AN1200.53: stream from the 256-byte internal buffer while RX is still running.
  // We rely on IRQ_RX_DONE / IRQ_TIMEOUT to decide when the burst is complete (not a tiny "silence" window),
  // otherwise we may cut valid frames (WMBus T1 airtime is several ms).
  this->rx_buffer_.clear();
  this->rx_buffer_.reserve(512);

  // Ensure payload length starts at 255 (0xFF) so the packet engine doesn't stop early.
  this->write_register_(REG_RXTX_PAYLOAD_LEN, {0xFF});

  const uint32_t start_ms = millis();
  uint32_t last_change_ms = start_ms;

  size_t copied = 0;
  uint8_t state_index = 0;  // last read index (wraps 0..255)

  // Capture until RX_DONE/TIMEOUT (latched IRQ), then allow a short drain window.
  bool seen_end_irq = false;
  bool rssi_sampled = false;

  while (true) {
    const uint32_t now = millis();

    // Safety: don't hang forever
    if ((now - start_ms) > 250) {
      ESP_LOGD(TAG, "Long RX capture timeout, copied=%u", (unsigned) copied);
      break;
    }
    // Hard cap (covers max WMBus T1 raw size comfortably)
    if (copied >= 512) {
      ESP_LOGD(TAG, "Long RX capture capped at 512 bytes");
      break;
    }

    const uint8_t cur = this->read_register8_(REG_RX_ADDR_PTR);
    uint8_t avail = (uint8_t) (cur - state_index);  // uint8 wrap by design

    // Cap to remaining space
    const size_t room = 512 - copied;
    if (avail > room) {
      avail = (uint8_t) room;
    }

    // Compute a "virtual" current_index (may be < cur if capped)
    const uint8_t current_index = (uint8_t) (state_index + avail);

    // Keep receiver alive: keep RxTxPldLen always behind the current write pointer.
    this->write_register_(REG_RXTX_PAYLOAD_LEN, {(uint8_t) (current_index - 1)});

    if (avail != 0) {
      // Sample instantaneous RSSI during an active burst (more reliable than after RX ends).
      if (!rssi_sampled) {
        uint8_t ri{};
        this->cmd_read_(CMD_GET_RSSI_INST, {0x00}, &ri, 1);
        if (ri != 0) {
          this->last_rssi_dbm_ = (int8_t)(-((int) ri) / 2);
          rssi_sampled = true;
        }
      }
      uint8_t tmp[256];
      const uint8_t off = state_index;
      const uint8_t first = (uint8_t) ((off + avail <= 256) ? avail : (256 - off));
      this->read_buffer_(off, tmp, first);
      if (avail > first) {
        this->read_buffer_(0, tmp + first, (size_t) (avail - first));
      }
      this->rx_buffer_.insert(this->rx_buffer_.end(), tmp, tmp + avail);
      copied += avail;
      state_index = current_index;
      last_change_ms = now;
    } else {
      // No new bytes right now.
      const uint16_t irq = this->get_irq_status_();
      if (irq & (IRQ_RX_DONE | IRQ_TIMEOUT)) {
        seen_end_irq = true;
      }

      // After we saw end IRQ, wait a short "drain" window to pull remaining bytes, then stop.
      if (seen_end_irq && (now - last_change_ms) > 15) {
        break;
      }

      delay(1);
    }
  }

  // Cache RSSI BEFORE stopping RX (standby), otherwise GetPacketStatus may return zeros.
  {
    uint8_t ps[3]{};
    this->cmd_read_(CMD_GET_PACKET_STATUS, {0x00}, ps, sizeof(ps));
    this->last_rssi_dbm_ = (int8_t)(-((int) ps[2]) / 2);

    // Prefer instantaneous RSSI if available (prevents 0dBm logs when PacketStatus is zero).
    uint8_t ri{};
    this->cmd_read_(CMD_GET_RSSI_INST, {0x00}, &ri, 1);
    if (ri != 0) {
      this->last_rssi_dbm_ = (int8_t)(-((int) ri) / 2);
    }
  }

  // Stop RX and clear IRQs.
  this->cmd_write_(CMD_SET_STANDBY, {STANDBY_RC});

  // Optional: clear latched SX1262 device errors on boot and store before/after snapshot.
  if (this->clear_device_errors_on_boot_) {
    uint8_t e[2]{};
    this->cmd_read_(CMD_GET_DEVICE_ERRORS, {0x00}, e, sizeof(e));
    this->boot_dev_err_before_ = (uint16_t(e[0]) << 8) | uint16_t(e[1]);

    // Semtech: ClearDeviceErrors requires two NOP bytes after opcode.
    this->cmd_write_(CMD_CLEAR_DEVICE_ERRORS, {0x00, 0x00});

    uint8_t e2[2]{};
    this->cmd_read_(CMD_GET_DEVICE_ERRORS, {0x00}, e2, sizeof(e2));
    this->boot_dev_err_after_ = (uint16_t(e2[0]) << 8) | uint16_t(e2[1]);
    this->boot_dev_err_valid_ = true;
    ESP_LOGV(TAG, "DevErr cleared on boot: before=0x%04X after=0x%04X", (unsigned) this->boot_dev_err_before_, (unsigned) this->boot_dev_err_after_);
  }
  this->cmd_write_(CMD_CLEAR_IRQ_STATUS, {0xFF, 0xFF});

  if (this->rx_buffer_.empty())
    return false;

  this->rx_idx_ = 0;
  this->rx_len_ = this->rx_buffer_.size();
  this->rx_loaded_ = true;

  ESP_LOGD(TAG, "Long RX captured %u bytes", (unsigned) this->rx_len_);
  return true;
}



void SX1262::setup() {
  this->irq_edge_ = gpio::INTERRUPT_RISING_EDGE;
  this->common_setup();
  ESP_LOGV(TAG, "Setup");

  // MUST be before any SPI transfers
  this->spi_setup();

  // Optional FEM pins (if used instead of YAML switch/output)
  if (this->fem_en_pin_ != nullptr) {
    this->fem_en_pin_->setup();
    this->fem_en_pin_->digital_write(true);
  }
  if (this->fem_ctrl_pin_ != nullptr) {
    this->fem_ctrl_pin_->setup();
    this->fem_ctrl_pin_->digital_write(true);
  }
  if (this->fem_pa_pin_ != nullptr) {
    this->fem_pa_pin_->setup();
    this->fem_pa_pin_->digital_write(false);
  }

  this->reset();
  delay(10);

  // Apply RX gain (datasheet values)
  const uint8_t gain =
      (this->rx_gain_ == SX1262RxGain::POWER_SAVING) ? RX_GAIN_POWER_SAVING : RX_GAIN_BOOSTED;
  this->write_register_(REG_RX_GAIN, {gain});
  ESP_LOGI(TAG, "RX gain: %s", (this->rx_gain_ == SX1262RxGain::POWER_SAVING) ? "POWER_SAVING" : "BOOSTED");

  this->cmd_write_(CMD_SET_STANDBY, {STANDBY_RC});

  // Optional: clear latched SX1262 device errors on boot and store before/after snapshot.
  if (this->clear_device_errors_on_boot_) {
    uint8_t e[2]{};
    this->cmd_read_(CMD_GET_DEVICE_ERRORS, {0x00}, e, sizeof(e));
    this->boot_dev_err_before_ = (uint16_t(e[0]) << 8) | uint16_t(e[1]);

    // Semtech: ClearDeviceErrors requires two NOP bytes after opcode.
    this->cmd_write_(CMD_CLEAR_DEVICE_ERRORS, {0x00, 0x00});

    uint8_t e2[2]{};
    this->cmd_read_(CMD_GET_DEVICE_ERRORS, {0x00}, e2, sizeof(e2));
    this->boot_dev_err_after_ = (uint16_t(e2[0]) << 8) | uint16_t(e2[1]);
    this->boot_dev_err_valid_ = true;
    ESP_LOGV(TAG, "DevErr cleared on boot: before=0x%04X after=0x%04X", (unsigned) this->boot_dev_err_before_, (unsigned) this->boot_dev_err_after_);
  }

  // DIO2 RF switch
  this->cmd_write_(CMD_SET_DIO2_AS_RF_SWITCH_CTRL, {uint8_t(this->dio2_rf_switch_ ? 0x01 : 0x00)});

  // TCXO only if enabled
  if (this->has_tcxo_) {
    this->cmd_write_(CMD_SET_DIO3_AS_TCXO_CTRL, {DIO3_OUTPUT_3_0, 0x00, 0x00, 0x40});
    delay(5);
  }

  this->cmd_write_(CMD_CALIBRATE_IMAGE, {0xD7, 0xDB});

  this->cmd_write_(CMD_SET_PACKET_TYPE, {PACKET_TYPE_GFSK});
  this->set_rf_frequency_(868950000UL);

  this->cmd_write_(CMD_SET_BUFFER_BASE_ADDRESS, {0x00, 0x00});

  // Modulation params: 100 kbps, BT=0.5, BW, fdev=50k
  const uint32_t bitrate = 100000;
  const uint32_t br = (XTAL_FREQ * 32UL) / bitrate;

  const uint32_t freq_dev = 50000;
  const uint32_t fdev = ((uint64_t) freq_dev << 25) / XTAL_FREQ;

  this->cmd_write_(CMD_SET_MODULATION_PARAMS,
                   {(uint8_t) ((br >> 16) & 0xFF), (uint8_t) ((br >> 8) & 0xFF), (uint8_t) (br & 0xFF),
                    GFSK_PULSE_SHAPE_BT_0_5, GFSK_RX_BW_312_0, (uint8_t) ((fdev >> 16) & 0xFF),
                    (uint8_t) ((fdev >> 8) & 0xFF), (uint8_t) (fdev & 0xFF)});

  // Packet params
  const uint16_t preamble_bits = 64;
  const uint8_t preamble_msb = (uint8_t) ((preamble_bits >> 8) & 0xFF);
  const uint8_t preamble_lsb = (uint8_t) (preamble_bits & 0xFF);

  // Packet length mode:
  // - in long-GFSK streaming: FIX_LEN with 0xFF so the packet engine does not stop early
  // - in normal mode: VAR_LEN (legacy behavior) so RX_DONE triggers at end-of-packet
  const uint8_t pkt_len_mode = this->long_gfsk_packets_ ? GFSK_PACKET_FIX_LEN : GFSK_PACKET_VAR_LEN;
  this->cmd_write_(CMD_SET_PACKET_PARAMS,
                   {preamble_msb, preamble_lsb, GFSK_PREAMBLE_DETECT_16,
                    0x10,  // 16 bits sync
                    GFSK_ADDRESS_FILT_OFF, pkt_len_mode,
                    0xFF,  // max payload
                    GFSK_CRC_OFF, GFSK_WHITENING_OFF});

  // IRQ routing -> DIO1
  const uint16_t mask = this->long_gfsk_packets_ ? (IRQ_SYNC_WORD_VALID | IRQ_RX_DONE | IRQ_CRC_ERROR | IRQ_TIMEOUT)
                                      : (IRQ_RX_DONE | IRQ_CRC_ERROR | IRQ_TIMEOUT);
  const uint8_t mask_msb = (uint8_t) ((mask >> 8) & 0xFF);
  const uint8_t mask_lsb = (uint8_t) (mask & 0xFF);

  this->cmd_write_(CMD_SET_DIO_IRQ_PARAMS,
                   {mask_msb, mask_lsb,  // IRQ mask
                    mask_msb, mask_lsb,  // DIO1 mask
                    0x00, 0x00,          // DIO2 mask
                    0x00, 0x00});        // DIO3 mask

  this->restart_rx();
  ESP_LOGV(TAG, "SX1262 setup done");
}

void SX1262::restart_rx() {
  // Ping-pong between C-mode Block B (0x3D) and Block A (0xCD)
  // by changing the 2nd sync byte. This lets us catch both variants
  // without user-side configuration.
  // Bias towards Block B: every 4th hop uses Block A.
  const uint8_t sync2 = (this->sync_cycle_ == 3) ? 0xCD : 0x3D;
  this->sync_cycle_ = (uint8_t) ((this->sync_cycle_ + 1) & 0x03);

  this->set_sync_word_(sync2);

  this->cmd_write_(CMD_CLEAR_IRQ_STATUS, {0xFF, 0xFF});
  this->cmd_write_(CMD_SET_STANDBY, {STANDBY_XOSC});

  // RX continuous
  this->cmd_write_(CMD_SET_RX, {0xFF, 0xFF, 0xFF});

  this->rx_loaded_ = false;
  this->rx_idx_ = 0;
  this->rx_len_ = 0;
}


optional<uint8_t> SX1262::read() {
  if (!this->rx_loaded_) {
    if (this->long_gfsk_packets_) {
      // Don't start a long capture unless an IRQ is latched (SyncWordValid / RxDone / Timeout / CRCError).
      // Otherwise we'd stop RX periodically and miss frames.
      const uint16_t irq = this->get_irq_status_();
      if ((irq & (IRQ_SYNC_WORD_VALID | IRQ_RX_DONE | IRQ_TIMEOUT | IRQ_CRC_ERROR)) == 0) {
        return {};
      }
      ESP_LOGD(TAG, "IRQ=%04X, capturing RX stream (long GFSK)", irq);
      if (!this->capture_rx_stream_())
        return {};
    } else {
      /* Many SX1262 boards expose DIO1 as a pulse; polling GPIO can miss it.
         Use latched IRQ flags over SPI instead. */
      const uint16_t irq2 = this->get_irq_status_();
      if ((irq2 & (IRQ_RX_DONE | IRQ_TIMEOUT | IRQ_CRC_ERROR)) == 0) {
        return {};
      }
      ESP_LOGD(TAG, "IRQ=%04X, loading buffer", irq2);
      if (!this->load_rx_buffer_()) {
        if (irq2 & (IRQ_TIMEOUT | IRQ_CRC_ERROR)) {
          this->restart_rx();
        }
        return {};
      }
    }
  }

  if (this->rx_idx_ < this->rx_len_)
    return this->rx_buffer_[this->rx_idx_++];
  return {};
}

int8_t SX1262::get_rssi() {
  // Return cached RSSI captured when the RX buffer was filled.
  // In long-GFSK mode we stop RX after capture, so live GetPacketStatus
  // would often read as 0.
  return this->last_rssi_dbm_;
}

const char *SX1262::get_name() { return TAG; }

}  // namespace wmbus_radio
}  // namespace esphome
