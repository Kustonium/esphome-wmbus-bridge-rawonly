// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <deque>
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

// Only pointers to these entity types are kept on Radio, and only the sensor/
// number platform .cpp files call methods on them, so a forward declaration
// is enough here and keeps sensor.h/number.h out of every translation unit
// that includes component.h.
namespace esphome {
namespace sensor { class Sensor; }
namespace number { class Number; }
namespace select { class Select; }
}  // namespace esphome

namespace esphome {
namespace wmbus_radio {

enum class SX1276BusyEtherMode : uint8_t { NORMAL = 0, AGGRESSIVE = 1, ADAPTIVE = 2 };

class Radio : public Component {
public:
  void set_radio(RadioTransceiver *radio) { this->radio = radio; };
  void set_diag_topic(const std::string &topic) { this->diag_topic_ = topic; }

  // Always-on radio health pulse + ESP-side meter flags (independent of
  // diagnostic_mode). The addon presents Layer 1 ("ESP alive = ear alive") and
  // the ESP-flagged meter badge from these, even when diagnostics are off.
  void set_health_topic(const std::string &topic) { this->health_topic_ = topic; }
  void set_meters_topic(const std::string &topic) { this->meters_topic_ = topic; }
  void set_rssi_topic(const std::string &topic) { this->rssi_topic_ = topic; }

  // Optional built-in RAW forwarding to MQTT.
  void set_telegram_topic(const std::string &topic) { this->telegram_topic_ = topic; }
  void set_rx_topic(const std::string &topic) { this->rx_topic_ = topic; }
  void set_target_meter_id_str(const std::string &meter_id) { this->target_meter_id_str_ = meter_id; }
  void set_target_topic(const std::string &topic) { this->target_topic_ = topic; }
  void set_target_log(bool enabled) { this->target_log_ = enabled; }
  void set_publish_radio_raw(bool enabled) { this->publish_radio_raw_ = enabled; }
  void set_publish_rssi(bool enabled) { this->publish_rssi_ = enabled; }

  // Per-topic MQTT QoS (0/1/2). Defaults all preserve the pre-existing
  // hardcoded behaviour: telegram/rssi/health/diagnostics stayed at 0, the
  // /rx metadata channel stayed at 1. See mqtt_publish.cpp / diagnostics.cpp /
  // meter_stats.cpp / rf_runtime.cpp for where each is used.
  void set_telegram_qos(uint8_t qos) { this->telegram_qos_ = qos; }
  void set_rssi_qos(uint8_t qos) { this->rssi_qos_ = qos; }
  void set_health_qos(uint8_t qos) { this->health_qos_ = qos; }
  void set_diag_qos(uint8_t qos) { this->diag_qos_ = qos; }
  void set_rx_qos(uint8_t qos) { this->rx_qos_ = qos; }

  // Optional runtime QoS control for the two RAM-buffered topics (raw
  // telegram + /rx metadata - see mqtt_outbox.cpp for why exactly these two).
  // Wired in from the optional select: platform (components/wmbus_radio/
  // select). set_telegram_qos()/set_rx_qos() above already are what the
  // select's control() calls into - these pointers only exist so setup() can
  // push the compiled starting value to the entity once, the same way
  // buffer_capacity_number_ is seeded by publish_initial_buffer_state_().
  void set_telegram_qos_select(select::Select *s) { this->telegram_qos_select_ = s; }
  void set_rx_qos_select(select::Select *s) { this->rx_qos_select_ = s; }

