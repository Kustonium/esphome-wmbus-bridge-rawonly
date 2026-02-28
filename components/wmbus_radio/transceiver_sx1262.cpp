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
static constexpr uint8_t CMD_SET_DIO2_AS_RF_SWITCH_CTRL = 0x9D;
static constexpr uint8_t CMD_SET_DIO3_AS_TCXO_CTRL = 0x97;
static constexpr uint8_t CMD_CALIBRATE_IMAGE = 0x98;
static constexpr uint8_t CMD_WRITE_REGISTER = 0x0D;

// SX126x constants (subset)
static constexpr uint8_t STANDBY_RC = 0x00;
static constexpr uint8_t STANDBY_XOSC = 0x01;

static constexpr uint8_t PACKET_TYPE_GFSK = 0x00;

// GFSK settings
static constexpr uint8_t GFSK_PULSE_SHAPE_BT_0_5 = 0x09;
static constexpr uint8_t GFSK_RX_BW_234_3 = 0x0A;
static constexpr uint8_t GFSK_PREAMBLE_DETECT_16 = 0x05;
static constexpr uint8_t GFSK_ADDRESS_FILT_OFF = 0x00;

// NOTE: SX126x uses 0x00 for variable length, 0x01 for fixed length.
// If set wrong, RX payload_len will be truncated (often to a small fixed size).
static constexpr uint8_t GFSK_PACKET_VARIABLE = 0x00;

static constexpr uint8_t GFSK_CRC_OFF = 0x01;
static constexpr uint8_t GFSK_WHITENING_OFF = 0x00;

// DIO3 TCXO voltage
static constexpr uint8_t DIO3_OUTPUT_3_0 = 0x06;

// IRQ mask bits
static constexpr uint16_t IRQ_RX_DONE = 0x0002;
static constexpr uint16_t IRQ_TIMEOUT = 0x0200;   // RxTxTimeout
static constexpr uint16_t IRQ_CRC_ERROR = 0x0040; // CRC error

// Sync word base register
static constexpr uint16_t REG_SYNC_WORD_0 = 0x06C0;

// Rx gain register (datasheet SX1261/2 Rev 2.2)
static constexpr uint16_t REG_RX_GAIN = 0x08AC;
static constexpr uint8_t RX_GAIN_POWER_SAVING = 0x94;
static constexpr uint8_t RX_GAIN_BOOSTED = 0x96;

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
  this->cmd_read_(CMD_GET_IRQ_STATUS, {}, irq, sizeof(irq));
  const uint16_t flags = ((uint16_t) irq[0] << 8) | irq[1];
  return (flags & IRQ_RX_DONE) != 0;
}

bool SX1262::load_rx_buffer_() {
  if (!this->has_rx_done_())
    return false;

  uint8_t st[2]{};
  this->cmd_read_(CMD_GET_RX_BUFFER_STATUS, {}, st, sizeof(st));
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

  this->cmd_write_(CMD_CLEAR_IRQ_STATUS, {0xFF, 0xFF});

  this->rx_idx_ = 0;
  this->rx_len_ = this->rx_buffer_.size();
  this->rx_loaded_ = true;
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
                    GFSK_PULSE_SHAPE_BT_0_5, GFSK_RX_BW_234_3, (uint8_t) ((fdev >> 16) & 0xFF),
                    (uint8_t) ((fdev >> 8) & 0xFF), (uint8_t) (fdev & 0xFF)});

  // Packet params
  const uint16_t preamble_bits = 64;
  const uint8_t preamble_msb = (uint8_t) ((preamble_bits >> 8) & 0xFF);
  const uint8_t preamble_lsb = (uint8_t) (preamble_bits & 0xFF);

  this->cmd_write_(CMD_SET_PACKET_PARAMS,
                   {preamble_msb, preamble_lsb, GFSK_PREAMBLE_DETECT_16,
                    0x10,  // 16 bits sync
                    GFSK_ADDRESS_FILT_OFF, GFSK_PACKET_VARIABLE,
                    0xFF,  // max payload
                    GFSK_CRC_OFF, GFSK_WHITENING_OFF});

  // IRQ routing -> DIO1
  const uint16_t mask = IRQ_RX_DONE | IRQ_CRC_ERROR | IRQ_TIMEOUT;
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
    if (!this->irq_pin_->digital_read())
      return {};
    ESP_LOGD(TAG, "IRQ detected, loading buffer");
    if (!this->load_rx_buffer_())
      return {};
  }

  if (this->rx_idx_ < this->rx_len_)
    return this->rx_buffer_[this->rx_idx_++];
  return {};
}

int8_t SX1262::get_rssi() {
  // SX126x "GetPacketStatus" returns 3 bytes.
  // For GFSK typically:
  //   st[0] = Rx status
  //   st[1] = RSSI on sync (rssiSync)
  //   st[2] = RSSI average (rssiAvg)
  // Values are 0.5 dB steps, returned as unsigned where dBm = -value/2.
  uint8_t st[3]{};
  this->cmd_read_(CMD_GET_PACKET_STATUS, {}, st, sizeof(st));

  const int rssi_sync = -((int) st[1]) / 2;
  const int rssi_avg = -((int) st[2]) / 2;
  ESP_LOGD(TAG, "PKT_STATUS: rx=%02X sync=%02X avg=%02X -> rssi_sync=%ddBm rssi_avg=%ddBm", st[0], st[1], st[2], rssi_sync,
           rssi_avg);

  // Keep previous behaviour: return average RSSI.
  return (int8_t) rssi_avg;
}

const char *SX1262::get_name() { return TAG; }

}  // namespace wmbus_radio
}  // namespace esphome
