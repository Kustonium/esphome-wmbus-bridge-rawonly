// SPDX-License-Identifier: GPL-3.0-or-later
#include "transceiver_sx1276.h"

#include "esphome/core/log.h"
#include <esp_timer.h>
#include <vector>
#include <algorithm>
#include "driver/gpio.h"

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
    if (!this->rssi_captured_) {
      this->last_rssi_dbm_ = (int8_t)(-(int) this->spi_read(REG_RSSI_VALUE) / 2);
      this->rssi_captured_ = true;
    }

    this->spi_read_burst_(REG_FIFO, this->chunk_buffer_.data(), SX1276_CHUNK_SIZE);
    this->chunk_len_ = SX1276_CHUNK_SIZE;
    this->chunk_idx_ = 0;
    this->frame_active_ = true;
    return this->chunk_buffer_[this->chunk_idx_++];
  }

  // Tail path: less than threshold left, so only a single-byte read is safe.
  if (!(irq2 & FLAG2_FIFO_EMPTY)) {
    if (!this->rssi_captured_) {
      this->last_rssi_dbm_ = (int8_t)(-(int) this->spi_read(REG_RSSI_VALUE) / 2);
      this->rssi_captured_ = true;
    }

    this->frame_active_ = true;
    return this->spi_read(REG_FIFO);
  }

  return {};
}