  // RAM store-and-forward for the telegram/rx-metadata stream. See
  // mqtt_outbox.cpp for the rationale: today, when the broker is unreachable,
  // reception continues but every publish is silently skipped (confirmed by
  // the project's own docs). This buffers whatever could not be published and
  // flushes it, oldest first, once MQTT reconnects, instead of losing it.
  //
  // mqtt_buffer_size (YAML) sets the compiled ceiling. The runtime capacity
  // can be lowered (never raised past the ceiling) live from the optional
  // `number` entity below, e.g. from ESPHome's web_server with `auth:` set -
  // a lightweight authenticated portal to inspect/tune the buffer without
  // reflashing, without this component running its own HTTP server.
  // Both setters only update the numbers here; the actual quota
  // recalculation (and, if the queue is already non-empty, trimming
  // whatever no longer fits) happens in recompute_buffer_quotas_(), called
  // from here ONLY once setup_done_ is true. Before that (still applying the
  // compiled YAML defaults, before Radio::setup() has run) the outbox is
  // always empty anyway, so there is nothing to trim yet - recompute_buffer_
  // quotas_() runs once explicitly at the end of setup() instead, by which
  // point forward_meters/buffer_priority have actually been parsed.
  void set_mqtt_outbox_max_capacity(size_t max_capacity) {
    this->mqtt_outbox_max_capacity_ = max_capacity;
    if (this->mqtt_outbox_capacity_ > max_capacity) this->mqtt_outbox_capacity_ = max_capacity;
    if (this->setup_done_) this->recompute_buffer_quotas_();
  }
  void set_mqtt_outbox_capacity(size_t capacity) {
    this->mqtt_outbox_capacity_ = (capacity > this->mqtt_outbox_max_capacity_) ? this->mqtt_outbox_max_capacity_ : capacity;
    if (this->setup_done_) this->recompute_buffer_quotas_();
  }
  size_t get_mqtt_outbox_capacity() const { return this->mqtt_outbox_capacity_; }
  size_t get_mqtt_outbox_max_capacity() const { return this->mqtt_outbox_max_capacity_; }
  // Lifetime count of frames dropped (buffer full, heap safety valve, or
  // trimmed by a capacity/quota shrink). Exposed so the optional
  // buffer_capacity number: entity can report how many of ITS OWN action's
  // drops just happened, by diffing this before/after control() - see
  // wmbus_buffer_capacity_number.h.
  uint32_t get_mqtt_outbox_dropped_total() const { return this->mqtt_outbox_dropped_total_; }
  // mqtt_buffer_size: auto - re-evaluate the ceiling periodically from free
  // heap instead of a fixed compiled number. See mqtt_outbox.cpp for the
  // sizing formula and its (documented) limits.
  void set_mqtt_outbox_auto(bool auto_size) { this->mqtt_outbox_auto_ = auto_size; }
  bool get_mqtt_outbox_auto() const { return this->mqtt_outbox_auto_; }

  // buffer_priority (YAML): "<meter_id>:<weight>,..." - only meaningful once
  // forward_meters is a non-empty whitelist. Parsed in setup(); see
  // recompute_buffer_quotas_() in mqtt_outbox.cpp for how weights become
  // per-meter quotas that always sum to exactly the current capacity.
  void set_buffer_priority_csv(const std::string &csv) { this->buffer_priority_csv_ = csv; }

  // Optional entities wired in from the sensor:/number: YAML platforms
  // (components/wmbus_radio/sensor, components/wmbus_radio/number). All
  // nullptr (and simply not updated) when the user has not declared them.
  void set_buffer_depth_sensor(sensor::Sensor *s) { this->buffer_depth_sensor_ = s; }
  void set_buffer_dropped_sensor(sensor::Sensor *s) { this->buffer_dropped_sensor_ = s; }
  void set_buffer_dropped_last_outage_sensor(sensor::Sensor *s) { this->buffer_dropped_last_outage_sensor_ = s; }
  void set_buffer_oldest_age_sensor(sensor::Sensor *s) { this->buffer_oldest_age_sensor_ = s; }
  void set_buffer_capacity_number(number::Number *n) { this->buffer_capacity_number_ = n; }

