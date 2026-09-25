// SPDX-License-Identifier: GPL-3.0-or-later
#include "wmbus_tx.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif

#include <algorithm>
#include <cstdio>
#include <string>

namespace esphome {
namespace wmbus_tx {

static const char *const TAG = "wmbus_tx";

#define F_OSC 32000000
#define BYTE_OF(x, n) ((uint8_t) ((x) >> ((n) * 8)))

static constexpr uint8_t REG_OP_MODE = 0x01;
static constexpr uint8_t REG_DIO_MAPPING1 = 0x40;
static constexpr uint8_t REG_VERSION = 0x42;

static constexpr uint8_t OP_MODE_SLEEP = 0x00;
static constexpr uint8_t OP_MODE_STANDBY = 0x01;
static constexpr uint8_t OP_MODE_TX = 0x03;

// ─── SPI plumbing ────────────────────────────────────────────────────────────

uint8_t SX1276Transmitter::spi_transaction_(uint8_t operation, uint8_t address,
                                            std::initializer_list<uint8_t> data) {
  this->delegate_->begin_transaction();
  uint8_t rval = this->delegate_->transfer(operation | address);
  for (auto byte : data) rval = this->delegate_->transfer(byte);
  this->delegate_->end_transaction();
  return rval;
}

uint8_t SX1276Transmitter::spi_read_(uint8_t address) { return this->spi_transaction_(0x00, address, {0}); }

void SX1276Transmitter::spi_write_(uint8_t address, std::initializer_list<uint8_t> data) {
  this->spi_transaction_(0x80, address, data);
}

void SX1276Transmitter::spi_write_(uint8_t address, uint8_t data) { this->spi_write_(address, {data}); }

void SX1276Transmitter::reset_() {
  if (this->reset_pin_ == nullptr) return;
  this->reset_pin_->digital_write(0);
  delay(5);
  this->reset_pin_->digital_write(1);
  delay(5);
}

// ─── Frame assembly ──────────────────────────────────────────────────────────

// EN 13757-4 CRC, polynomial 0x3D65, complemented. Kept in the original
// bit-by-bit form rather than the equivalent "xor the byte in first" variant:
// the two produce identical results, but this one is the version that has been
// verified against captured telegrams, and a CRC bug would show up as the
// receiver silently rejecting every frame.
static uint16_t crc_wmbus_(const uint8_t *data, size_t len) {
  uint16_t crc = 0x0000;
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      const bool mix = (((crc & 0x8000) >> 8) ^ (b & 0x80)) != 0;
      crc = mix ? (uint16_t) ((crc << 1) ^ 0x3D65) : (uint16_t) (crc << 1);
      b <<= 1;
    }
  }
  return (uint16_t) ~crc;
}

static void append_crc_block_(const std::vector<uint8_t> &src, size_t off, size_t len,
                              std::vector<uint8_t> &out) {
  const size_t start = out.size();
  for (size_t i = 0; i < len; i++) out.push_back(src[off + i]);
  const uint16_t crc = crc_wmbus_(out.data() + start, len);
  out.push_back((uint8_t) (crc >> 8));
  out.push_back((uint8_t) (crc & 0xFF));
}

// Format A block structure: a 10-byte first block, then 16-byte blocks, each
// followed by its own CRC.
static std::vector<uint8_t> add_dll_crc_format_a_(const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> out;
  out.reserve(payload.size() + 40);
  const size_t first = payload.size() < 10 ? payload.size() : 10;
  append_crc_block_(payload, 0, first, out);
  for (size_t off = first; off < payload.size();) {
    const size_t len = std::min<size_t>(16, payload.size() - off);
    append_crc_block_(payload, off, len, out);
    off += len;
  }
  return out;
}

static void push_bit_(std::vector<uint8_t> &out, size_t &bitpos, bool bit) {
  if ((bitpos & 7) == 0) out.push_back(0);
  if (bit) out.back() |= (uint8_t) (0x80u >> (bitpos & 7));
  bitpos++;
}