void SX1276::setup() {
  // Original driver used DIO1=FifoEmpty with falling edge.
  // Here DIO1 is remapped to FifoLevel, which is active-high, so wake on rising edge.
  this->irq_edge_ = gpio::INTERRUPT_RISING_EDGE;

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

  this->spi_write(0x1F, (uint8_t) ((1 << 7) | (1 << 5) | 0x0A));
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

namespace esphome {
namespace wmbus_radio {

namespace {
static const char *listen_mode_name_tx_(ListenMode mode) {
  switch (mode) {
    case LISTEN_MODE_T1: return "T1 only";
    case LISTEN_MODE_C1: return "C1 only";
    case LISTEN_MODE_S1: return "S1 only";
    default: return "T1+C1";
  }
}

static const uint8_t ENC3OF6_TX[16] = {
  0x16, 0x0D, 0x0E, 0x0B, 0x1C, 0x19, 0x1A, 0x13,
  0x2C, 0x25, 0x26, 0x23, 0x34, 0x31, 0x32, 0x29
};

static uint16_t crc_wmbus_tx_(const uint8_t *data, size_t len) {
  uint16_t crc = 0x0000;
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      bool mix = (((crc & 0x8000) >> 8) ^ (b & 0x80)) != 0;
      crc = mix ? (uint16_t)((crc << 1) ^ 0x3D65) : (uint16_t)(crc << 1);
      b <<= 1;
    }
  }
  return (uint16_t)(~crc);
}

static void append_crc_block_tx_(const std::vector<uint8_t> &src, size_t off, size_t len, std::vector<uint8_t> &out) {
  const size_t start = out.size();
  for (size_t i = 0; i < len; i++) out.push_back(src[off + i]);
  const uint16_t crc = crc_wmbus_tx_(out.data() + start, len);
  out.push_back((uint8_t)(crc >> 8));
  out.push_back((uint8_t)(crc & 0xFF));
}

static std::vector<uint8_t> build_test_payload_tx_(uint8_t l_field) {
  const size_t plen = (size_t) l_field + 1;
  std::vector<uint8_t> p(plen, 0);
  p[0] = l_field;
  p[1] = 0x44;       // SND-NR
  p[2] = 0x74;       // TST manufacturer, little endian 0x5274
  p[3] = 0x52;
  p[4] = 0x78;       // ID 12345678 BCD/le-style, matches existing test utility
  p[5] = 0x56;
  p[6] = 0x34;
  p[7] = 0x12;
  p[8] = 0xFF;       // version
  p[9] = 0x00;       // medium: other
  p[10] = 0x7A;      // CI
  for (size_t i = 11; i < plen; i++) p[i] = (uint8_t)(i & 0xFF);
  return p;
}

static std::vector<uint8_t> add_dll_crc_format_a_tx_(const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> out;
  out.reserve(payload.size() + 40);
  const size_t first = payload.size() < 10 ? payload.size() : 10;
  append_crc_block_tx_(payload, 0, first, out);
  for (size_t off = first; off < payload.size();) {
    const size_t len = std::min<size_t>(16, payload.size() - off);
    append_crc_block_tx_(payload, off, len, out);
    off += len;
  }
  return out;
}

static void push_bit_tx_(std::vector<uint8_t> &out, size_t &bitpos, bool bit) {
  if ((bitpos & 7) == 0) out.push_back(0);
  if (bit) out.back() |= (uint8_t)(0x80u >> (bitpos & 7));
  bitpos++;
}

static std::vector<uint8_t> encode_3of6_tx_(const std::vector<uint8_t> &src, size_t &valid_bits) {
  std::vector<uint8_t> out;
  out.reserve((src.size() * 12 + 7) / 8);
  size_t bitpos = 0;
  for (size_t i = 0; i < src.size() * 2; i++) {
    uint8_t byte = src[i / 2];
    uint8_t nibble = (i & 1) ? (byte & 0x0F) : (byte >> 4);
    uint8_t code = ENC3OF6_TX[nibble];
    for (int b = 5; b >= 0; b--) push_bit_tx_(out, bitpos, (code & (1u << b)) != 0);
  }
  valid_bits = bitpos;
  return out;
}

static std::vector<uint8_t> encode_manchester_tx_(const std::vector<uint8_t> &src, size_t &valid_bits) {
  // Test convention: 0 -> 01, 1 -> 10. If needed we can add polarity later.
  std::vector<uint8_t> out;
  out.reserve(src.size() * 2);
  size_t bitpos = 0;
  for (uint8_t byte : src) {
    for (int b = 7; b >= 0; b--) {
      const bool bit = (byte & (1u << b)) != 0;
      push_bit_tx_(out, bitpos, bit ? true : false);
      push_bit_tx_(out, bitpos, bit ? false : true);
    }
  }
  valid_bits = bitpos;
  return out;
}

static bool bit_at_tx_(const std::vector<uint8_t> &stream, size_t bit_index) {
  const uint8_t b = stream[bit_index >> 3];
  return (b >> (7 - (bit_index & 7))) & 0x01;
}
}  // namespace

bool SX1276::transmit_test_frame(ListenMode mode, uint16_t frame_length, uint8_t tx_data_gpio) {
  if (this->irq_pin_ == nullptr) {
    ESP_LOGE(TAG, "TX test requires irq_pin wired to SX1276 DIO1/DCLK / tx_test wymaga irq_pin podlaczonego do DIO1/DCLK");
    return false;
  }
  if (frame_length < 10) frame_length = 10;
  if (frame_length > 255) frame_length = 255;

  const uint32_t bitrate = (mode == LISTEN_MODE_S1) ? 32768UL : 100000UL;
  const uint32_t fdev_hz = (mode == LISTEN_MODE_C1) ? 45000UL : 50000UL;

  this->spi_write(REG_OP_MODE, (uint8_t) 0x00);  // sleep, FSK map
  delay(2);
  this->spi_write(REG_OP_MODE, (uint8_t) 0x01);  // standby
  delay(2);

  const uint32_t frf = ((uint64_t) this->configured_frequency_hz_ * (1 << 19)) / F_OSC;
  this->spi_write(0x06, {BYTE(frf, 2), BYTE(frf, 1), BYTE(frf, 0)});

  uint32_t br = (F_OSC << 4) / bitrate;
  this->spi_write(0x5D, (uint8_t) (br & 0x0F));
  br >>= 4;
  this->spi_write(0x02, {BYTE(br, 1), BYTE(br, 0)});

  const uint16_t frd = ((uint64_t) fdev_hz * (1 << 19)) / F_OSC;
  this->spi_write(0x04, {BYTE(frd, 1), BYTE(frd, 0)});

  this->spi_write(0x09, (uint8_t) (0x80 | 10));  // PA_BOOST, moderate power
  this->spi_write(0x4D, (uint8_t) 0x84);         // normal PA boost
  this->spi_write(0x0B, (uint8_t) 0x2B);
  this->spi_write(0x0A, (uint8_t) 0x49);         // GFSK BT-ish shaping + ramp
  this->spi_write(0x30, (uint8_t) 0x00);
  this->spi_write(0x31, (uint8_t) 0x00);         // continuous mode
  this->spi_write(0x27, (uint8_t) 0x00);         // no HW sync; stream includes preamble+sync
  this->spi_write(0x25, {0x00, 0x00});
  this->spi_write(0x32, (uint8_t) 0x00);
  this->spi_write(REG_DIO_MAPPING1, (uint8_t) 0x00); // FSK continuous: DIO1=DCLK, DIO2=DATA
  this->spi_write(0x41, (uint8_t) 0x00);

  const auto payload = build_test_payload_tx_((uint8_t) frame_length);
  const auto with_crc = add_dll_crc_format_a_tx_(payload);

  size_t encoded_bits = 0;
  std::vector<uint8_t> encoded;
  if (mode == LISTEN_MODE_S1) {
    encoded = encode_manchester_tx_(with_crc, encoded_bits);
  } else if (mode == LISTEN_MODE_C1) {
    encoded = with_crc;
    encoded_bits = encoded.size() * 8;
  } else {
    encoded = encode_3of6_tx_(with_crc, encoded_bits);
  }

  std::vector<uint8_t> stream;
  stream.reserve(16 + encoded.size());
  for (int i = 0; i < 8; i++) stream.push_back(0x55); // 64-bit preamble
  if (mode == LISTEN_MODE_S1) {
    stream.push_back(0x54); stream.push_back(0x76); stream.push_back(0x96);
  } else {
    stream.push_back(0x54); stream.push_back(0x3D);
  }
  const size_t header_bits = stream.size() * 8;
  stream.insert(stream.end(), encoded.begin(), encoded.end());
  const size_t total_bits = header_bits + encoded_bits;

  gpio_set_direction((gpio_num_t) tx_data_gpio, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t) tx_data_gpio, bit_at_tx_(stream, 0) ? 1 : 0);

  this->spi_write(REG_OP_MODE, (uint8_t) 0x03);  // TX

  size_t bit_index = 0;
  const uint32_t start = millis();
  const uint32_t expected_ms = (uint32_t)((total_bits * 1000ULL + bitrate - 1) / bitrate);
  const uint32_t timeout_ms = expected_ms + 800;
  bool last_clk = this->irq_pin_->digital_read();

  while (bit_index + 1 < total_bits) {
    const bool clk = this->irq_pin_->digital_read();
    if (last_clk && !clk) { // falling edge: prepare next bit
      bit_index++;
      gpio_set_level((gpio_num_t) tx_data_gpio, bit_at_tx_(stream, bit_index) ? 1 : 0);
    }
    last_clk = clk;
    if (millis() - start > timeout_ms) {
      ESP_LOGE(TAG, "TX test timeout / timeout nadajnika testowego: bit=%u/%u", (unsigned) bit_index, (unsigned) total_bits);
      this->spi_write(REG_OP_MODE, (uint8_t) 0x01);
      gpio_set_level((gpio_num_t) tx_data_gpio, 0);
      return false;
    }
  }

  delayMicroseconds(100);
  this->spi_write(REG_OP_MODE, (uint8_t) 0x01);  // standby
  gpio_set_level((gpio_num_t) tx_data_gpio, 0);

  ESP_LOGI(TAG, "SX1276 TX test sent / wyslano test TX: mode=%s freq=%.3fMHz bitrate=%u frame_length=%u decoded=%u crc_bytes=%u encoded_bits=%u total_bits=%u gpio=%u",
           listen_mode_name_tx_(mode),
           this->configured_frequency_hz_ / 1000000.0f,
           (unsigned) bitrate,
           (unsigned) frame_length,
           (unsigned) payload.size(),
           (unsigned) with_crc.size(),
           (unsigned) encoded_bits,
           (unsigned) total_bits,
           (unsigned) tx_data_gpio);
  return true;
}

}  // namespace wmbus_radio
}  // namespace esphome