  // Optional whitelist limiting which meters reach telegram_topic. Meters are
  // provided as a CSV string in YAML (list is joined in python). An empty list
  // forwards every decoded frame, which is the pre-existing behaviour.
  void set_forward_meters_csv(const std::string &csv) { this->forward_meters_csv_ = csv; }
  // True when the list was inherited from highlight_meters (forward_meters: true).
  // Only affects logging, so a later edit to highlight_meters that silently changes
  // what gets published is traceable in the boot log.
  void set_forward_meters_inherited(bool inherited) { this->forward_meters_inherited_ = inherited; }

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
  void set_diag_meter_stats_all(bool enabled) { this->diag_meter_stats_all_ = enabled; }
  void add_config_warning(const std::string &warning) { this->config_warnings_.push_back(warning); }
  void set_diag_publish_suggestion(bool enabled) { this->diag_publish_suggestion_ = enabled; }
  void set_diag_summary_interval_ms(uint32_t interval_ms) {
    // Keep it sane: minimum 5s
    this->diag_summary_interval_ms_ = interval_ms < 5000 ? 5000 : interval_ms;
  }
  void set_diag_publish_summary_15min(bool enabled) { this->diag_publish_summary_15min_ = enabled; }
  void set_diag_publish_summary_60min(bool enabled) { this->diag_publish_summary_60min_ = enabled; }
  void set_diag_publish_summary_highlight_meters(bool enabled) { this->diag_publish_summary_highlight_meters_ = enabled; }
  void set_sx1276_busy_ether_mode(SX1276BusyEtherMode mode) { this->sx1276_busy_ether_mode_ = mode; }
  void set_sx1262_yaml_sanity(bool has_tcxo, bool dio2_rf_switch, bool long_gfsk_packets, const std::string &rx_gain) {
    this->sx1262_yaml_sanity_configured_ = true;
    this->sx1262_yaml_has_tcxo_ = has_tcxo;
    this->sx1262_yaml_dio2_rf_switch_ = dio2_rf_switch;
    this->sx1262_yaml_long_gfsk_packets_ = long_gfsk_packets;
    this->sx1262_yaml_rx_gain_ = rx_gain;
  }
  void set_sx1262_rf_sw_pin_configured(bool configured) { this->sx1262_yaml_rf_sw_pin_ = configured; }
  void set_cc1101_yaml_sanity(bool gdo0_configured, bool gdo2_configured) {
    this->cc1101_yaml_sanity_configured_ = true;
    this->cc1101_yaml_gdo0_ = gdo0_configured;
    this->cc1101_yaml_gdo2_ = gdo2_configured;
  }
  // Built by __init__.py, which is the only place that knows the schema
  // defaults. Logged verbatim so the driver never has to restate a default.
  void add_config_report_line(const std::string &line) { this->config_report_.push_back(line); }
  void set_sx1276_yaml_sanity(bool tcxo_pin_configured) {
    this->sx1276_yaml_sanity_configured_ = true;
    this->sx1276_yaml_tcxo_pin_configured_ = tcxo_pin_configured;
  }
  void set_listen_mode_filter_after_parse(bool enabled) { this->listen_mode_filter_after_parse_ = enabled; }
  void set_use_noise_floor_threshold(bool enabled) { this->use_noise_floor_threshold_ = enabled; }
  void set_noise_floor_margin_db(int32_t db) { this->noise_floor_margin_db_ = db; }
  void set_diagnostic_mode_str(const std::string &mode) { this->diag_mode_str_ = mode; }
  void set_diagnostic_meter_stats_str(const std::string &mode) { this->meter_stats_str_ = mode; }
  void set_receiver_task_stack_size(uint32_t stack_size) {
    // This configures the dedicated radio_recv FreeRTOS task created by
    // wmbus_radio. It does NOT change ESPHome's main loop task stack.
    //
    // Why this exists: some boards can run older builds on the default 3 KB
    // receiver stack, but overflow on newer builds with heavier diagnostics.
    // Keeping it as a YAML option avoids board-specific branches and lets the
    // user raise the stack only where needed.
    this->receiver_task_stack_size_ = stack_size < 2048 ? 2048 : stack_size;
  }

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
  // Stack for the dedicated radio_recv task. Default stays at 3 KB so existing
  // configs behave exactly as before unless the user overrides it in YAML.
  uint32_t receiver_task_stack_size_{3 * 1024};

  // Default false = legacy/stable behavior: filter listen_mode by preliminary
  // raw packet mode before running the full parser. True = experimental behavior:
  // parse first, then filter by parser/CRC-selected final mode.
  bool listen_mode_filter_after_parse_{false};

  std::vector<std::function<void(Frame *)>> handlers_;

  // Per-meter reception statistics (only tracked for highlight_meters IDs)
  struct MeterStats {
    // Meter ID exactly as the reception log prints it: eight decimal digits for
    // a BCD meter, eight hex digits for one whose A-field is not BCD. Stored
    // rather than derived from the map key, because the key holds the raw
    // A-field value and rendering that as decimal produces a number that
    // belongs to no meter.
    char id_str[9]{};
    uint32_t last_seen_ms{0};      // millis() when last packet was received
    uint32_t last_interval_ms{0};  // elapsed ms since previous packet (0 = first seen)
    uint32_t interval_sum_ms{0};   // cumulative sum for average interval
    uint32_t interval_n{0};        // number of intervals recorded
    uint32_t count{0};             // total packets received (lifetime)
    // RSSI of the last packet that carried a measurement. Frames the
    // transceiver could not measure (-127 sentinel) leave this untouched, so it
    // always holds a real reading; 0 means "never measured", the same value
    // win_avg_rssi already publishes for a window with no samples.
    int32_t  rssi_last{0};
    int32_t  rssi_sum{0};          // cumulative RSSI sum (measured samples only)
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

