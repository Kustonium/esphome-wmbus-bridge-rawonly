#pragma once

#include <cstdint>

namespace esphome {
namespace wmbus_radio {

// Lightweight, cached (no-SPI) chip diagnostics snapshot.
// Filled by the transceiver when it already performs SPI work for RX.
// Read by the Radio component to enrich MQTT diagnostics without racing SPI.
struct ChipDiagSnapshot {
  bool valid{false};

  // Latched IRQ flags (e.g. SyncWordValid, RxDone, Timeout)
  bool has_irq{false};
  uint16_t irq{0};

  // Device errors (SX126x: GetDeviceErrors)
  bool has_dev_err{false};
  uint16_t dev_err{0};

  // Stats (SX126x: GetStats) - counters since last reset
  bool has_stats{false};
  uint16_t stat_rx{0};
  uint16_t stat_crc{0};
  uint16_t stat_hdr{0};

  // RX buffer status (SX126x: GetRxBufferStatus)
  bool has_rx_buf{false};
  uint8_t rx_buf_len{0};
  uint8_t rx_buf_start_ptr{0};
};

}  // namespace wmbus_radio
}  // namespace esphome
