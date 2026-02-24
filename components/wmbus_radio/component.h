#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <functional>
#include <string>

#include "freertos/FreeRTOS.h"

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"

#include "esphome/components/spi/spi.h"
// Keep component lightweight (no full wmbusmeters stack)
#include "link_mode.h"

#include "packet.h"
#include "transceiver.h"

namespace esphome {
namespace wmbus_radio {

class Radio : public Component {
public:
  void set_radio(RadioTransceiver *radio) { this->radio = radio; };
  void set_diag_topic(const std::string &topic) { this->diag_topic_ = topic; }

  // Diagnostics runtime controls (can be toggled from YAML via template switches)
  void set_diag_verbose(bool enabled) { this->diag_verbose_ = enabled; }
  void set_diag_publish_raw(bool enabled) { this->diag_publish_raw_ = enabled; }
  void set_diag_summary_interval_ms(uint32_t interval_ms) {
    // Keep it sane: minimum 5s
    this->diag_summary_interval_ms_ = interval_ms < 5000 ? 5000 : interval_ms;
  }

  void setup() override;
  void loop() override;
  void receive_frame();

  void add_frame_handler(std::function<void(Frame *)> &&callback);

protected:
  static void wakeup_receiver_task_from_isr(TaskHandle_t *arg);
  static void receiver_task(Radio *arg);

  RadioTransceiver *radio{nullptr};
  TaskHandle_t receiver_task_handle_{nullptr};
  QueueHandle_t packet_queue_{nullptr};

  std::vector<std::function<void(Frame *)>> handlers_;


  // Diagnostics counters (published periodically if diagnostic_topic is set)
  uint32_t diag_summary_interval_ms_{60000};

  // When false, only the periodic summary is published (still counts internally)
  bool diag_verbose_{true};
  // When false, per-packet payloads/logs omit the raw hex (much less spam)
  bool diag_publish_raw_{true};

  enum DropBucket : uint8_t {
    DB_TOO_SHORT = 0,
    DB_DECODE_FAILED,
    // DLL CRC failed (we drop the packet before publishing to avoid poisoning downstream decoders)
    DB_DLL_CRC_FAILED,
    DB_UNKNOWN_PREAMBLE,
    DB_L_FIELD_INVALID,
    DB_UNKNOWN_LINK_MODE,
    DB_OTHER,
    DB_COUNT
  };

  // Windowed counters (reset after each published summary)
  uint32_t diag_total_{0};
  uint32_t diag_ok_{0};
  uint32_t diag_truncated_{0};
  uint32_t diag_dropped_{0};
  // RSSI aggregates (integer averages)
  int32_t diag_rssi_ok_sum_{0};
  uint32_t diag_rssi_ok_n_{0};
  int32_t diag_rssi_drop_sum_{0};
  uint32_t diag_rssi_drop_n_{0};

  // Per-mode window stats (index: (uint8_t)LinkMode)
  std::array<uint32_t, 3> diag_mode_total_{};
  std::array<uint32_t, 3> diag_mode_ok_{};
  std::array<uint32_t, 3> diag_mode_dropped_{};
  std::array<uint32_t, 3> diag_mode_crc_failed_{};
  std::array<int32_t, 3> diag_mode_rssi_ok_sum_{};
  std::array<uint32_t, 3> diag_mode_rssi_ok_n_{};
  std::array<int32_t, 3> diag_mode_rssi_drop_sum_{};
  std::array<uint32_t, 3> diag_mode_rssi_drop_n_{};

  std::array<uint32_t, DB_COUNT> diag_dropped_by_bucket_{};

  // T1 symbol-level diagnostics (windowed, reset after each summary)
  uint32_t diag_t1_symbols_total_{0};
  uint32_t diag_t1_symbols_invalid_{0};
  uint32_t last_diag_summary_ms_{0};

  static DropBucket bucket_for_reason_(const std::string &reason);
  void maybe_publish_diag_summary_(uint32_t now_ms);

  std::string diag_topic_{"wmbus/diag"};
};
} // namespace wmbus_radio
} // namespace esphome