    // Independent 60-minute window counters — reset only at summary_60min,
    // NOT at summary_15min. Fixes count_window showing only last 15min of data.
    uint32_t count_window_60min{0};
    int32_t  rssi_sum_window_60min{0};
    uint32_t rssi_n_window_60min{0};
    uint32_t interval_sum_window_60min_ms{0};
    uint32_t interval_n_window_60min{0};
  };
  // Key encodes both meter_id and link mode: (meter_id << 8) | (uint8_t)LinkMode.
  // This keeps T1 and C1 statistics separate for dual-mode meters
  // (e.g. a device that transmits the same ID on both T1 and C1).
  std::unordered_map<uint64_t, MeterStats> highlight_meter_stats_{};

  // Always-on radio health pulse + ESP-side meter flags. Published every
  // HEALTH_INTERVAL_MS_ regardless of diagnostic_mode, with retain=false (a
  // liveness signal must never become a retained tombstone). The pulse carries
  // proof that the RX path is alive (sec_since_last_rx), not just that the main
  // loop ticks.
  std::string health_topic_{};
  std::string meters_topic_{};
  uint32_t rx_total_lifetime_{0};   // monotonic count of received (filtered) frames
  uint32_t last_rx_ms_{0};          // millis() of the last received frame
  bool any_rx_{false};              // false until the first frame is received
  uint32_t last_health_ms_{0};      // last health/meters publish (0 = publish ASAP)
  static constexpr uint32_t HEALTH_INTERVAL_MS_ = 60000;
  void maybe_publish_health_(uint32_t now_ms);

  // Optional RAW forwarding / target forwarding.
  std::string telegram_topic_{};
  std::string rx_topic_{};
  std::string rssi_topic_{};
  uint32_t rx_boot_id_{0};
  uint32_t rx_publish_seq_{0};
  std::string target_meter_id_str_{};
  uint32_t target_meter_id_{0};
  bool target_meter_enabled_{false};
  std::string target_topic_{};
  bool target_log_{true};
  bool publish_radio_raw_{false};
  bool publish_rssi_{false};

  // Per-topic QoS, see the set_*_qos() setters above.
  uint8_t telegram_qos_{0};
  uint8_t rssi_qos_{0};
  uint8_t health_qos_{0};
  uint8_t diag_qos_{0};
  uint8_t rx_qos_{1};

  // RAM store-and-forward outbox. Holds fully-serialized MQTT messages
  // (already-built topic/payload/qos/retain) rather than raw Frame objects,
  // so buffering works the same way for the telegram publish and its /rx
  // metadata companion without either one needing to know it might be
  // deferred. See mqtt_outbox.cpp.
  // One queued MQTT publish. topic + payload bytes are allocated on the enqueue
  // path (mqtt_outbox.cpp) through esphome::RAMAllocator<char>, whose default
  // policy is "PSRAM first, fall back to internal heap". So on a board with
  // PSRAM the buffer is bounded by PSRAM, not the ~40 KB internal-heap
  // reserve, and on a board without PSRAM the bytes land in internal heap
  // exactly as before. Move-only: the two buffers are owned and freed on
  // destruction, so std::deque pop_front/erase/clear release them with no
  // manual bookkeeping at the call sites.
  struct OutboxMsg {
    char *topic{nullptr};    // NUL-terminated
    char *payload{nullptr};  // NOT NUL-terminated; length in payload_len
    uint32_t enqueued_ms{0};
    // 0 = not meter-specific (should not currently happen: every call site
    // that enqueues is meter-specific). See meter_bucket_key() in
    // meter_filter.h - a real key is always non-zero (bit 32/33 tag).
    uint64_t meter_key{0};
    uint16_t payload_len{0};  // MQTT payloads here are a few hundred bytes; 64 KiB cap is plenty
    uint8_t qos{0};
    bool retain{false};

    OutboxMsg() = default;
    OutboxMsg(const OutboxMsg &) = delete;
    OutboxMsg &operator=(const OutboxMsg &) = delete;
    OutboxMsg(OutboxMsg &&o) noexcept { this->steal_(o); }
    OutboxMsg &operator=(OutboxMsg &&o) noexcept {
      if (this != &o) {
        this->free_();
        this->steal_(o);
      }
      return *this;
    }
    ~OutboxMsg() { this->free_(); }

