#pragma once

#include "transceiver.h"
#include <array>

namespace esphome {
namespace wmbus_radio {

// Burst size used only when SX1276 reports FifoLevel.
// Threshold is set to CHUNK_SIZE - 1, so a burst of exactly CHUNK_SIZE bytes is safe.
static constexpr size_t SX1276_CHUNK_SIZE = 16;

// Short tail-gap bridge after FIFO temporarily becomes empty.
// At 100 kbps one byte takes ~80 us, so 1000 us covers scheduler jitter
// and small intra-frame gaps without waiting long enough to merge frames.
static constexpr uint32_t SX1276_TAIL_GAP_US = 1000;

class SX1276 : public RadioTransceiver {
 public:
  void setup() override;
  optional<uint8_t> read() override;
  void restart_rx() override;
  int8_t get_rssi() override;
  const char *get_name() override;
  bool supports_preamble_retry() const override { return true; }
  bool supports_unknown_size_raw_drain() const override { return true; }
  bool supports_weak_partial_start_abort() const override { return true; }
  bool consume_rx_abort_request() override;
  uint32_t take_fifo_overrun_count() override;

 protected:
  uint8_t sync_cycle_{0};

  // Burst chunk buffered in ESP32 RAM and served byte-by-byte to upper layer.
  std::array<uint8_t, SX1276_CHUNK_SIZE> chunk_buffer_{};
  size_t chunk_len_{0};
  size_t chunk_idx_{0};

  // True after at least one byte/chunk of the current frame has been seen.
  // Used to bridge short empty-FIFO gaps near the tail of one frame.
  bool frame_active_{false};

  int8_t last_rssi_dbm_{-127};
  bool rssi_captured_{false};
  bool abort_requested_{false};
  uint32_t fifo_overrun_count_{0};

  // Burst SPI: CS held low for the entire transfer.
  // SAFE only when caller knows at least 'len' bytes are already in FIFO.
  void spi_read_burst_(uint8_t address, uint8_t *dst, size_t len);

  // Drain one safe chunk or one safe tail byte if available.
  optional<uint8_t> drain_fifo_once_();
};

}  // namespace wmbus_radio
}  // namespace esphome
