#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include <unordered_map>

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

  // Optional log highlighting for selected meter IDs (configured from YAML).
  // Meters are provided as a CSV string in YAML (list is joined in python).
  void set_highlight_meters_csv(const std::string &csv) { this->highlight_meters_csv_ = csv; }
  void set_highlight_ansi(bool enabled) { this->highlight_ansi_ = enabled; }
  void set_highlight_tag(const std::string &tag) { this->highlight_tag_ = tag; }
  void set_highlight_prefix(const std::string &prefix) { this->highlight_prefix_ = prefix; }

  // Publish SX1262 device errors (before/after clear) once after boot.
  void set_publish_dev_err_after_clear(bool enabled) { this->publish_dev_err_after_clear_ = enabled; }

  // Diagnostics runtime controls (can be toggled from YAML via template switches)
  void set_diag_verbose(bool enabled) { this->diag_verbose_ = enabled; }
  void set_diag_publish_raw(bool enabled) { this->diag_publish_raw_ = enabled; }
  void set_diag_publish_summary(bool enabled) { this->diag_publish_summary_ = enabled; }
  void set_diag_publish_drop_events(bool enabled) { this->diag_publish_drop_events_ = enabled; }
  void set_diag_publish_rx_path_events(bool enabled) { this->diag_publish_rx_path_events_ = enabled; }
  void set_diag_publish_highlight_only(bool enabled) { this->diag_publish_highlight_only_ = enabled; }
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

  // Per-meter reception statistics (only tracked for highlight_meters IDs)
  struct MeterStats {
    uint32_t last_seen_ms{0};      // millis() when last packet was received
    uint32_t last_interval_ms{0};  // elapsed ms since previous packet (0 = first seen)
    uint32_t interval_sum_ms{0};   // cumulative sum for average interval
    uint32_t interval_n{0};        // number of intervals recorded
    uint32_t count{0};             // total packets received (lifetime)
    int32_t  rssi_last{0};         // RSSI of the last packet
    int32_t  rssi_sum{0};          // cumulative RSSI sum (lifetime)
    uint32_t rssi_n{0};            // number of RSSI samples (lifetime)
    // Windowed counters — reset after each periodic summary
    uint32_t count_window{0};
    int32_t  rssi_sum_window{0};
    uint32_t rssi_n_window{0};
  };
  std::unordered_map<uint32_t, MeterStats> highlight_meter_stats_{};

  // Highlight configuration
  std::string highlight_meters_csv_{};
  std::vector<uint32_t> highlight_meter_ids_{};
  bool highlight_ansi_{false};
  std::string highlight_tag_{"wmbus_user"};
  std::string highlight_prefix_{"★ "};

  // SX1262 boot device errors (optional one-shot MQTT event)
  bool publish_dev_err_after_clear_{false};
  bool dev_err_cleared_pending_{false};
  uint16_t dev_err_before_{0};
  uint16_t dev_err_after_{0};


  // Diagnostics counters (published periodically if diagnostic_topic is set)
  uint32_t diag_summary_interval_ms_{60000};

  // When false, only the periodic summary is published to MQTT (still counts internally)
  bool diag_verbose_{true};
  // When false, per-packet payloads/logs omit the raw hex (much less spam)
  bool diag_publish_raw_{true};
  bool diag_publish_summary_{true};
  bool diag_publish_drop_events_{true};
  bool diag_publish_rx_path_events_{true};
  // If enabled, publish per-packet MQTT diagnostics only for ids present in
  // highlight_meters. Summary remains global and still counts everything.
  bool diag_publish_highlight_only_{false};

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

  enum StageBucket : uint8_t {
    SB_PRECHECK = 0,
    SB_T1_DECODE3OF6,
    SB_T1_L_FIELD,
    SB_T1_LENGTH_CHECK,
    SB_C1_PRECHECK,
    SB_C1_PREAMBLE,
    SB_C1_SUFFIX,
    SB_C1_L_FIELD,
    SB_C1_LENGTH_CHECK,
    SB_DLL_CRC_FIRST,
    SB_DLL_CRC_MID,
    SB_DLL_CRC_FINAL,
    SB_DLL_CRC_B1,
    SB_DLL_CRC_B2,
    SB_LINK_MODE,
    SB_OTHER,
    SB_COUNT
  };

  struct RxPathCounters {
    uint32_t irq_timeout{0};
    uint32_t preamble_read_failed{0};
    uint32_t t1_header_read_failed{0};
    uint32_t payload_size_unknown{0};
    uint32_t payload_read_failed{0};
    uint32_t queue_send_failed{0};
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
  std::array<uint32_t, SB_COUNT> diag_dropped_by_stage_{};
  RxPathCounters diag_rx_path_{};

  // T1 symbol-level diagnostics (windowed, reset after each summary)
  uint32_t diag_t1_symbols_total_{0};
  uint32_t diag_t1_symbols_invalid_{0};
  uint32_t last_diag_summary_ms_{0};

  static DropBucket bucket_for_reason_(const std::string &reason);
  static StageBucket bucket_for_stage_(const std::string &stage);
  bool meter_is_highlighted_(uint32_t meter_id) const;
  bool should_publish_packet_event_(const Packet *packet) const;
  void maybe_publish_diag_summary_(uint32_t now_ms);
  void publish_meter_window_for_(const char *trigger, uint32_t elapsed_s,
                                   const char *id_str, MeterStats &st);
  void maybe_publish_meter_windows_(uint32_t now_ms);

  // Periodic timer for meter window summaries (default: 15 min)
  uint32_t meter_window_interval_ms_{900000};
  uint32_t last_meter_window_ms_{0};
  // Count-based trigger: publish after this many packets per window (0 = disabled)
  uint32_t meter_window_count_threshold_{10};
  void publish_rx_path_event_(const char *event, const char *stage, const char *detail = nullptr, int rssi = 0);

  std::string diag_topic_{"wmbus/diag"};
};
} // namespace wmbus_radio
} // namespace esphome