   private:
    // deallocate() in RAMAllocator is a plain free(), and heap_caps_malloc'd
    // blocks (internal or PSRAM) are freed with the ordinary free(), so this
    // needs no allocator instance.
    void free_() {
      ::free(this->topic);
      ::free(this->payload);
      this->topic = nullptr;
      this->payload = nullptr;
    }
    void steal_(OutboxMsg &o) {
      this->topic = o.topic;
      this->payload = o.payload;
      this->enqueued_ms = o.enqueued_ms;
      this->meter_key = o.meter_key;
      this->payload_len = o.payload_len;
      this->qos = o.qos;
      this->retain = o.retain;
      o.topic = nullptr;
      o.payload = nullptr;
      o.payload_len = 0;
    }
  };
  std::deque<OutboxMsg> mqtt_outbox_{};
  // OPT-IN: 0 = no buffering, which is this project's behaviour from before the
  // outbox existed (publish when connected, drop when not). A store-and-forward
  // buffer changes MQTT delivery semantics - a reconnect replays a burst of
  // telegrams whose reception time is minutes old - and costs internal heap on
  // boards without PSRAM, so it must never switch itself on for an existing
  // installation that did not ask for it. mqtt_buffer_size in YAML is what
  // turns it on; these defaults deliberately match cv.Optional(default=0)
  // there, so the C++ side and the schema cannot drift apart.
  size_t mqtt_outbox_capacity_{0};      // current effective cap (runtime-adjustable, <= max)
  size_t mqtt_outbox_max_capacity_{0};  // ceiling: fixed (mqtt_buffer_size in YAML) or, in
                                         // auto mode, the last value computed from free heap
  bool mqtt_outbox_auto_{false};
  uint32_t mqtt_outbox_queued_total_{0};    // lifetime count of frames that ever entered the buffer
  uint32_t mqtt_outbox_dropped_total_{0};   // lifetime count dropped because the buffer was full
                                             // OR refused by the free-heap safety valve
  // Per-outage drop accounting: all three reset the moment the MQTT link
  // goes down (a new outage begins), so buffer_dropped_last_outage always
  // reads "drops caused by the current, or most recent, broker outage".
  // The lifetime mqtt_outbox_dropped_total_ above is never reset.
  uint32_t mqtt_outbox_dropped_this_outage_{0};
  uint32_t mqtt_outbox_refused_heap_this_outage_{0};  // subset of the above refused at the door by the 40 KB heap valve
  // Per-meter drops this outage, keyed by the printable meter id (raw A-field
  // when available, else the BCD id) - i.e. the value the receive log prints
  // as id:XXXXXXXX. 30s stats line only.
  std::unordered_map<uint32_t, uint32_t> outbox_drop_by_meter_{};
  bool outbox_was_connected_{true};        // MQTT link state at the previous stats tick, to catch the edge into an outage
  void note_outbox_drop_(uint32_t display_id, bool refused_heap);
  // Resets the per-outage drop accounting on the connected->disconnected edge.
  // Called from BOTH the 1 Hz sampler and the frame path, so drops counted in
  // the gap before the sampler notices are no longer wiped afterwards.
  void note_outbox_link_state_(bool connected);
  uint32_t last_outbox_stats_ms_{0};
  uint32_t last_outbox_stats_log_ms_{0};   // 30s periodic "MQTT outbox stats" INFO line
  uint32_t last_outbox_autosize_ms_{0};
  uint32_t last_outbox_heap_warning_ms_{0};
  // Sampled free-heap figures for the safety valve in enqueue_or_publish_.
  // Held here rather than read per message because heap_caps_get_free_size()
  // takes the heap lock that the radio_recv task also needs mid-reception -
  // see the comment at the valve for the full reasoning.
  uint32_t last_heap_sample_ms_{0};
  size_t cached_free_internal_{0};
  size_t cached_free_psram_{0};
  // Largest CONTIGUOUS free internal block, sampled on the same 1 Hz tick.
  // Total free heap is not the quantity that protects WiFi/MQTT/TLS: those
  // need single contiguous buffers, and a long outage fills the outbox with
  // hundreds of small, variable-sized allocations. See the valve in
  // mqtt_outbox.cpp for why both figures are checked.
  size_t cached_largest_internal_{0};
  // Last value pushed to each buffer_* sensor, so update_outbox_stats_ only
  // republishes (and the sensor: framework only logs) on an actual change
  // instead of once a second forever. -1 = nothing published yet.
  float last_pub_depth_{-1.0f};
  float last_pub_dropped_{-1.0f};
  float last_pub_dropped_outage_{-1.0f};
  float last_pub_oldest_age_{-1.0f};
  uint32_t last_stats_log_dropped_{0};     // dropped_total at the last 30s stats line, to stay quiet when idle
  bool outbox_draining_{false};            // currently working through a backlog (flush logging)
  sensor::Sensor *buffer_depth_sensor_{nullptr};
  sensor::Sensor *buffer_dropped_sensor_{nullptr};
  sensor::Sensor *buffer_dropped_last_outage_sensor_{nullptr};
  sensor::Sensor *buffer_oldest_age_sensor_{nullptr};
  number::Number *buffer_capacity_number_{nullptr};
  // Optional runtime QoS controls (select: platform), see the setters above.
  select::Select *telegram_qos_select_{nullptr};
  select::Select *rx_qos_select_{nullptr};
  // Set true at the very end of setup(). Guards the two capacity setters
  // above: before this point the outbox is always empty (nothing has been
  // received yet) and forward_meters/buffer_priority have not been parsed
  // yet either, so recompute_buffer_quotas_() would see an empty whitelist
  // regardless of what YAML actually configured. setup() calls it explicitly
  // once, after parsing, instead.
  bool setup_done_{false};