// 3-of-6: every nibble becomes a 6-bit symbol with constant weight.
static std::vector<uint8_t> encode_3of6_(const std::vector<uint8_t> &src, size_t &valid_bits) {
  static const uint8_t LUT[16] = {
      0b010110, 0b001101, 0b001110, 0b001011, 0b011100, 0b011001, 0b011010, 0b010011,
      0b101100, 0b100101, 0b100110, 0b100011, 0b110100, 0b110001, 0b110010, 0b101001,
  };
  std::vector<uint8_t> out;
  out.reserve((src.size() * 12 + 7) / 8);
  size_t bitpos = 0;
  for (size_t i = 0; i < src.size() * 2; i++) {
    const uint8_t byte = src[i / 2];
    const uint8_t nibble = (i & 1) ? (uint8_t) (byte & 0x0F) : (uint8_t) (byte >> 4);
    const uint8_t code = LUT[nibble];
    for (int bit = 5; bit >= 0; bit--) push_bit_(out, bitpos, (code >> bit) & 1);
  }
  valid_bits = bitpos;
  return out;
}

// Manchester for S1: 1 -> 10, 0 -> 01.
static std::vector<uint8_t> encode_manchester_(const std::vector<uint8_t> &src, size_t &valid_bits) {
  std::vector<uint8_t> out;
  out.reserve(src.size() * 2 + 1);
  size_t bitpos = 0;
  for (uint8_t byte : src) {
    for (int bit = 7; bit >= 0; bit--) {
      const bool one = (byte >> bit) & 1;
      push_bit_(out, bitpos, one);
      push_bit_(out, bitpos, !one);
    }
  }
  valid_bits = bitpos;
  return out;
}

static bool bit_at_(const std::vector<uint8_t> &stream, size_t bit_index) {
  const size_t byte_index = bit_index >> 3;
  if (byte_index >= stream.size()) return false;
  return (stream[byte_index] >> (7 - (bit_index & 7))) & 1;
}

static const char *mode_name_(TxMode mode) {
  switch (mode) {
    case TX_MODE_C1: return "C1";
    case TX_MODE_S1: return "S1";
    default: return "T1";
  }
}

// ─── Component ───────────────────────────────────────────────────────────────

void SX1276Transmitter::setup() {
  if (this->reset_pin_ != nullptr) this->reset_pin_->setup();
  if (this->dclk_pin_ != nullptr) this->dclk_pin_->setup();
  this->spi_setup();
  this->reset_();

  const uint8_t revision = this->spi_read_(REG_VERSION);
  if (revision < 0x11 || revision > 0x13) {
    ESP_LOGE(TAG, "Invalid SX1276 silicon revision / nieprawidlowa rewizja ukladu: 0x%02X", revision);
    this->mark_failed();
    return;
  }

  this->ready_ = true;
  ESP_LOGI(TAG, "SX1276 transmitter ready / nadajnik gotowy: rev=0x%02X freq=%.3fMHz mode=%s interval=%us frame=%u bytes",
           revision, this->frequency_hz_ / 1000000.0f, mode_name_(this->mode_),
           (unsigned) (this->interval_ms_ / 1000), (unsigned) this->frame_.size());
}

void SX1276Transmitter::dump_config() {
  ESP_LOGCONFIG(TAG, "wM-Bus transmitter (SX1276):");
  ESP_LOGCONFIG(TAG, "  Operation: transmit only - this component never receives");
  ESP_LOGCONFIG(TAG, "  Mode: %s", mode_name_(this->mode_));
  ESP_LOGCONFIG(TAG, "  Frequency: %.3f MHz", this->frequency_hz_ / 1000000.0f);
  ESP_LOGCONFIG(TAG, "  Power: %u dBm (PA_BOOST nibble %u)", (unsigned) this->power_dbm_,
                (unsigned) ((this->power_dbm_ - 2) & 0x0F));
  ESP_LOGCONFIG(TAG, "  Interval: %u s", (unsigned) (this->interval_ms_ / 1000));
  ESP_LOGCONFIG(TAG, "  Frame: %u bytes", (unsigned) this->frame_.size());
  ESP_LOGCONFIG(TAG, "  DATA GPIO: %u (SX1276 DIO2)", (unsigned) this->data_gpio_);
  if (this->reset_pin_ != nullptr) LOG_PIN("  Reset pin: ", this->reset_pin_);
  if (this->dclk_pin_ != nullptr) LOG_PIN("  DCLK pin (SX1276 DIO1): ", this->dclk_pin_);
}

