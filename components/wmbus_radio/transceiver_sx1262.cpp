#include "transceiver_sx1262.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"

// Semtech reference driver (Clear BSD) - avoids command-level bugs.
extern "C" {
#include "sx126x_hal.h"
#include "sx126x.h"

namespace esphome {
namespace wmbus_radio {

static const char *TAG = "SX1262";

// DIO3 TCXO voltage (datasheet encoding; Semtech driver uses same values)
static constexpr uint8_t DIO3_OUTPUT_3_0 = 0x06;

// Sync word base register
static constexpr uint16_t REG_SYNC_WORD_0 = 0x06C0;

// Rx gain register (datasheet SX1261/2 Rev 2.2)
static constexpr uint16_t REG_RX_GAIN = 0x08AC;
static constexpr uint8_t RX_GAIN_POWER_SAVING = 0x94;
static constexpr uint8_t RX_GAIN_BOOSTED = 0x96;

// RF frequency step for SX126x: 32e6 / 2^25 (Hz)
static constexpr uint32_t XTAL_FREQ = 32000000UL;

static inline void u16_to_be(uint16_t v, uint8_t &msb, uint8_t &lsb) {
  msb = (uint8_t) ((v >> 8) & 0xFF);
  lsb = (uint8_t) (v & 0xFF);
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

// Legacy helpers kept for troubleshooting; Semtech driver is used for all radio ops.
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
  // Keep a small helper for single-register tweaks.
  // Main init uses sx126x_write_register.
  uint8_t msb, lsb;
  u16_to_be(addr, msb, lsb);
  this->wait_while_busy_();
  this->delegate_->begin_transaction();
  this->delegate_->transfer(0x0D);  // WriteRegister
  this->delegate_->transfer(msb);
  this->delegate_->transfer(lsb);
  for (auto b : data)
    this->delegate_->transfer(b);
  this->delegate_->end_transaction();
  this->wait_while_busy_();
}

void SX1262::set_rf_frequency_(uint32_t freq_hz) {
  (void) sx126x_set_rf_freq(this, freq_hz);
}

void SX1262::set_sync_word_(uint8_t sync2) {
  // 16-bit sync word: 0x54?? (Block A/B selector in 2nd byte)
  const uint8_t sw[2] = {0x54, sync2};
  (void) sx126x_set_gfsk_sync_word(this, sw, sizeof(sw));
}

bool SX1262::has_rx_done_() {
  sx126x_irq_mask_t irq = SX126X_IRQ_NONE;
  (void) sx126x_get_irq_status(this, &irq);
  return (irq & SX126X_IRQ_RX_DONE) != 0;
}

bool SX1262::load_rx_buffer_() {
  sx126x_irq_mask_t irq = SX126X_IRQ_NONE;
  (void) sx126x_get_irq_status(this, &irq);
  if ((irq & SX126X_IRQ_RX_DONE) == 0)
    return false;

  // If CRC error, drop without touching buffer (mirrors Semtech flow).
  if ((irq & SX126X_IRQ_CRC_ERROR) != 0) {
    (void) sx126x_clear_irq_status(this, SX126X_IRQ_ALL);
    return false;
  }

  uint8_t payload_len = 0;
  uint8_t start_ptr = 0;
  (void) sx126x_get_rx_buffer_status(this, &payload_len, &start_ptr);

  if (payload_len == 0) {
    (void) sx126x_clear_irq_status(this, SX126X_IRQ_ALL);
    return false;
  }

  // Hard hint: payload_len is uint8 -> 255 is the max in packet mode.
  if (payload_len == 0xFF) {
    ESP_LOGW(TAG, "RX payload_len==255 (hit packet-mode limit) -> may be truncated/drop for long telegrams");
  }

  this->rx_buffer_.assign(payload_len, 0);
  (void) sx126x_read_buffer(this, start_ptr, this->rx_buffer_.data(), payload_len);

  (void) sx126x_clear_irq_status(this, SX126X_IRQ_ALL);

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

  // Reset via Semtech HAL
  (void) sx126x_reset(this);
  delay(10);

  // Apply RX gain (datasheet values)
  const uint8_t gain =
      (this->rx_gain_ == SX1262RxGain::POWER_SAVING) ? RX_GAIN_POWER_SAVING : RX_GAIN_BOOSTED;
  (void) sx126x_write_register(this, REG_RX_GAIN, &gain, 1);
  ESP_LOGI(TAG, "RX gain: %s", (this->rx_gain_ == SX1262RxGain::POWER_SAVING) ? "POWER_SAVING" : "BOOSTED");

  (void) sx126x_set_standby(this, SX126X_STANDBY_CFG_RC);

  // DIO2 RF switch
  (void) sx126x_set_dio2_as_rf_sw_ctrl(this, this->dio2_rf_switch_);

  // TCXO only if enabled
  if (this->has_tcxo_) {
    // 0x000040 ~= 1ms in Semtech RTC steps (matches old behaviour)
    (void) sx126x_set_dio3_as_tcxo_ctrl(this, (sx126x_tcxo_ctrl_voltages_t) DIO3_OUTPUT_3_0, 0x000040);
    delay(5);
  }

  (void) sx126x_cal_img(this, 0xD7, 0xDB);

  (void) sx126x_set_pkt_type(this, SX126X_PKT_TYPE_GFSK);
  this->set_rf_frequency_(868950000UL);

  (void) sx126x_set_buffer_base_address(this, 0x00, 0x00);

  // Modulation params: 100 kbps, BT=0.5, BW=234.3k, fdev=50k
  sx126x_mod_params_gfsk_t mp{};
  mp.br_in_bps = 100000;
  mp.fdev_in_hz = 50000;
  mp.pulse_shape = SX126X_GFSK_PULSE_SHAPE_BT_05;
  mp.bw_dsb_param = SX126X_GFSK_BW_234300;
  (void) sx126x_set_gfsk_mod_params(this, &mp);

  // Packet params (variable length, 16-bit sync, no CRC/whitening)
  sx126x_pkt_params_gfsk_t pp{};
  pp.preamble_len_in_bits = 64;
  pp.preamble_detector = SX126X_GFSK_PREAMBLE_DETECTOR_16_BITS;
  pp.sync_word_len_in_bits = 16;
  pp.address_filtering = SX126X_GFSK_ADDRESS_FILTERING_DISABLE;
  pp.header_type = SX126X_GFSK_PKT_LEN_MODE_VAR;
  pp.pld_len_in_bytes = 0xFF;  // max payload accepted
  pp.crc_type = SX126X_GFSK_CRC_OFF;
  pp.dc_free = SX126X_GFSK_DC_FREE_OFF;
  (void) sx126x_set_gfsk_pkt_params(this, &pp);

  // IRQ routing -> DIO1
  const sx126x_irq_mask_t mask = (sx126x_irq_mask_t) (SX126X_IRQ_RX_DONE | SX126X_IRQ_CRC_ERROR | SX126X_IRQ_TIMEOUT);
  (void) sx126x_set_dio_irq_params(this, mask, mask, SX126X_IRQ_NONE, SX126X_IRQ_NONE);

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

  (void) sx126x_clear_irq_status(this, SX126X_IRQ_ALL);
  (void) sx126x_set_standby(this, SX126X_STANDBY_CFG_XOSC);

  // RX continuous
  (void) sx126x_set_rx(this, SX126X_RX_CONTINUOUS);

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
  sx126x_pkt_status_gfsk_t st{};
  (void) sx126x_get_gfsk_pkt_status(this, &st);
  ESP_LOGD(TAG, "PKT_STATUS: rssi_sync=%ddBm rssi_avg=%ddBm", (int) st.rssi_sync, (int) st.rssi_avg);
  return st.rssi_avg;
}

const char *SX1262::get_name() { return TAG; }

}  // namespace wmbus_radio
}  // namespace esphome