  // buffer_priority (YAML "<id>:<weight>,..."), parsed once in setup() into
  // weights keyed by meter_bucket_key(). A meter in the whitelist without an
  // explicit entry here defaults to weight 1 - so leaving this unset entirely
  // means an equal split across all whitelisted meters, not "undefined".
  std::string buffer_priority_csv_{};
  std::unordered_map<uint64_t, uint32_t> buffer_priority_weights_{};

  // Per-meter outbox quotas. Empty = per-meter mode disabled (forward_meters
  // is empty, i.e. no whitelist): the outbox behaves as one shared FIFO
  // exactly as before this feature existed. Non-empty = every whitelisted
  // meter gets its own slice of mqtt_outbox_capacity_, proportional to its
  // weight, always summing to exactly the current capacity (largest-
  // remainder apportionment - see recompute_buffer_quotas_() in
  // mqtt_outbox.cpp for why that avoids needing weights to add up to
  // anything in particular).
  struct MeterQuota {
    uint64_t key{0};
    uint32_t weight{1};
    size_t quota{0};
    size_t count{0};  // currently queued for this meter (re-derived on every recompute)
  };
  std::vector<MeterQuota> mqtt_outbox_meter_quotas_{};
  MeterQuota *find_meter_quota_(uint64_t key);
  // Make exactly one slot in a full buffer. Victim = the meter furthest over
  // its buffer_priority quota (oldest frame first); if none is over quota,
  // the globally oldest frame. Lazy: only ever called when depth == capacity,
  // never to pre-emptively reserve empty space.
  void evict_one_for_priority_();
  // Rebuilds mqtt_outbox_meter_quotas_ from buffer_priority_weights_ + the
  // forward_meters whitelist + the current capacity, and trims any meter
  // that is now over its (possibly shrunk) quota. Called once at the end of
  // setup(), and again automatically by the capacity setters above once
  // setup_done_ is true (auto re-sizing, or the buffer_capacity number
  // entity). When the whitelist is empty this instead just enforces the
  // flat global capacity - the pre-existing single-shared-buffer behaviour.
  void recompute_buffer_quotas_();

  // If MQTT is connected, publish immediately (unchanged fast path). If not,
  // queue the message (dropping the oldest queued one first if already at
  // capacity - the oldest of the SAME meter's own messages, when per-meter
  // quotas are active) instead of discarding it outright. Also refuses to
  // grow the queue - regardless of the nominal capacity - once free heap
  // drops below a safety floor, so a manually-set large mqtt_buffer_size can
  // never be the thing that starves WiFi/MQTT/TLS of RAM. See
  // mqtt_outbox.cpp. meter_id/meter_id_raw identify which meter this message
  // is about (see meter_bucket_key()); every current call site is
  // meter-specific, so both are always meaningful today.
  void enqueue_or_publish_(const std::string &topic, const std::string &payload, uint8_t qos, bool retain,
                           uint32_t meter_id, uint32_t meter_id_raw);
  // Drains the outbox in FIFO order once MQTT is connected again. Called from
  // loop(); bounded per call so a large backlog cannot starve radio servicing.
  void flush_mqtt_outbox_();
  void update_outbox_stats_(uint32_t now_ms);
  // Pushes mqtt_outbox_capacity_ to buffer_capacity_number_ once at boot, if set.
  void publish_initial_buffer_state_();
  // mqtt_buffer_size: auto only. Re-evaluates the ceiling from currently free
  // heap every ~30s, so it adapts if free RAM changes after boot (more
  // diagnostics enabled, TLS handshake buffers, etc.) instead of freezing a
  // single boot-time guess.
  void maybe_reautosize_outbox_(uint32_t now_ms);
  // The formula behind both the periodic auto re-sizing above and the
  // one-shot boot log line. See mqtt_outbox.cpp for the reasoning and its
  // documented limits (internal heap only - see note there on PSRAM).
  size_t suggested_mqtt_outbox_capacity_() const;