void SX1276Transmitter::loop() {
  if (!this->ready_) return;
  const uint32_t now = millis();
  if (this->last_tx_ms_ != 0 && (now - this->last_tx_ms_) < this->interval_ms_) return;
  this->last_tx_ms_ = now;
  this->transmit();
}

bool SX1276Transmitter::transmit() {
  if (this->dclk_pin_ == nullptr) {
    ESP_LOGE(TAG, "dclk_pin is required: the bit clock comes from SX1276 DIO1 / dclk_pin jest wymagany");
    return false;
  }
  if (this->frame_.empty()) {
    ESP_LOGE(TAG, "No frame configured / nie skonfigurowano ramki");
    return false;
  }

  const uint32_t bitrate = (this->mode_ == TX_MODE_S1) ? 32768UL : 100000UL;
  const uint32_t fdev_hz = (this->mode_ == TX_MODE_C1) ? 45000UL : 50000UL;

  this->spi_write_(REG_OP_MODE, OP_MODE_SLEEP);  // sleep, FSK map
  delay(2);
  this->spi_write_(REG_OP_MODE, OP_MODE_STANDBY);
  delay(2);

  const uint32_t frf = ((uint64_t) this->frequency_hz_ * (1 << 19)) / F_OSC;
  this->spi_write_(0x06, {BYTE_OF(frf, 2), BYTE_OF(frf, 1), BYTE_OF(frf, 0)});

  uint32_t br = (F_OSC << 4) / bitrate;
  this->spi_write_(0x5D, (uint8_t) (br & 0x0F));
  br >>= 4;
  this->spi_write_(0x02, {BYTE_OF(br, 1), BYTE_OF(br, 0)});

  const uint16_t frd = ((uint64_t) fdev_hz * (1 << 19)) / F_OSC;
  this->spi_write_(0x04, {BYTE_OF(frd, 1), BYTE_OF(frd, 0)});

  // RegPaConfig: PaSelect=1 (PA_BOOST) plus the OutputPower nibble. On PA_BOOST
  // the datasheet gives Pout = 17 - (15 - OutputPower) dBm, so the nibble is
  // simply power_dbm - 2. The default 12 dBm keeps the nibble at 10, which is
  // what this component transmitted before the option existed.
  //
  // The point of exposing it is measurement: stepping the power down on a fixed
  // emission is the only way to compare receivers by the level at which each
  // one stops decoding, instead of arguing about which board "hears better".
  const uint8_t pa_nibble = (uint8_t) ((this->power_dbm_ - 2) & 0x0F);
  this->spi_write_(0x09, (uint8_t) (0x80 | pa_nibble));
  this->spi_write_(0x4D, (uint8_t) 0x84);         // normal PA boost
  this->spi_write_(0x0B, (uint8_t) 0x2B);
  this->spi_write_(0x0A, (uint8_t) 0x49);         // shaping + ramp
  this->spi_write_(0x30, (uint8_t) 0x00);
  this->spi_write_(0x31, (uint8_t) 0x00);         // continuous mode
  this->spi_write_(0x27, (uint8_t) 0x00);         // no HW sync; the stream carries its own
  this->spi_write_(0x25, {0x00, 0x00});
  this->spi_write_(0x32, (uint8_t) 0x00);
  this->spi_write_(REG_DIO_MAPPING1, (uint8_t) 0x00);  // DIO1=DCLK, DIO2=DATA
  this->spi_write_(0x41, (uint8_t) 0x00);

  const auto with_crc = add_dll_crc_format_a_(this->frame_);

  size_t encoded_bits = 0;
  std::vector<uint8_t> encoded;
  if (this->mode_ == TX_MODE_S1) {
    encoded = encode_manchester_(with_crc, encoded_bits);
  } else if (this->mode_ == TX_MODE_C1) {
    // Mode C carries a two-byte indicator ahead of the link layer: 0x54, then
    // 0xCD for format A (0x3D for format B). It belongs in the PAYLOAD, after
    // the sync word, because that is where a receiver reads it -
    // Packet::link_mode() keys on the leading 0x54 and l_field() takes the
    // length from index 2, not 0.
    //
    // Without it this mode emitted a bare DLL frame, which a receiver cannot
    // tell from an uncoded T1 capture: it sees the L-field where it expects
    // 0x54, classifies the frame as T1 and fails 3-of-6 on data that was
    // never 3-of-6 encoded. The mode was evidently never exercised end to end.
    //
    // Added here rather than next to the sync bytes below on purpose: those
    // count into header_bits, which the DCLK diagnostics report as the part of
    // the stream that is NOT in the receiver's drain. These two bytes are.
    encoded.reserve(with_crc.size() + 2);
    encoded.push_back(0x54);
    encoded.push_back(0xCD);
    encoded.insert(encoded.end(), with_crc.begin(), with_crc.end());
    encoded_bits = encoded.size() * 8;
  } else {
    encoded = encode_3of6_(with_crc, encoded_bits);
  }

  // Preamble length differs by mode. T1/C1 send 64 chips, which is what those
  // meters do and what receivers are tuned for. S-mode is specified with a much
  // longer one - EN 13757-4 puts it at 279 chips - and at 32.768 kchip/s the
  // 64-chip header lasts under 2 ms, which is not what an S1 receiver's
  // preamble detector is given by a real device. 36 bytes = 288 chips keeps the
  // byte boundary and stays just above the specified length.
  //
  // This matters when the transmitter is used to debug a receiver: a short
  // preamble makes the test signal atypical exactly in the part being tested.
  const int preamble_bytes = (this->mode_ == TX_MODE_S1) ? 36 : 8;

  std::vector<uint8_t> stream;
  stream.reserve(preamble_bytes + 3 + encoded.size());
  for (int i = 0; i < preamble_bytes; i++) stream.push_back(0x55);
  if (this->mode_ == TX_MODE_S1) {
    stream.push_back(0x54);
    stream.push_back(0x76);
    stream.push_back(0x96);
  } else {
    stream.push_back(0x54);
    stream.push_back(0x3D);
  }
  const size_t header_bits = stream.size() * 8;
  stream.insert(stream.end(), encoded.begin(), encoded.end());
  const size_t total_bits = header_bits + encoded_bits;
  // Allocate before TX. During TX only timestamp the completed DATA updates;
  // no formatting, publication or allocation belongs in the timing loop.
  std::vector<uint32_t> service_times;
  if (this->dclk_diagnostics_) service_times.resize(total_bits - 1);

  gpio_set_direction((gpio_num_t) this->data_gpio_, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t) this->data_gpio_, bit_at_(stream, 0) ? 1 : 0);

  this->spi_write_(REG_OP_MODE, OP_MODE_TX);

  // Clock the stream out against DCLK. The radio owns the timing; we only have
  // to present the next bit on every falling edge.
  size_t bit_index = 0;
  const uint32_t start = millis();
  const uint32_t expected_ms = (uint32_t) ((total_bits * 1000ULL + bitrate - 1) / bitrate);
  const uint32_t timeout_ms = expected_ms + 800;
  bool last_clk = this->dclk_pin_->digital_read();
  uint32_t last_edge_ms = start;

  // This loop must not miss a DCLK falling edge - at 100 kb/s a bit is 10 us -
  // so it does NOT yield between bits. What it must also not do is spin for
  // most of a second with the CPU to itself, which is what the old version did
  // whenever DCLK was not ticking: it waited out the whole expected+800 ms
  // window on every attempt. At interval: 1s that is a tight busy-wait for the
  // best part of every second, and it is why the long frame built for the
  // LR1121 buffer experiment was marked DO NOT re-enable on 2026-08-25.
  //
  // Stall detection and watchdog servicing prevent long blocked loops:
  //   * a stalled DCLK is now detected in tens of milliseconds instead of the
  //     full window - thousands of bit periods is already unambiguous,
  //   * App.feed_wdt() is called while spinning. It is rate-limited inside
  //     ESPHome. Its execution cost and task preemption are not assumed zero.
  //
  // The nominal transmission duration is short: 2684 bits at
  // 100 kb/s is ~27 ms, which no watchdog objects to.
  while (bit_index + 1 < total_bits) {
    const bool clk = this->dclk_pin_->digital_read();
    if (last_clk && !clk) {
      bit_index++;
      gpio_set_level((gpio_num_t) this->data_gpio_, bit_at_(stream, bit_index) ? 1 : 0);
      if (this->dclk_diagnostics_)
        service_times[bit_index - 1] = (uint32_t) esp_timer_get_time();
      last_edge_ms = millis();
    }
    last_clk = clk;

    const uint32_t now = millis();
    // Before the first edge the radio may still be leaving standby, so that
    // wait gets a longer allowance than a stall in mid-frame.
    const uint32_t stall_limit_ms = (bit_index == 0) ? 200 : 50;
    if (now - last_edge_ms > stall_limit_ms) {
      ESP_LOGE(TAG, "DCLK stalled / zegar bitowy zamarl: bit=%u/%u after %ums - "
                    "is dclk_pin really wired to DIO1?",
               (unsigned) bit_index, (unsigned) total_bits, (unsigned) (now - last_edge_ms));
      this->spi_write_(REG_OP_MODE, OP_MODE_STANDBY);
      gpio_set_level((gpio_num_t) this->data_gpio_, 0);
      return false;
    }
    if (now - start > timeout_ms) {
      ESP_LOGE(TAG, "TX timeout / timeout nadawania: bit=%u/%u - is dclk_pin really wired to DIO1?",
               (unsigned) bit_index, (unsigned) total_bits);
      this->spi_write_(REG_OP_MODE, OP_MODE_STANDBY);
      gpio_set_level((gpio_num_t) this->data_gpio_, 0);
      return false;
    }
    App.feed_wdt();
  }

  delayMicroseconds(100);
  this->spi_write_(REG_OP_MODE, OP_MODE_STANDBY);
  gpio_set_level((gpio_num_t) this->data_gpio_, 0);

  if (this->dclk_diagnostics_ && service_times.size() >= 2) {
    // These are software DATA-service intervals, not hardware DCLK timestamps.
    // A >=1.5-bit interval is suspicious, not proof of a lost physical edge.
    const uint32_t threshold_us = (1500000UL + bitrate - 1) / bitrate;
    uint32_t max_us = 0, long_gaps = 0, saved = 0;
    std::string gaps;
    for (size_t i = 1; i < service_times.size(); i++) {
      const uint32_t dt = service_times[i] - service_times[i - 1];
      max_us = std::max(max_us, dt);
      if (dt < threshold_us) continue;
      long_gaps++;
      if (saved < 64) {
        char row[64];
        snprintf(row, sizeof(row), "%s[%u,%u]", saved == 0 ? "" : ",",
                 (unsigned) (i + 1), (unsigned) dt);
        gaps += row;
        saved++;
      }
    }
    const uint32_t seq = ++this->dclk_seq_;
    const uint32_t span_us = service_times.back() - service_times.front();
    ESP_LOGI(TAG, "DCLK diag v1: seq=%u updates=%u span_us=%u max_us=%u gaps_ge_%uus=%u saved=%u",
             (unsigned) seq, (unsigned) service_times.size(), (unsigned) span_us,
             (unsigned) max_us, (unsigned) threshold_us, (unsigned) long_gaps, (unsigned) saved);
#ifdef USE_MQTT
    if (mqtt::global_mqtt_client != nullptr && mqtt::global_mqtt_client->is_connected()) {
      char head[384];
      snprintf(head, sizeof(head),
               "{\"schema\":1,\"seq\":%u,\"uptime_ms\":%u,\"bitrate\":%u,\"header_bits\":%u,"
               "\"updates\":%u,\"span_us\":%u,\"max_us\":%u,\"threshold_us\":%u,"
               "\"long_gaps\":%u,\"saved\":%u,\"gap_fields\":[\"stream_bit\",\"dt_us\"],\"gaps\":[",
               (unsigned) seq, (unsigned) millis(), (unsigned) bitrate, (unsigned) header_bits,
               (unsigned) service_times.size(), (unsigned) span_us, (unsigned) max_us,
               (unsigned) threshold_us, (unsigned) long_gaps, (unsigned) saved);
      mqtt::global_mqtt_client->publish("wmbus/txgen/diag/dclk", std::string(head) + gaps + "]}", 0, true);
    }
#endif
  }

  ESP_LOGI(TAG, "Frame sent / wyslano ramke: mode=%s freq=%.3fMHz power=%udBm bitrate=%u body=%u crc=%u bits=%u",
           mode_name_(this->mode_), this->frequency_hz_ / 1000000.0f,
           (unsigned) this->power_dbm_, (unsigned) bitrate,
           (unsigned) this->frame_.size(), (unsigned) with_crc.size(), (unsigned) total_bits);
  return true;
}

}  // namespace wmbus_tx
}  // namespace esphome
