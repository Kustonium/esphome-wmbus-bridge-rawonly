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

enum class SX1276BusyEtherMode : uint8_t { NORMAL = 0, AGGRESSIVE = 1, ADAPTIVE = 2 };

class Radio : public Component {
public:
  void set_radio(RadioTransceiver *radio) { this->radio = radio; };
  void set_diag_topic(const std::string &topic) { this->diag_topic_ = topic; }

  // Optional built-in RAW forwarding to MQTT.
  void set_telegram_topic(const std::string &topic) { this->telegram_topic_ = topic; }
  void set_target_meter_id_str(const std::string &meter_id) { this->target_meter_id_str_ = meter_id; }
  void set_target_topic(const std::string &topic) { this->target_topic_ = topic; }
  void set_target_log(bool enabled) { this->target_log_ = enabled; }

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
  void set_diag_publish_summary_15min(bool enabled) { this->diag_publish_summary_15min_ = enabled; }
  void set_diag_publish_summary_60min(bool enabled) { this->diag_publish_summary_60min_ = enabled; }
  void set_diag_publish_summary_highlight_meters(bool enabled) { this->diag_publish_summary_highlight_meters_ = enabled; }
  void set_sx1276_busy_ether_mode(SX1276BusyEtherMode mode) { this->sx1276_busy_ether_mode_ = mode; }

  void setup() override;
  void loop() override;
  void dump_config() override;
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
    // Independent windowed counters for time-based and count-based triggers.
    // They must not share state, otherwise one trigger resets the other.
    uint32_t count_window_time{0};
    int32_t  rssi_sum_window_time{0};
    uint32_t rssi_n_window_time{0};
    uint32_t interval_sum_window_time_ms{0};
    uint32_t interval_n_window_time{0};

