#include "transceiver_sx1276.h"

#include "esphome/core/log.h"
#include <esp_timer.h>

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
                   : "T1+C1 (both, 3:1 bias)";
    ESP_LOGI(TAG, "Listen mode / tryb nasluchu: %s", lm);
  }
  this->reset();

  const uint8_t revision = this->spi_read(0x42);
  if (revision < 0x11 || revision > 0x13) {
    ESP_LOGE(TAG, "Invalid silicon revision / nieprawidlowa rewizja ukladu: %02X", revision);
    return;
  }

  const uint32_t frequency = 868950000;
  const uint32_t frf = ((uint64_t) frequency * (1 << 19)) / F_OSC;
  this->spi_write(0x06, {BYTE(frf, 2), BYTE(frf, 1), BYTE(frf, 0)});

  this->spi_write(0x12, {2, 2});

  const uint16_t freq_dev = 50000;
  const uint16_t frd = ((uint64_t) freq_dev * (1 << 19)) / F_OSC;
  this->spi_write(0x04, {BYTE(frd, 1), BYTE(frd, 0)});

  const uint32_t bitrate = 100000;
  uint32_t br = (F_OSC << 4) / bitrate;
  this->spi_write(0x5D, (uint8_t) (br & 0x0F));
  br >>= 4;
  this->spi_write(0x02, {BYTE(br, 1), BYTE(br, 0)});

  const uint16_t preamble_length = 32 / 8;
  this->spi_write(0x25, {BYTE(preamble_length, 1), BYTE(preamble_length, 0)});

  this->spi_write(0x1F, (uint8_t) ((1 << 7) | (1 << 5) | 0x0A));
  this->spi_write(0x0D, (uint8_t) ((1 << 4) | (1 << 3) | 0b110));
  this->spi_write(0x24, (uint8_t) 0b111);

  const uint8_t sync_cfg = (1 << 5) | (1 << 4) | (2 - 1);
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