  // Forwarding whitelist (sorted + deduplicated by the CSV parser, so lookups
  // can use binary search). Both empty means "forward everything".
  // _raw_ holds A-field values for meters whose ID is not BCD.
  std::string forward_meters_csv_{};
  std::vector<uint32_t> forward_meter_ids_{};
  std::vector<uint32_t> forward_meter_raw_ids_{};
  bool forward_meters_inherited_{false};

  // Highlight configuration
  std::string highlight_meters_csv_{};
  std::vector<uint32_t> highlight_meter_ids_{};
  std::vector<uint32_t> highlight_meter_raw_ids_{};
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

  // Diagnostics publishing is opt-in. Internal counters still run because radio-side
  // logic (for example SX1276 adaptive mode) depends on them even when MQTT output is off.
  bool diag_verbose_{false};
  // When false, per-packet payloads/logs omit the raw hex (much less spam)
  bool diag_publish_raw_{false};
  bool diag_publish_summary_{false};
  bool diag_publish_drop_events_{false};
  bool diag_publish_rx_path_events_{false};
  // If enabled, publish per-packet MQTT diagnostics only for ids present in
  // highlight_meters. Summary remains global and still counts everything.
  bool diag_publish_highlight_only_{false};
  bool diag_meter_stats_all_{false};
  std::vector<std::string> config_warnings_{};
  bool diag_publish_suggestion_{false};

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
    SB_S1_PRECHECK,
    SB_S1_MANCHESTER,
    SB_S1_L_FIELD,
    SB_S1_LENGTH_CHECK,
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
    // How many times the radio actually raised the data interrupt, i.e. the
    // receiver started something at all. Counted before any parsing, so it is
    // the only counter that separates "never triggered" from "triggered and
    // then lost downstream".
    uint32_t irq_fired{0};
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
  std::array<uint32_t, 4> diag_mode_total_{};
  std::array<uint32_t, 4> diag_mode_ok_{};
  std::array<uint32_t, 4> diag_mode_dropped_{};
  std::array<uint32_t, 4> diag_mode_crc_failed_{};
  std::array<int32_t, 4> diag_mode_rssi_ok_sum_{};
  std::array<uint32_t, 4> diag_mode_rssi_ok_n_{};
  std::array<int32_t, 4> diag_mode_rssi_drop_sum_{};
  std::array<uint32_t, 4> diag_mode_rssi_drop_n_{};

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
  std::array<uint32_t, 4> diag_15m_mode_total_{};
  std::array<uint32_t, 4> diag_15m_mode_ok_{};
  std::array<uint32_t, 4> diag_15m_mode_dropped_{};
  std::array<uint32_t, 4> diag_15m_mode_crc_failed_{};
  std::array<int32_t, 4> diag_15m_mode_rssi_ok_sum_{};
  std::array<uint32_t, 4> diag_15m_mode_rssi_ok_n_{};
  std::array<int32_t, 4> diag_15m_mode_rssi_drop_sum_{};
  std::array<uint32_t, 4> diag_15m_mode_rssi_drop_n_{};
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
  std::array<uint32_t, 4> diag_60min_mode_total_{};
  std::array<uint32_t, 4> diag_60min_mode_ok_{};
  std::array<uint32_t, 4> diag_60min_mode_dropped_{};
  std::array<uint32_t, 4> diag_60min_mode_crc_failed_{};
  std::array<int32_t, 4> diag_60min_mode_rssi_ok_sum_{};
  std::array<uint32_t, 4> diag_60min_mode_rssi_ok_n_{};
  std::array<int32_t, 4> diag_60min_mode_rssi_drop_sum_{};
  std::array<uint32_t, 4> diag_60min_mode_rssi_drop_n_{};
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

  // Ambient noise floor, sampled while the receiver sits idle. This exists
  // because every weak-start threshold in rf_runtime.cpp is currently derived
  // from recent_ok_rssi_avg_ - an average over SUCCESSFUL receptions only, so
  // aborting weak frames raises it, which raises the threshold, which aborts
  // more. The noise floor has no such loop: it is what the channel does when
  // we are not receiving.
  //
  // It also fixes a portability problem the bench made visible. A board with a
  // FEM reads roughly 10 dB hotter than the same chip without one (measured
  // 2026-08-25: two SX1262 boards, medians -59 vs -68, minima -79 vs -89), so
  // an absolute clamp like [-96, -86] dBm means something different on each.
  // "N dB above the floor" means the same thing everywhere.
  //
  // Minimum over a short ring rather than a mean: a sample taken while someone
  // else transmits is high, and a mean would let that pull the floor up.
  static constexpr size_t NOISE_FLOOR_SAMPLES = 16;
  int8_t noise_floor_ring_[NOISE_FLOOR_SAMPLES]{};
  uint8_t noise_floor_pos_{0};
  uint8_t noise_floor_count_{0};
  bool use_noise_floor_threshold_{false};
  int32_t noise_floor_margin_db_{6};
  void note_idle_rssi_(int rssi_dbm);
  // False until the ring has filled enough to mean anything.
  bool noise_floor_dbm_(int32_t *out) const;
  int32_t recent_ok_rssi_avg_{-80};
  bool recent_ok_rssi_valid_{false};