    uint32_t count_window_count{0};
    int32_t  rssi_sum_window_count{0};
    uint32_t rssi_n_window_count{0};
    uint32_t interval_sum_window_count_ms{0};
    uint32_t interval_n_window_count{0};
    uint32_t count_window_started_ms{0};
  };
  // Key encodes both meter_id and link mode: (meter_id << 8) | (uint8_t)LinkMode.
  // This keeps T1 and C1 statistics separate for dual-mode meters
  // (e.g. a device that transmits the same ID on both T1 and C1).
  std::unordered_map<uint64_t, MeterStats> highlight_meter_stats_{};

  // Optional RAW forwarding / target forwarding.
  std::string telegram_topic_{};
  std::string target_meter_id_str_{};
  uint32_t target_meter_id_{0};
  bool target_meter_enabled_{false};
  std::string target_topic_{};
  bool target_log_{true};

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
    uint32_t preamble_retry_recovered{0};
    uint32_t t1_header_read_failed{0};
    uint32_t payload_size_unknown{0};
    uint32_t raw_drain_attempted{0};
    uint32_t raw_drain_recovered{0};
    uint32_t raw_drain_bytes{0};
    uint32_t payload_read_failed{0};
    uint32_t queue_send_failed{0};
    uint32_t fifo_overrun{0};
    uint32_t weak_start_aborted{0};
    uint32_t probe_start_aborted{0};
    uint32_t raw_drain_skipped_weak{0};
    // RSSI distribution for probe_start_aborted and weak_start_aborted.
    // Buckets: [0]>-70  [1]-70..-79  [2]-80..-89  [3]-90..-99  [4]<=-100
    uint32_t probe_abort_rssi[5]{};
    uint32_t weak_abort_rssi[5]{};
  };

  SX1276BusyEtherMode sx1276_busy_ether_mode_{SX1276BusyEtherMode::ADAPTIVE};

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

  // Independent 15-minute diagnostic counters (disabled when publish flag = false).
  uint32_t diag_15m_total_{0};
  uint32_t diag_15m_ok_{0};
  uint32_t diag_15m_truncated_{0};
  uint32_t diag_15m_dropped_{0};
  int32_t diag_15m_rssi_ok_sum_{0};
  uint32_t diag_15m_rssi_ok_n_{0};
  int32_t diag_15m_rssi_drop_sum_{0};
  uint32_t diag_15m_rssi_drop_n_{0};
  std::array<uint32_t, 3> diag_15m_mode_total_{};
  std::array<uint32_t, 3> diag_15m_mode_ok_{};
  std::array<uint32_t, 3> diag_15m_mode_dropped_{};
  std::array<uint32_t, 3> diag_15m_mode_crc_failed_{};
  std::array<int32_t, 3> diag_15m_mode_rssi_ok_sum_{};
  std::array<uint32_t, 3> diag_15m_mode_rssi_ok_n_{};
  std::array<int32_t, 3> diag_15m_mode_rssi_drop_sum_{};
  std::array<uint32_t, 3> diag_15m_mode_rssi_drop_n_{};
  std::array<uint32_t, DB_COUNT> diag_15m_dropped_by_bucket_{};
  std::array<uint32_t, SB_COUNT> diag_15m_dropped_by_stage_{};
  RxPathCounters diag_15m_rx_path_{};

  // Independent 60-minute diagnostic counters (disabled when publish flag = false).
  uint32_t diag_60min_total_{0};
  uint32_t diag_60min_ok_{0};
  uint32_t diag_60min_truncated_{0};
  uint32_t diag_60min_dropped_{0};
  int32_t diag_60min_rssi_ok_sum_{0};
  uint32_t diag_60min_rssi_ok_n_{0};
  int32_t diag_60min_rssi_drop_sum_{0};
  uint32_t diag_60min_rssi_drop_n_{0};
  std::array<uint32_t, 3> diag_60min_mode_total_{};
  std::array<uint32_t, 3> diag_60min_mode_ok_{};
  std::array<uint32_t, 3> diag_60min_mode_dropped_{};
  std::array<uint32_t, 3> diag_60min_mode_crc_failed_{};
  std::array<int32_t, 3> diag_60min_mode_rssi_ok_sum_{};
  std::array<uint32_t, 3> diag_60min_mode_rssi_ok_n_{};
  std::array<int32_t, 3> diag_60min_mode_rssi_drop_sum_{};
  std::array<uint32_t, 3> diag_60min_mode_rssi_drop_n_{};
  std::array<uint32_t, DB_COUNT> diag_60min_dropped_by_bucket_{};
  std::array<uint32_t, SB_COUNT> diag_60min_dropped_by_stage_{};
  RxPathCounters diag_60min_rx_path_{};

  // T1 symbol-level diagnostics (windowed, reset after each summary)
  uint32_t diag_t1_symbols_total_{0};
  uint32_t diag_t1_symbols_invalid_{0};
  uint32_t diag_15m_t1_symbols_total_{0};
  uint32_t diag_15m_t1_symbols_invalid_{0};
  uint32_t diag_60min_t1_symbols_total_{0};
  uint32_t diag_60min_t1_symbols_invalid_{0};
  uint32_t last_diag_summary_ms_{0};
  uint32_t last_diag_15min_summary_ms_{0};
  uint32_t last_diag_60min_summary_ms_{0};
  int32_t recent_ok_rssi_avg_{-80};
  bool recent_ok_rssi_valid_{false};

  static DropBucket bucket_for_reason_(const std::string &reason);
  static StageBucket bucket_for_stage_(const std::string &stage);
  bool meter_is_highlighted_(uint32_t meter_id) const;
  void collect_radio_rx_diag_();
  uint32_t current_false_start_like_() const;
  bool sx1276_busy_ether_aggressive_now_() const;
  bool sx1276_busy_ether_severe_now_() const;
  bool should_abort_weak_partial_start_(int rssi_dbm, size_t bytes_read, bool is_c_mode) const;
  bool should_abort_t1_probe_start_(int rssi_dbm) const;
  bool should_attempt_raw_drain_(int rssi_dbm, size_t bytes_read, bool is_c_mode) const;
  std::string derived_target_topic_() const;
  void maybe_forward_frame_(Frame &frame, uint32_t meter_id, const char *id_str, const char *log_tag);
  bool should_publish_packet_event_(const Packet *packet) const;
  void maybe_publish_diag_summary_(uint32_t now_ms);
  void maybe_publish_diag_15min_summary_(uint32_t now_ms);
  void maybe_publish_diag_60min_summary_(uint32_t now_ms);
  std::string diag_summary_topic_() const;
  std::string diag_summary_15min_topic_() const;
  std::string diag_summary_60min_topic_() const;
  std::string meter_window_topic_for_(const char *id_str, const char *trigger, const char *mode_str) const;
  void publish_meter_window_for_(const char *trigger, uint32_t elapsed_s,
                                   const char *id_str, const char *mode_str, MeterStats &st,
                                   uint32_t count_window, int32_t rssi_sum_window,
                                   uint32_t rssi_n_window,
                                   uint32_t interval_sum_window_ms,
                                   uint32_t interval_n_window,
                                   bool reset_time_window,
                                   bool reset_count_window);
  void maybe_publish_meter_windows_(uint32_t now_ms);

  // Periodic timer for meter window summaries (default: 15 min)
  uint32_t meter_window_interval_ms_{900000};
  uint32_t last_meter_window_ms_{0};
  // Count-based trigger: publish after this many packets per window (0 = disabled)
  uint32_t meter_window_count_threshold_{10};
  void publish_rx_path_event_(const char *event, const char *stage, const char *detail = nullptr, int rssi = 0);

  // Boot log / boot info fields
  bool boot_log_done_{false};
  uint32_t boot_log_last_ms_{0};
  uint32_t boot_log_count_{0};
  bool boot_info_mqtt_pending_{false};
  bool boot_info_event_pending_{false};

  // Adaptive busy-ether hold state: aggressive mode stays active until this timestamp (ms).
  // Updated once per diagnostic summary window by evaluate_busy_ether_adaptive_().
  uint32_t busy_ether_active_until_ms_{0};
  bool busy_ether_was_active_{false};  // tracks last known state for change detection
  void evaluate_busy_ether_adaptive_(uint32_t now_ms);

  // Suggestion system: publish actionable hints to {diag_topic}/suggestion.
  // Throttled per suggestion code — at most once per hour per code.
  std::unordered_map<std::string, uint32_t> last_suggestion_ms_{};
  void maybe_publish_suggestion_(uint32_t now_ms);
  std::string diag_suggestion_topic_() const;
  static constexpr uint32_t SUGGESTION_THROTTLE_MS_ = 60U * 60U * 1000U; // 1 hour

  static constexpr uint32_t DIAG_15MIN_INTERVAL_MS_ = 15U * 60U * 1000U;
  static constexpr uint32_t DIAG_60MIN_INTERVAL_MS_ = 60U * 60U * 1000U;
  bool diag_publish_summary_15min_{false};
  bool diag_publish_summary_60min_{false};
  bool diag_publish_summary_highlight_meters_{false};
  std::string diag_topic_{"wmbus/diag"};
};
} // namespace wmbus_radio
} // namespace esphome