  static DropBucket bucket_for_reason_(const std::string &reason);
  static StageBucket bucket_for_stage_(const std::string &stage);
  bool meter_is_highlighted_(uint32_t meter_id, uint32_t meter_id_raw) const;
  void collect_radio_rx_diag_();
  uint32_t current_false_start_like_() const;
  uint32_t external_false_start_like_() const;
  bool sx1276_busy_ether_aggressive_now_() const;
  bool sx1276_busy_ether_severe_now_() const;
  bool should_abort_weak_partial_start_(int rssi_dbm, size_t bytes_read, bool is_c_mode) const;
  bool should_abort_t1_probe_start_(int rssi_dbm) const;
  bool should_attempt_raw_drain_(int rssi_dbm, size_t bytes_read, bool is_c_mode) const;
  std::string derived_target_topic_() const;
  bool forward_meter_allowed_(uint32_t meter_id, uint32_t meter_id_raw) const;
  // One formatting place for the whitelist state, logged from setup(), from the
  // delayed boot block and from dump_config().
  std::string forward_whitelist_summary_() const;
  void maybe_forward_frame_(Frame &frame, uint32_t meter_id, uint32_t meter_id_raw, const char *id_str,
                            const char *log_tag);
  void publish_rx_metadata_(Frame &frame, const char *id_str, uint32_t meter_id, uint32_t meter_id_raw);
  void maybe_publish_radio_raw_(Packet *packet, uint32_t now_ms);
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
  void publish_meter_window_batch_(const char *trigger, uint32_t elapsed_s, uint32_t now_ms);

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
  // Retained per-radio snapshot of the effective configuration on <diag>/config.
  // Consumed by the add-on's diagnostics panel; kept in sync with the boot log block.
  bool config_report_mqtt_pending_{true};

  // SX1262 YAML sanity state. These values are copied from YAML as-is.
  // They do not auto-configure board wiring; they only make risky settings visible in boot logs.
  bool sx1262_yaml_sanity_configured_{false};
  bool sx1262_yaml_has_tcxo_{false};
  bool sx1262_yaml_dio2_rf_switch_{true};
  bool sx1262_yaml_long_gfsk_packets_{false};
  std::string sx1262_yaml_rx_gain_{"boosted"};
  bool sx1262_yaml_warning_logged_{false};

  // SX1276 YAML sanity state. tcxo_pin is optional: normal SX1276 boards do not need it,
  // but boards such as LilyGO T3 V3.0 TCXO use GPIO12 as TCXO enable.
  bool sx1276_yaml_sanity_configured_{false};
  bool sx1276_yaml_tcxo_pin_configured_{false};
  bool sx1262_yaml_rf_sw_pin_{false};
  bool cc1101_yaml_sanity_configured_{false};
  bool cc1101_yaml_gdo0_{false};
  bool cc1101_yaml_gdo2_{false};
  std::vector<std::string> config_report_;

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

  // Silence required before NO_METERS_DETECTED may claim a wiring/config fault.
  // wM-Bus meters transmit tens of seconds to several minutes apart, so the
  // first summary window after boot is routinely empty on a healthy receiver.
  static constexpr uint32_t NO_METERS_MIN_UPTIME_MS_ = 5U * 60U * 1000U; // 5 minutes

  static constexpr uint32_t DIAG_15MIN_INTERVAL_MS_ = 15U * 60U * 1000U;
  static constexpr uint32_t DIAG_60MIN_INTERVAL_MS_ = 60U * 60U * 1000U;
  bool diag_publish_summary_15min_{false};
  bool diag_publish_summary_60min_{false};
  bool diag_publish_summary_highlight_meters_{false};
  std::string diag_topic_{};
  std::string diag_mode_str_{"off"};
  std::string meter_stats_str_{"off"};
};
} // namespace wmbus_radio
} // namespace esphome
