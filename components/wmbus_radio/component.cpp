#include "component.h"

#include "freertos/queue.h"
#include "freertos/task.h"

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <cinttypes>

// Optional: publish diagnostics via ESPHome MQTT if mqtt component is present.
#include "esphome/components/mqtt/mqtt_client.h"

#define WMBUS_PREAMBLE_SIZE (3)
#define WMBUS_MODE_C_PREAMBLE (0x54)
#define WMBUS_T1_LEN_PROBE_BYTES (18)
// SX1276-specific recovery path: if length cannot be derived from the initial
// probe, keep draining the raw stream until idle and let the packet parser make
// the final decision. Sized to cover long T1 telegrams (>255 B after decode).
#define WMBUS_RAW_DRAIN_MAX_BYTES (416)

#define ASSERT(expr, expected, before_exit)                                    \
  {                                                                            \
    auto result = (expr);                                                      \
    if (!!result != expected) {                                                \
      ESP_LOGE(TAG, "Assertion failed: %s -> %d", #expr, result);              \
      before_exit;                                                             \
      return;                                                                  \
    }                                                                          \
  }

#define ASSERT_SETUP(expr) ASSERT(expr, 1, this->mark_failed())

namespace esphome {
namespace wmbus_radio {
static const char *TAG = "wmbus";

// Logging policy for this component:
// - important user-facing INFO/WARN/ERROR messages may be concise bilingual EN/PL,
// - DEBUG/VERBOSE stays English to keep issue reports and grep output usable,
// - YAML/MQTT identifiers remain English-only because they form the stable technical API.

static const char *listen_mode_to_string_(ListenMode mode) {
  switch (mode) {
    case LISTEN_MODE_T1:
      return "T1 only";
    case LISTEN_MODE_C1:
      return "C1 only";
    case LISTEN_MODE_BOTH:
    default:
      return "T1+C1 (both, 3:1 bias)";
  }
}

static void parse_meter_id_csv_(const std::string &csv, std::vector<uint32_t> &out) {
  out.clear();
  if (csv.empty()) return;
  size_t i = 0;
  while (i < csv.size()) {
    // skip separators/whitespace
    while (i < csv.size() && (csv[i] == ',' || csv[i] == ';' || csv[i] == ' ' || csv[i] == '\t' || csv[i] == '\n' || csv[i] == '\r')) i++;
    if (i >= csv.size()) break;
    size_t j = i;
    while (j < csv.size() && csv[j] != ',' && csv[j] != ';' && csv[j] != ' ' && csv[j] != '\t' && csv[j] != '\n' && csv[j] != '\r') j++;
    std::string tok = csv.substr(i, j - i);
    // trim
    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
    if (!tok.empty()) {
      const char *s = tok.c_str();
      int base = 10;
      if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        s += 2;
        base = 16;
      }
      char *endp = nullptr;
      unsigned long v = std::strtoul(s, &endp, base);
      if (endp != s) {
        out.push_back((uint32_t) v);
      }
    }
    i = j;
  }
  if (!out.empty()) {
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  }
}


static bool radio_supports_preamble_retry_(const RadioTransceiver *radio) {
  return radio != nullptr && radio->supports_preamble_retry();
}

static bool radio_supports_unknown_size_raw_drain_(const RadioTransceiver *radio) {
  return radio != nullptr && radio->supports_unknown_size_raw_drain();
}

static bool radio_supports_weak_partial_start_abort_(const RadioTransceiver *radio) {
  return radio != nullptr && radio->supports_weak_partial_start_abort();
}


Radio::DropBucket Radio::bucket_for_reason_(const std::string &reason) {
  // Keep this stable: these strings come from Packet::set_drop_()
  if (reason == "too_short") return DB_TOO_SHORT;
  if (reason == "decode_failed") return DB_DECODE_FAILED;
  // Backwards compatible: older builds used dll_crc_strip_failed
  if (reason == "dll_crc_failed" || reason == "dll_crc_strip_failed") return DB_DLL_CRC_FAILED;
  if (reason == "unknown_preamble") return DB_UNKNOWN_PREAMBLE;
  if (reason == "l_field_invalid") return DB_L_FIELD_INVALID;
  if (reason == "unknown_link_mode") return DB_UNKNOWN_LINK_MODE;
  return DB_OTHER;
}

Radio::StageBucket Radio::bucket_for_stage_(const std::string &stage) {
  if (stage == "precheck") return SB_PRECHECK;
  if (stage == "t1_decode3of6") return SB_T1_DECODE3OF6;
  if (stage == "t1_l_field") return SB_T1_L_FIELD;
  if (stage == "t1_length_check") return SB_T1_LENGTH_CHECK;
  if (stage == "c1_precheck") return SB_C1_PRECHECK;
  if (stage == "c1_preamble") return SB_C1_PREAMBLE;
  if (stage == "c1_suffix") return SB_C1_SUFFIX;
  if (stage == "c1_l_field") return SB_C1_L_FIELD;
  if (stage == "c1_length_check") return SB_C1_LENGTH_CHECK;
  if (stage == "dll_crc_first") return SB_DLL_CRC_FIRST;
  if (stage == "dll_crc_mid") return SB_DLL_CRC_MID;
  if (stage == "dll_crc_final") return SB_DLL_CRC_FINAL;
  if (stage == "dll_crc_b1") return SB_DLL_CRC_B1;
  if (stage == "dll_crc_b2") return SB_DLL_CRC_B2;
  if (stage == "link_mode" || stage == "listen_mode_filter") return SB_LINK_MODE;
  return SB_OTHER;
}

void Radio::collect_radio_rx_diag_() {
  if (this->radio == nullptr) return;
  const uint32_t fifo_overrun = this->radio->take_fifo_overrun_count();
  this->diag_rx_path_.fifo_overrun += fifo_overrun;
  this->diag_15m_rx_path_.fifo_overrun += fifo_overrun;
  this->diag_60min_rx_path_.fifo_overrun += fifo_overrun;
}

// Returns the RSSI bucket index for abort RSSI distributions.
// [0] > -70   [1] -70..-79   [2] -80..-89   [3] -90..-99   [4] <= -100
static inline uint8_t rssi_abort_bucket_(int rssi_dbm) {
  if (rssi_dbm > -70)  return 0;
  if (rssi_dbm > -80)  return 1;
  if (rssi_dbm > -90)  return 2;
  if (rssi_dbm > -100) return 3;
  return 4;
}

uint32_t Radio::current_false_start_like_() const {
  return this->diag_rx_path_.preamble_read_failed +
         this->diag_rx_path_.payload_size_unknown +
         this->diag_rx_path_.weak_start_aborted +
         this->diag_rx_path_.probe_start_aborted +
         this->diag_rx_path_.raw_drain_skipped_weak;
}

bool Radio::sx1276_busy_ether_aggressive_now_() const {
  if (!radio_supports_weak_partial_start_abort_(this->radio)) return false;
  if (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::NORMAL) return false;
  if (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::AGGRESSIVE) return true;
  // ADAPTIVE: hold state is evaluated once per diagnostic summary window (evaluate_busy_ether_adaptive_).
  // millis() is a simple hardware timer read, safe from any context.
  return millis() < this->busy_ether_active_until_ms_;
}

bool Radio::sx1276_busy_ether_severe_now_() const {
  if (!radio_supports_weak_partial_start_abort_(this->radio)) return false;
  const uint32_t false_start_like = this->current_false_start_like_();
  const uint32_t drop_pct_window = (this->diag_total_ > 0 && this->diag_total_ > this->diag_ok_)
      ? (((this->diag_total_ - this->diag_ok_) * 100U) / this->diag_total_) : 0U;
  const uint32_t t1_sym_invalid_pct = (this->diag_t1_symbols_total_ > 0)
      ? ((this->diag_t1_symbols_invalid_ * 100U) / this->diag_t1_symbols_total_) : 0U;

  if (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::AGGRESSIVE) {
    // Raised thresholds: previous values (20/10/15) fired almost immediately in a typical
    // wMBus building due to normal transmission collisions, making AGGRESSIVE + SEVERE the
    // permanent state and killing distant-but-valid meters via RSSI threshold escalation.
    return false_start_like >= 60 || this->diag_rx_path_.preamble_read_failed >= 30 || drop_pct_window >= 25;
  }
  if (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::NORMAL) return false;

  if (false_start_like >= 140) return true;
  if (this->diag_rx_path_.preamble_read_failed >= 40 && this->diag_rx_path_.probe_start_aborted >= 40) return true;
  if (drop_pct_window >= 30) return true;
  if (t1_sym_invalid_pct >= 8 && false_start_like >= 30) return true;
  return false;
}

// Evaluates whether the ether is busy enough to activate aggressive filtering.
// Called once per diagnostic summary window, BEFORE the windowed counters are reset,
// so all current-window accumulations are visible.
// If conditions are met the adaptive hold timer is extended by 5 minutes; logging is
// emitted on state transitions so the user can observe adaptive behaviour via serial/MQTT.
void Radio::evaluate_busy_ether_adaptive_(uint32_t now_ms) {
  if (this->sx1276_busy_ether_mode_ != SX1276BusyEtherMode::ADAPTIVE) return;

  const uint32_t fsl = this->current_false_start_like_();
  const uint32_t drop_pct = (this->diag_total_ > 0 && this->diag_total_ > this->diag_ok_)
      ? (((this->diag_total_ - this->diag_ok_) * 100U) / this->diag_total_) : 0U;
  const uint32_t t1_sym_inv_pct = (this->diag_t1_symbols_total_ > 0)
      ? ((this->diag_t1_symbols_invalid_ * 100U) / this->diag_t1_symbols_total_) : 0U;

  const bool trigger =
      (this->diag_rx_path_.fifo_overrun > 0) ||
      // fsl alone is not enough — high fsl with low drop_pct means RF background noise
      // that does not actually harm reception. Require drop_pct >= 10 to confirm real collisions.
      (fsl >= 80 && drop_pct >= 10) ||
      (this->diag_rx_path_.preamble_read_failed >= 25 && this->diag_rx_path_.probe_start_aborted >= 20 && drop_pct >= 10) ||
      (drop_pct >= 20 && fsl >= 30) ||
      (t1_sym_inv_pct >= 5 && fsl >= 20 && drop_pct >= 10);

  // was_active tracks the last known state via busy_ether_was_active_ (persistent bool).
  // is_active_now reflects the current timer state.
  // These must be separate — was_active=true means we were active last window,
  // is_active_now=false means the hold has since expired. Using the timer for both
  // caused a dead code path where the passive transition event never fired.
  const bool was_active = this->busy_ether_was_active_;
  const bool is_active_now = (now_ms < this->busy_ether_active_until_ms_);

  if (trigger) {
    this->busy_ether_active_until_ms_ = now_ms + 300000; // 5-minute hold
    if (!was_active) {
      ESP_LOGW(TAG, "BusyEther [ADAPTIVE]: noisy window detected / wykryto zaszumione okno — activating / aktywacja na 5 min "
               "(fsl=%u drop_pct=%u t1_sym_inv_pct=%u preamble_fail=%u probe_abort=%u fifo_overrun=%u)",
               fsl, drop_pct, t1_sym_inv_pct,
               this->diag_rx_path_.preamble_read_failed,
               this->diag_rx_path_.probe_start_aborted,
               this->diag_rx_path_.fifo_overrun);
      // Publish busy_ether_changed event: passive -> active
      if (!this->diag_topic_.empty()) {
        auto *mqtt = esphome::mqtt::global_mqtt_client;
        if (mqtt != nullptr && mqtt->is_connected()) {
          char ev[256];
          snprintf(ev, sizeof(ev),
                   "{\"event\":\"busy_ether_changed\",\"chip\":\"SX1276\","
                   "\"state\":\"adaptive_active\","
                   "\"fsl\":%u,\"drop_pct\":%u}",
                   fsl, drop_pct);
          mqtt->publish(this->diag_topic_ + "/busy_ether_changed", std::string(ev), static_cast<uint8_t>(0), false);
        }
      }
      this->busy_ether_was_active_ = true;
    } else {
      ESP_LOGI(TAG, "BusyEther [ADAPTIVE]: hold extended / przedluzono hold (fsl=%u drop_pct=%u)", fsl, drop_pct);
    }
  } else if (was_active && !is_active_now) {
    // Hold has expired and no new trigger — transition to passive.
    ESP_LOGI(TAG, "BusyEther [ADAPTIVE]: hold expired / hold wygasl, returning to passive mode / powrot do trybu pasywnego (fsl=%u drop_pct=%u)", fsl, drop_pct);
    // Publish busy_ether_changed event: active -> passive
    if (!this->diag_topic_.empty()) {
      auto *mqtt = esphome::mqtt::global_mqtt_client;
      if (mqtt != nullptr && mqtt->is_connected()) {
        char ev[256];
        snprintf(ev, sizeof(ev),
                 "{\"event\":\"busy_ether_changed\",\"chip\":\"SX1276\","
                 "\"state\":\"adaptive_passive\","
                 "\"fsl\":%u,\"drop_pct\":%u}",
                 fsl, drop_pct);
        mqtt->publish(this->diag_topic_ + "/busy_ether_changed", std::string(ev), static_cast<uint8_t>(0), false);
      }
    }
    this->busy_ether_was_active_ = false;
  }
  // else: was_active && is_active_now && !trigger — still in hold, quiet window, don't extend, don't log.
}

bool Radio::should_abort_weak_partial_start_(int rssi_dbm, size_t bytes_read, bool is_c_mode) const {
  if (!radio_supports_weak_partial_start_abort_(this->radio)) return false;
  if (is_c_mode) return false;
  if (rssi_dbm <= -126 || rssi_dbm >= 0) return false;

  const uint32_t false_start_like = this->current_false_start_like_();
  int32_t recent_ok = this->recent_ok_rssi_valid_ ? this->recent_ok_rssi_avg_ : -80;
  int32_t threshold = recent_ok - 10;
  if (false_start_like >= 40 || this->diag_rx_path_.fifo_overrun > 0) threshold += 2;
  if (false_start_like >= 100) threshold += 2;
  if (this->sx1276_busy_ether_aggressive_now_()) threshold += 3;
  if (this->sx1276_busy_ether_severe_now_()) threshold += 2;
  // Clamp upper bound at -88 dBm: wMBus meters in a building routinely transmit at
  // -80..-90 dBm. The previous -78 limit killed distant-but-valid meters even without severe.
  threshold = std::clamp<int32_t>(threshold, -96, -88);
  if (rssi_dbm > threshold) return false;

  size_t max_partial = WMBUS_T1_LEN_PROBE_BYTES + 8;
  if (this->sx1276_busy_ether_aggressive_now_()) max_partial = WMBUS_T1_LEN_PROBE_BYTES + 4;
  if (this->sx1276_busy_ether_severe_now_()) max_partial = WMBUS_T1_LEN_PROBE_BYTES + 2;

  // Only for clearly partial / short starts. Once we already have a long body, keep the current path.
  if (bytes_read > max_partial) return false;
  return true;
}

bool Radio::should_abort_t1_probe_start_(int rssi_dbm) const {
  if (!radio_supports_weak_partial_start_abort_(this->radio)) return false;
  if (rssi_dbm <= -126 || rssi_dbm >= 0) return false;

  const uint32_t false_start_like = this->current_false_start_like_();
  if (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::NORMAL &&
      false_start_like < 20 && this->diag_rx_path_.fifo_overrun == 0) return false;

  int32_t recent_ok = this->recent_ok_rssi_valid_ ? this->recent_ok_rssi_avg_ : -80;
  int32_t threshold = recent_ok - 12;
  if (false_start_like >= 60 || this->diag_rx_path_.fifo_overrun > 0) threshold += 2;
  if (false_start_like >= 120) threshold += 2;
  if (this->sx1276_busy_ether_aggressive_now_()) threshold += 4;
  if (this->sx1276_busy_ether_severe_now_()) threshold += 3;
  // Clamp upper bound at -86 dBm: the previous -76 limit aborted T1 probe starts for
  // any meter weaker than -76 dBm once AGGRESSIVE+SEVERE was active (which was almost always).
  threshold = std::clamp<int32_t>(threshold, -96, -86);
  return rssi_dbm <= threshold;
}

bool Radio::should_attempt_raw_drain_(int rssi_dbm, size_t bytes_read, bool is_c_mode) const {
  if (!radio_supports_unknown_size_raw_drain_(this->radio)) return false;
  if (bytes_read == 0 || bytes_read >= WMBUS_RAW_DRAIN_MAX_BYTES) return false;
  if (is_c_mode) return true;

  if (this->should_abort_weak_partial_start_(rssi_dbm, bytes_read, is_c_mode)) return false;
  if (bytes_read <= (WMBUS_T1_LEN_PROBE_BYTES + 4) && this->should_abort_t1_probe_start_(rssi_dbm)) return false;

  const int32_t recent_ok = this->recent_ok_rssi_valid_ ? this->recent_ok_rssi_avg_ : -80;
  if (this->sx1276_busy_ether_severe_now_()) {
    if (bytes_read <= (WMBUS_T1_LEN_PROBE_BYTES + 16)) return false;
    if (rssi_dbm <= (recent_ok - 8)) return false;
  } else if (this->sx1276_busy_ether_aggressive_now_()) {
    if (bytes_read <= (WMBUS_T1_LEN_PROBE_BYTES + 10) && rssi_dbm <= (recent_ok - 6)) return false;
  }

  return true;
}

std::string Radio::derived_target_topic_() const {
  if (!this->target_topic_.empty()) return this->target_topic_;
  if (!this->target_meter_enabled_) return {};

  char id_buf[9];
  snprintf(id_buf, sizeof(id_buf), "%08u", (unsigned) this->target_meter_id_);

  if (!this->telegram_topic_.empty()) {
    const std::string suffix = "/telegram";
    if (this->telegram_topic_.size() > suffix.size() &&
        this->telegram_topic_.compare(this->telegram_topic_.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return this->telegram_topic_.substr(0, this->telegram_topic_.size() - suffix.size()) + "/target_" + id_buf;
    }
  }

  if (!this->diag_topic_.empty()) {
    return this->diag_topic_ + "/target_" + id_buf;
  }

  return {};
}

void Radio::maybe_forward_frame_(Frame &frame, uint32_t meter_id, const char *id_str, const char *log_tag) {
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  std::string hex;
  const bool want_all = !this->telegram_topic_.empty();
  const bool want_target = this->target_meter_enabled_ && meter_id == this->target_meter_id_;
  if (!want_all && !want_target) return;

  hex = frame.as_hex();
  if (want_all) {
    mqtt->publish(this->telegram_topic_, hex);
  }

  if (want_target) {
    if (this->target_log_) {
      ESP_LOGI(log_tag != nullptr ? log_tag : TAG, "TARGET %s caught / przechwycono RSSI=%d len=%u",
               id_str != nullptr ? id_str : "????????",
               (int) frame.rssi(),
               (unsigned) frame.data().size());
    }
    const std::string topic = this->derived_target_topic_();
    if (!topic.empty()) {
      mqtt->publish(topic, hex);
    }
  }
}

bool Radio::meter_is_highlighted_(uint32_t meter_id) const {
  return meter_id != 0 && !this->highlight_meter_ids_.empty() &&
         std::binary_search(this->highlight_meter_ids_.begin(), this->highlight_meter_ids_.end(), meter_id);
}

bool Radio::should_publish_packet_event_(const Packet *packet) const {
  if (packet == nullptr || !this->diag_publish_drop_events_) return false;
  if (!this->diag_publish_highlight_only_ || this->highlight_meter_ids_.empty()) return true;

  uint32_t meter_id = 0;
  if (!packet->try_get_meter_id(meter_id)) return false;
  return this->meter_is_highlighted_(meter_id);
}

void Radio::publish_rx_path_event_(const char *event, const char *stage, const char *detail, int rssi) {
  if (!this->diag_publish_rx_path_events_) return;
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected() || this->diag_topic_.empty()) return;

  const uint32_t now_ms = (uint32_t) esphome::millis();
  const char *listen_mode = (this->radio != nullptr)
                                ? listen_mode_to_string_(this->radio->get_listen_mode())
                                : "unknown";

  char payload[448];
  if (detail != nullptr && detail[0] != '\0') {
    snprintf(payload, sizeof(payload),
             "{\"event\":\"%s\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"stage\":\"%s\",\"rssi\":%d,\"detail\":\"%s\"}",
             event, (unsigned long) now_ms, listen_mode, stage, rssi, detail);
  } else {
    snprintf(payload, sizeof(payload),
             "{\"event\":\"%s\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"stage\":\"%s\",\"rssi\":%d}",
             event, (unsigned long) now_ms, listen_mode, stage, rssi);
  }
  mqtt->publish(this->diag_topic_, payload);
}

std::string Radio::diag_summary_topic_() const {
  if (this->diag_topic_.empty()) return {};
  return this->diag_topic_ + "/summary";
}

std::string Radio::diag_summary_15min_topic_() const {
  if (this->diag_topic_.empty()) return {};
  return this->diag_topic_ + "/summary_15min";
}

std::string Radio::diag_summary_60min_topic_() const {
  if (this->diag_topic_.empty()) return {};
  return this->diag_topic_ + "/summary_60min";
}

std::string Radio::diag_suggestion_topic_() const {
  if (this->diag_topic_.empty()) return {};
  return this->diag_topic_ + "/suggestion";
}

// Publish a suggestion event, throttled to once per hour per code.
// Suggestions are not retained — they are one-time actionable hints.
// yaml_snippet is a ready-to-copy YAML fragment the user can paste directly.
static void publish_suggestion_(esphome::mqtt::MQTTClientComponent *mqtt,
                                const std::string &topic,
                                std::unordered_map<std::string, uint32_t> &last_ms,
                                uint32_t now_ms,
                                uint32_t throttle_ms,
                                const char *chip,
                                const char *code,
                                const char *yaml_key,
                                const char *suggested_value,
                                const char *yaml_snippet,
                                const char *hint_en,
                                const char *hint_pl) {
  if (topic.empty()) return;
  auto it = last_ms.find(code);
  if (it != last_ms.end() && (now_ms - it->second) < throttle_ms) return;
  last_ms[code] = now_ms;

  char payload[640];
  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"suggestion\","
           "\"chip\":\"%s\","
           "\"code\":\"%s\","
           "\"yaml_key\":\"%s\","
           "\"suggested_value\":\"%s\","
           "\"yaml_snippet\":\"%s\","
           "\"hint_en\":\"%s\","
           "\"hint_pl\":\"%s\""
           "}",
           chip, code, yaml_key, suggested_value, yaml_snippet, hint_en, hint_pl);

  mqtt->publish(topic, std::string(payload), static_cast<uint8_t>(0), false);
  ESP_LOGI("wmbus", "SUGGESTION / SUGESTIA [%s]: %s", code, hint_en);
}

void Radio::maybe_publish_suggestion_(uint32_t now_ms) {
  if (this->diag_topic_.empty()) return;
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  const std::string topic = this->diag_suggestion_topic_();
  const bool is_sx1276 = (this->radio != nullptr && strcmp(this->radio->get_name(), "SX1276") == 0);
  const char *chip = is_sx1276 ? "SX1276" : "SX1262";

  const uint32_t fsl = this->current_false_start_like_();
  const uint32_t total = this->diag_total_;
  const uint32_t drop_pct = (total > 0) ? (((total - this->diag_ok_) * 100U) / total) : 0U;
  const uint32_t t1_sym_inv_pct = (this->diag_t1_symbols_total_ > 0)
      ? ((this->diag_t1_symbols_invalid_ * 100U) / this->diag_t1_symbols_total_) : 0U;

  // ── STAGE 1: orientation ────────────────────────────────────────────────────
  // No packets at all — user may have a wiring/config problem.
  if (total == 0) {
    publish_suggestion_(mqtt, topic, this->last_suggestion_ms_, now_ms, SUGGESTION_THROTTLE_MS_,
        chip, "NO_METERS_DETECTED",
        "listen_mode", "t1",
        "listen_mode: t1",
        "No wM-Bus frames received. Check antenna, SPI pins, listen_mode (t1/c1/both) and radio_type.",
        "Brak odebranych ramek wM-Bus. Sprawdź antenę, piny SPI, listen_mode (t1/c1/both) i radio_type.");
    return; // nothing more to suggest until we have data
  }

  // Packets arriving but highlight_meters not configured — user doesn't know
  // which meter IDs they have. They need to check wmbusmeters first.
  // NOTE: check highlight_meter_ids_ (user config), not highlight_meter_stats_ (runtime),
  // because stats may be empty simply because the listed meters haven't been seen yet.
  if (this->highlight_meter_ids_.empty()) {
    publish_suggestion_(mqtt, topic, this->last_suggestion_ms_, now_ms, SUGGESTION_THROTTLE_MS_,
        chip, "ADD_HIGHLIGHT_METERS",
        "highlight_meters", "<meter_id>",
        "highlight_meters:\n  - \"<meter_id>\"",
        "Meters are being received. Check which IDs appear in wmbusmeters, then add them to highlight_meters to track per-meter reception quality.",
        "Liczniki są odbierane. Sprawdź w wmbusmeters jakie ID pojawiają się, następnie dodaj je do highlight_meters aby śledzić skuteczność odbioru per licznik.");
    return; // advanced suggestions only make sense once user knows their meters
  }

  // ── STAGE 2: diagnostic suggestions (highlight_meters configured) ───────────

  // SX1276: many false starts causing real losses — suggest rx_path_events.
  // Require drop_pct >= 10 (same threshold as adaptive trigger) to avoid false alarms
  // when high fsl is just RF background noise with no actual losses.
  // Skip if already enabled in YAML.
  if (is_sx1276 && fsl >= 80 && drop_pct >= 10 && !this->diag_publish_rx_path_events_) {
    publish_suggestion_(mqtt, topic, this->last_suggestion_ms_, now_ms, SUGGESTION_THROTTLE_MS_,
        chip, "ENABLE_RX_PATH_EVENTS",
        "diagnostic_publish_rx_path_events", "true",
        "diagnostic_publish_rx_path_events: true",
        "Many false starts with real packet losses (SX1276). Enable diagnostic_publish_rx_path_events: true to investigate the RX path.",
        "Dużo fałszywych startów z realnymi stratami (SX1276). Włącz diagnostic_publish_rx_path_events: true aby zbadać ścieżkę RX.");
  }

  // Weak signal — suggest drop_events + raw.
  // Skip if already enabled in YAML.
  if (drop_pct >= 40 && this->diag_rssi_ok_n_ > 0 && (!this->diag_publish_drop_events_ || !this->diag_publish_raw_)) {
    const int32_t avg_ok_rssi = this->diag_rssi_ok_sum_ / (int32_t) this->diag_rssi_ok_n_;
    if (avg_ok_rssi <= -85) {
      publish_suggestion_(mqtt, topic, this->last_suggestion_ms_, now_ms, SUGGESTION_THROTTLE_MS_,
          chip, "ENABLE_DROP_EVENTS_RAW",
          "diagnostic_publish_drop_events", "true",
          "diagnostic_publish_drop_events: true\ndiagnostic_publish_raw: true",
          "Many drops at weak RSSI. Enable diagnostic_publish_drop_events: true and diagnostic_publish_raw: true to analyze dropped frames.",
          "Dużo dropów przy słabym sygnale. Włącz diagnostic_publish_drop_events: true i diagnostic_publish_raw: true aby przeanalizować odrzucone ramki.");
    }
  }

  // SX1262: symbol errors — suggest checking cpu_frequency.
  // No YAML state to check — cpu_frequency is an ESPHome board setting, not a wmbus option.
  if (!is_sx1276 && t1_sym_inv_pct >= 3) {
    publish_suggestion_(mqtt, topic, this->last_suggestion_ms_, now_ms, SUGGESTION_THROTTLE_MS_,
        chip, "SX1262_SYMBOL_ERRORS",
        "cpu_frequency", "160MHz",
        "cpu_frequency: 160MHz",
        "SX1262 shows T1 symbol errors. Check cpu_frequency — 240MHz can cause EMI affecting reception. Use 160MHz.",
        "SX1262 pokazuje błędy symboli T1. Sprawdź cpu_frequency — 240MHz może powodować EMI wpływające na odbiór. Użyj 160MHz.");
  }

  // Quiet ether on SX1276 with adaptive — suggest considering normal mode.
  // Skip if already set to normal in YAML.
  if (is_sx1276 &&
      this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::ADAPTIVE &&
      now_ms > this->busy_ether_active_until_ms_ &&
      fsl < 20 && total >= 3) {
    publish_suggestion_(mqtt, topic, this->last_suggestion_ms_, now_ms, SUGGESTION_THROTTLE_MS_,
        chip, "QUIET_ETHER_ADAPTIVE_IDLE",
        "sx1276_busy_ether_mode", "normal",
        "sx1276_busy_ether_mode: normal",
        "RF environment looks quiet and adaptive has not activated. Consider sx1276_busy_ether_mode: normal if this is consistent.",
        "Eter wygląda spokojnie a adaptive nie aktywował się. Rozważ sx1276_busy_ether_mode: normal jeśli jest to konsekwentne.");
  }
}


void Radio::maybe_publish_diag_summary_(uint32_t now_ms) {
  if (!this->diag_publish_summary_) return;
  if (this->diag_topic_.empty()) return;
  if (this->last_diag_summary_ms_ == 0) {
    this->last_diag_summary_ms_ = now_ms;
    return;
  }
  uint32_t elapsed = now_ms - this->last_diag_summary_ms_;
  if (elapsed < this->diag_summary_interval_ms_) return;
  this->last_diag_summary_ms_ = now_ms;

  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  const char *listen_mode = (this->radio != nullptr)
                                ? listen_mode_to_string_(this->radio->get_listen_mode())
                                : "unknown";
  const uint32_t interval_s = elapsed / 1000U;

  char payload[2048];
  const uint32_t crc_failed = this->diag_dropped_by_bucket_[DB_DLL_CRC_FAILED];
  const uint32_t total = this->diag_total_;
  const uint32_t ok = this->diag_ok_;
  const uint32_t crc_fail_pct = (total == 0) ? 0 : (crc_failed * 100U) / total;
  const uint32_t drop_pct = (total == 0) ? 0 : (this->diag_dropped_ * 100U) / total;
  const uint32_t trunc_pct = (total == 0) ? 0 : (this->diag_truncated_ * 100U) / total;
  const int32_t avg_ok_rssi = (this->diag_rssi_ok_n_ == 0) ? 0 : (this->diag_rssi_ok_sum_ / (int32_t) this->diag_rssi_ok_n_);
  const int32_t avg_drop_rssi = (this->diag_rssi_drop_n_ == 0) ? 0 : (this->diag_rssi_drop_sum_ / (int32_t) this->diag_rssi_drop_n_);

  const uint8_t T1 = (uint8_t) LinkMode::T1;
  const uint8_t C1 = (uint8_t) LinkMode::C1;
  const uint32_t t1_total = this->diag_mode_total_[T1];
  const uint32_t c1_total = this->diag_mode_total_[C1];
  const uint32_t t1_ok = this->diag_mode_ok_[T1];
  const uint32_t c1_ok = this->diag_mode_ok_[C1];
  const uint32_t t1_drop = this->diag_mode_dropped_[T1];
  const uint32_t c1_drop = this->diag_mode_dropped_[C1];
  const uint32_t t1_crc = this->diag_mode_crc_failed_[T1];
  const uint32_t c1_crc = this->diag_mode_crc_failed_[C1];
  const uint32_t t1_per_pct = (t1_total == 0) ? 0 : (t1_drop * 100U) / t1_total;
  const uint32_t c1_per_pct = (c1_total == 0) ? 0 : (c1_drop * 100U) / c1_total;
  const uint32_t t1_crc_pct = (t1_total == 0) ? 0 : (t1_crc * 100U) / t1_total;
  const uint32_t c1_crc_pct = (c1_total == 0) ? 0 : (c1_crc * 100U) / c1_total;
  const int32_t t1_avg_ok_rssi = (this->diag_mode_rssi_ok_n_[T1] == 0) ? 0 : (this->diag_mode_rssi_ok_sum_[T1] / (int32_t) this->diag_mode_rssi_ok_n_[T1]);
  const int32_t c1_avg_ok_rssi = (this->diag_mode_rssi_ok_n_[C1] == 0) ? 0 : (this->diag_mode_rssi_ok_sum_[C1] / (int32_t) this->diag_mode_rssi_ok_n_[C1]);
  const int32_t t1_avg_drop_rssi = (this->diag_mode_rssi_drop_n_[T1] == 0) ? 0 : (this->diag_mode_rssi_drop_sum_[T1] / (int32_t) this->diag_mode_rssi_drop_n_[T1]);
  const int32_t c1_avg_drop_rssi = (this->diag_mode_rssi_drop_n_[C1] == 0) ? 0 : (this->diag_mode_rssi_drop_sum_[C1] / (int32_t) this->diag_mode_rssi_drop_n_[C1]);

  const uint32_t t1_sym_total = this->diag_t1_symbols_total_;
  const uint32_t t1_sym_invalid = this->diag_t1_symbols_invalid_;
  const uint32_t t1_sym_invalid_pct = (t1_sym_total == 0) ? 0 : (t1_sym_invalid * 100U) / t1_sym_total;
  const bool is_sx1276 = (this->radio != nullptr && strcmp(this->radio->get_name(), "SX1276") == 0);
  const int32_t ok_vs_drop_rssi_gap =
      (this->diag_rssi_ok_n_ == 0 || this->diag_rssi_drop_n_ == 0) ? 0 : (avg_ok_rssi - avg_drop_rssi);
  const uint32_t rx_false_start_like =
      this->diag_rx_path_.preamble_read_failed + this->diag_rx_path_.payload_size_unknown +
      this->diag_rx_path_.weak_start_aborted + this->diag_rx_path_.probe_start_aborted +
      this->diag_rx_path_.raw_drain_skipped_weak;
  const uint32_t raw_drain_recovery_pct =
      (this->diag_rx_path_.raw_drain_attempted == 0)
          ? 0
          : (this->diag_rx_path_.raw_drain_recovered * 100U) / this->diag_rx_path_.raw_drain_attempted;

  const uint32_t reasons_sum =
      this->diag_dropped_by_bucket_[DB_TOO_SHORT] +
      this->diag_dropped_by_bucket_[DB_DECODE_FAILED] +
      this->diag_dropped_by_bucket_[DB_DLL_CRC_FAILED] +
      this->diag_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE] +
      this->diag_dropped_by_bucket_[DB_L_FIELD_INVALID] +
      this->diag_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE] +
      this->diag_dropped_by_bucket_[DB_OTHER];
  const uint32_t reasons_sum_mismatch = (reasons_sum != this->diag_dropped_) ? 1U : 0U;

  const char *hint_code = "OK";
  const char *hint_en = "looks good";
  const char *hint_pl = "wygląda dobrze";
  if (total == 0) {
    hint_code = "NO_DATA";
    hint_en = "no packets received yet";
    hint_pl = "brak odebranych ramek";
  } else {
    if (c1_total > 0 && c1_ok == 0 && c1_crc == c1_total) {
      if (c1_avg_drop_rssi <= -95) {
        hint_code = "C1_WEAK_SIGNAL";
        hint_en = "C1 frames fail DLL CRC at very low RSSI; improve antenna/placement";
        hint_pl = "C1: CRC DLL nie przechodzi przy bardzo niskim RSSI; popraw antenę/pozycję";
      } else {
        hint_code = "C1_INTERFERENCE_OR_RX";
        hint_en = "C1 frames fail DLL CRC despite decent RSSI; check interference/RX settings";
        hint_pl = "C1: CRC DLL nie przechodzi mimo niezłego RSSI; sprawdź zakłócenia/ustawienia RX";
      }
    } else if (c1_total > 0 && c1_crc > 0 && c1_avg_ok_rssi >= -65 && c1_avg_drop_rssi >= -80) {
      hint_code = "C1_OVERLOAD_OR_MULTIPATH";
      hint_en = "C1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move antenna 0.5-2m, change polarization, or attenuate.";
      hint_pl = "C1: CRC pada mimo dobrego RSSI; możliwy przester odbiornika lub wielodrogowość. Odsuń antenę 0,5-2m, zmień polaryzację lub stłum sygnał.";
    } else if (t1_total > 0 && t1_crc > 0 && t1_avg_ok_rssi >= -65 && t1_avg_drop_rssi >= -80) {
      hint_code = "T1_OVERLOAD_OR_MULTIPATH";
      hint_en = "T1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move/rotate antenna or attenuate.";
      hint_pl = "T1: CRC pada mimo dobrego RSSI; możliwy przester lub wielodrogowość. Przestaw/obróć antenę lub stłum sygnał.";
    } else if (is_sx1276 &&
               drop_pct >= 40 &&
               avg_drop_rssi <= -90 &&
               avg_ok_rssi >= -82 &&
               ok_vs_drop_rssi_gap >= 10 &&
               (rx_false_start_like >= 40 || this->diag_rx_path_.raw_drain_attempted >= 20 || this->diag_rx_path_.fifo_overrun > 0)) {
      hint_code = "SX1276_BUSY_ETHER";
      hint_en = "SX1276 is picking up many very weak/partial T1 starts while valid packets are much stronger; this looks like dense ether with distant or overlapping meters, not just a bad antenna.";
      hint_pl = "SX1276 łapie dużo bardzo słabych/częściowych startów T1, podczas gdy poprawne pakiety są znacznie silniejsze; to wygląda na gęsty eter z dalekimi lub nakładającymi się licznikami, a nie tylko złą antenę.";
    } else if (drop_pct >= 40 &&
               avg_drop_rssi <= -90 &&
               avg_ok_rssi >= -82 &&
               ok_vs_drop_rssi_gap >= 10) {
      hint_code = "BUSY_ETHER_WEAK_TRASH";
      hint_en = "many dropped packets are far weaker than valid ones; the receiver likely hears lots of distant or overlapping neighbor meters.";
      hint_pl = "wiele odrzuconych ramek jest dużo słabszych niż poprawne; odbiornik prawdopodobnie słyszy dużo dalekich lub nakładających się liczników sąsiadów.";
    } else if (drop_pct >= 60 && avg_drop_rssi <= -92 && (ok == 0 || avg_ok_rssi <= -86)) {
      hint_code = "WEAK_SIGNAL";
      hint_en = "many drops at very low RSSI and weak valid packets; improve antenna/placement";
      hint_pl = "dużo dropów przy bardzo niskim RSSI i słabych poprawnych pakietach; popraw antenę/pozycję";
    } else if (t1_total > 0 && t1_sym_total >= 200 && t1_sym_invalid_pct >= 5) {
      hint_code = "T1_SYMBOL_ERRORS";
      hint_en = "T1 has many invalid 3-of-6 symbols; likely collisions, interference, or bit errors";
      hint_pl = "T1: dużo błędnych symboli 3-of-6; możliwe kolizje, zakłócenia lub błędy bitów";
    } else if (t1_total > 0 && t1_crc_pct >= 10 && t1_sym_invalid_pct < 2) {
      hint_code = "T1_BITFLIPS";
      hint_en = "T1 mostly decodes but often fails DLL CRC; likely occasional bitflips";
      hint_pl = "T1: dekoduje się, ale często pada CRC DLL; możliwe sporadyczne bitflipy";
    } else if (is_sx1276 &&
               drop_pct >= 20 &&
               (this->diag_rx_path_.preamble_read_failed >= 20 ||
                this->diag_rx_path_.payload_size_unknown >= 10)) {
      hint_code = "SX1276_RX_NOISY";
      hint_en = "SX1276 sees many false starts or undecodable starts; RX frontend likely needs tuning";
      hint_pl = "SX1276 ma dużo fałszywych startów lub nieustalonych początków ramek; ścieżka RX wymaga strojenia";
    } else if (ok > 0 && drop_pct <= 10) {
      hint_code = "GOOD";
      hint_en = "RF link looks stable";
      hint_pl = "łącze radiowe wygląda stabilnie";
    }
  }

  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"summary\"," 
           "\"interval_s\":%u,"
           "\"uptime_ms\":%lu,"
           "\"listen_mode\":\"%s\","
           "\"total\":%u,"
           "\"ok\":%u,"
           "\"truncated\":%u,"
           "\"dropped\":%u,"
           "\"crc_failed\":%u,"
           "\"crc_fail_pct\":%u,"
           "\"drop_pct\":%u,"
           "\"trunc_pct\":%u,"
           "\"avg_ok_rssi\":%d,"
           "\"avg_drop_rssi\":%d,"
           "\"t1\":{"
             "\"total\":%u,\"ok\":%u,\"dropped\":%u,\"per_pct\":%u,"
             "\"crc_failed\":%u,\"crc_pct\":%u,\"avg_ok_rssi\":%d,\"avg_drop_rssi\":%d,"
             "\"sym_total\":%u,\"sym_invalid\":%u,\"sym_invalid_pct\":%u"
           "},"
           "\"c1\":{"
             "\"total\":%u,\"ok\":%u,\"dropped\":%u,\"per_pct\":%u,"
             "\"crc_failed\":%u,\"crc_pct\":%u,\"avg_ok_rssi\":%d,\"avg_drop_rssi\":%d"
           "},"
           "\"dropped_by_reason\":{"
             "\"too_short\":%u,"
             "\"decode_failed\":%u,"
             "\"dll_crc_failed\":%u,"
             "\"unknown_preamble\":%u,"
             "\"l_field_invalid\":%u,"
             "\"unknown_link_mode\":%u,"
             "\"other\":%u"
           "},"
           "\"dropped_by_stage\":{"
             "\"precheck\":%u,"
             "\"t1_decode3of6\":%u,"
             "\"t1_l_field\":%u,"
             "\"t1_length_check\":%u,"
             "\"c1_precheck\":%u,"
             "\"c1_preamble\":%u,"
             "\"c1_suffix\":%u,"
             "\"c1_l_field\":%u,"
             "\"c1_length_check\":%u,"
             "\"dll_crc_first\":%u,"
             "\"dll_crc_mid\":%u,"
             "\"dll_crc_final\":%u,"
             "\"dll_crc_b1\":%u,"
             "\"dll_crc_b2\":%u,"
             "\"link_mode\":%u,"
             "\"other\":%u"
           "},"
           "\"rx_path\":{"
             "\"irq_timeout\":%u,"
             "\"preamble_read_failed\":%u,"
             "\"preamble_retry_recovered\":%u,"
             "\"t1_header_read_failed\":%u,"
             "\"payload_size_unknown\":%u,"
             "\"raw_drain_attempted\":%u,"
             "\"raw_drain_recovered\":%u,"
             "\"raw_drain_recovery_pct\":%u,"
             "\"raw_drain_bytes\":%u,"
             "\"payload_read_failed\":%u,"
             "\"queue_send_failed\":%u,"
             "\"fifo_overrun\":%u,"
             "\"weak_start_aborted\":%u,"
             "\"probe_start_aborted\":%u,"
             "\"raw_drain_skipped_weak\":%u,"
             "\"false_start_like\":%u,"
             "\"probe_abort_rssi\":{\"gt70\":%u,\"70_79\":%u,\"80_89\":%u,\"90_99\":%u,\"lt100\":%u},"
             "\"weak_abort_rssi\":{\"gt70\":%u,\"70_79\":%u,\"80_89\":%u,\"90_99\":%u,\"lt100\":%u}"
           "},"
           "\"reasons_sum\":%u,"
           "\"reasons_sum_mismatch\":%u,"
           "\"hint_code\":\"%s\","
           "\"hint_en\":\"%s\",\"hint_pl\":\"%s\","
           "\"busy_ether_state\":\"%s\""
           "}",
           (unsigned) interval_s,
           (unsigned long) now_ms,
           listen_mode,
           (unsigned) total,
           (unsigned) this->diag_ok_,
           (unsigned) this->diag_truncated_,
           (unsigned) this->diag_dropped_,
           (unsigned) crc_failed,
           (unsigned) crc_fail_pct,
           (unsigned) drop_pct,
           (unsigned) trunc_pct,
           (int) avg_ok_rssi,
           (int) avg_drop_rssi,
           (unsigned) t1_total,
           (unsigned) t1_ok,
           (unsigned) t1_drop,
           (unsigned) t1_per_pct,
           (unsigned) t1_crc,
           (unsigned) t1_crc_pct,
           (int) t1_avg_ok_rssi,
           (int) t1_avg_drop_rssi,
           (unsigned) t1_sym_total,
           (unsigned) t1_sym_invalid,
           (unsigned) t1_sym_invalid_pct,
           (unsigned) c1_total,
           (unsigned) c1_ok,
           (unsigned) c1_drop,
           (unsigned) c1_per_pct,
           (unsigned) c1_crc,
           (unsigned) c1_crc_pct,
           (int) c1_avg_ok_rssi,
           (int) c1_avg_drop_rssi,
           (unsigned) this->diag_dropped_by_bucket_[DB_TOO_SHORT],
           (unsigned) this->diag_dropped_by_bucket_[DB_DECODE_FAILED],
           (unsigned) this->diag_dropped_by_bucket_[DB_DLL_CRC_FAILED],
           (unsigned) this->diag_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE],
           (unsigned) this->diag_dropped_by_bucket_[DB_L_FIELD_INVALID],
           (unsigned) this->diag_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE],
           (unsigned) this->diag_dropped_by_bucket_[DB_OTHER],
           (unsigned) this->diag_dropped_by_stage_[SB_PRECHECK],
           (unsigned) this->diag_dropped_by_stage_[SB_T1_DECODE3OF6],
           (unsigned) this->diag_dropped_by_stage_[SB_T1_L_FIELD],
           (unsigned) this->diag_dropped_by_stage_[SB_T1_LENGTH_CHECK],
           (unsigned) this->diag_dropped_by_stage_[SB_C1_PRECHECK],
           (unsigned) this->diag_dropped_by_stage_[SB_C1_PREAMBLE],
           (unsigned) this->diag_dropped_by_stage_[SB_C1_SUFFIX],
           (unsigned) this->diag_dropped_by_stage_[SB_C1_L_FIELD],
           (unsigned) this->diag_dropped_by_stage_[SB_C1_LENGTH_CHECK],
           (unsigned) this->diag_dropped_by_stage_[SB_DLL_CRC_FIRST],
           (unsigned) this->diag_dropped_by_stage_[SB_DLL_CRC_MID],
           (unsigned) this->diag_dropped_by_stage_[SB_DLL_CRC_FINAL],
           (unsigned) this->diag_dropped_by_stage_[SB_DLL_CRC_B1],
           (unsigned) this->diag_dropped_by_stage_[SB_DLL_CRC_B2],
           (unsigned) this->diag_dropped_by_stage_[SB_LINK_MODE],
           (unsigned) this->diag_dropped_by_stage_[SB_OTHER],
           (unsigned) this->diag_rx_path_.irq_timeout,
           (unsigned) this->diag_rx_path_.preamble_read_failed,
           (unsigned) this->diag_rx_path_.preamble_retry_recovered,
           (unsigned) this->diag_rx_path_.t1_header_read_failed,
           (unsigned) this->diag_rx_path_.payload_size_unknown,
           (unsigned) this->diag_rx_path_.raw_drain_attempted,
           (unsigned) this->diag_rx_path_.raw_drain_recovered,
           (unsigned) raw_drain_recovery_pct,
           (unsigned) this->diag_rx_path_.raw_drain_bytes,
           (unsigned) this->diag_rx_path_.payload_read_failed,
           (unsigned) this->diag_rx_path_.queue_send_failed,
           (unsigned) this->diag_rx_path_.fifo_overrun,
           (unsigned) this->diag_rx_path_.weak_start_aborted,
           (unsigned) this->diag_rx_path_.probe_start_aborted,
           (unsigned) this->diag_rx_path_.raw_drain_skipped_weak,
           (unsigned) rx_false_start_like,
           // probe_abort_rssi buckets
           (unsigned) this->diag_rx_path_.probe_abort_rssi[0],
           (unsigned) this->diag_rx_path_.probe_abort_rssi[1],
           (unsigned) this->diag_rx_path_.probe_abort_rssi[2],
           (unsigned) this->diag_rx_path_.probe_abort_rssi[3],
           (unsigned) this->diag_rx_path_.probe_abort_rssi[4],
           // weak_abort_rssi buckets
           (unsigned) this->diag_rx_path_.weak_abort_rssi[0],
           (unsigned) this->diag_rx_path_.weak_abort_rssi[1],
           (unsigned) this->diag_rx_path_.weak_abort_rssi[2],
           (unsigned) this->diag_rx_path_.weak_abort_rssi[3],
           (unsigned) this->diag_rx_path_.weak_abort_rssi[4],
           (unsigned) reasons_sum,
           (unsigned) reasons_sum_mismatch,
           hint_code,
           hint_en,
           hint_pl,
           // busy_ether_state: reflects state BEFORE evaluating this window.
           // evaluate_busy_ether_adaptive_() runs after publish to access full window counters.
           // Use busy_ether_changed event for precise transition timestamps.
           (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::ADAPTIVE)
               ? (this->busy_ether_was_active_ ? "adaptive_active" : "adaptive_passive")
               : (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::AGGRESSIVE ? "aggressive" : "normal"));

  const std::string summary_topic = this->diag_summary_topic_();
  mqtt->publish(summary_topic, payload);
  ESP_LOGI(TAG, "DIAG summary / podsumowanie diag: topic=%s interval=%us uptime_ms=%lu listen_mode=%s total=%u ok=%u truncated=%u dropped=%u crc_failed=%u",
           summary_topic.c_str(), (unsigned) interval_s, (unsigned long) now_ms, listen_mode,
           (unsigned) total, (unsigned) this->diag_ok_,
           (unsigned) this->diag_truncated_, (unsigned) this->diag_dropped_, (unsigned) crc_failed);

  if (std::strcmp(hint_code, "OK") == 0 || std::strcmp(hint_code, "GOOD") == 0) {
    ESP_LOGI(TAG, "DIAG hint: %s | %s / %s", hint_code, hint_en, hint_pl);
  } else {
    ESP_LOGW(TAG, "DIAG hint: %s | %s / %s", hint_code, hint_en, hint_pl);
  }

  // Evaluate adaptive busy-ether state BEFORE resetting windowed counters —
  // this is the only point where all current-window accumulations are visible.
  this->evaluate_busy_ether_adaptive_(now_ms);

  // Publish suggestion event based on current window data (before reset).
  this->maybe_publish_suggestion_(now_ms);

  this->diag_total_ = 0;
  this->diag_ok_ = 0;
  this->diag_truncated_ = 0;
  this->diag_dropped_ = 0;
  this->diag_dropped_by_bucket_.fill(0);
  this->diag_dropped_by_stage_.fill(0);
  this->diag_rssi_ok_sum_ = 0;
  this->diag_rssi_ok_n_ = 0;
  this->diag_rssi_drop_sum_ = 0;
  this->diag_rssi_drop_n_ = 0;
  this->diag_mode_total_.fill(0);
  this->diag_mode_ok_.fill(0);
  this->diag_mode_dropped_.fill(0);
  this->diag_mode_crc_failed_.fill(0);
  this->diag_mode_rssi_ok_sum_.fill(0);
  this->diag_mode_rssi_ok_n_.fill(0);
  this->diag_mode_rssi_drop_sum_.fill(0);
  this->diag_mode_rssi_drop_n_.fill(0);
  this->diag_t1_symbols_total_ = 0;
  this->diag_t1_symbols_invalid_ = 0;
  this->diag_rx_path_ = {};
}


void Radio::maybe_publish_diag_15min_summary_(uint32_t now_ms) {
  if (!this->diag_publish_summary_) return;
  if (!this->diag_publish_summary_15min_) return;
  if (this->diag_topic_.empty()) return;
  if (this->last_diag_15min_summary_ms_ == 0) {
    this->last_diag_15min_summary_ms_ = now_ms;
    return;
  }
  uint32_t elapsed = now_ms - this->last_diag_15min_summary_ms_;
  if (elapsed < this->DIAG_15MIN_INTERVAL_MS_) return;
  this->last_diag_15min_summary_ms_ = now_ms;

  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  const char *listen_mode = (this->radio != nullptr)
                                ? listen_mode_to_string_(this->radio->get_listen_mode())
                                : "unknown";
  const uint32_t interval_s = elapsed / 1000U;

  char payload[2048];
  const uint32_t crc_failed = this->diag_15m_dropped_by_bucket_[DB_DLL_CRC_FAILED];
  const uint32_t total = this->diag_15m_total_;
  const uint32_t ok = this->diag_15m_ok_;
  const uint32_t crc_fail_pct = (total == 0) ? 0 : (crc_failed * 100U) / total;
  const uint32_t drop_pct = (total == 0) ? 0 : (this->diag_15m_dropped_ * 100U) / total;
  const uint32_t trunc_pct = (total == 0) ? 0 : (this->diag_15m_truncated_ * 100U) / total;
  const int32_t avg_ok_rssi = (this->diag_15m_rssi_ok_n_ == 0) ? 0 : (this->diag_15m_rssi_ok_sum_ / (int32_t) this->diag_15m_rssi_ok_n_);
  const int32_t avg_drop_rssi = (this->diag_15m_rssi_drop_n_ == 0) ? 0 : (this->diag_15m_rssi_drop_sum_ / (int32_t) this->diag_15m_rssi_drop_n_);

  const uint8_t T1 = (uint8_t) LinkMode::T1;
  const uint8_t C1 = (uint8_t) LinkMode::C1;
  const uint32_t t1_total = this->diag_15m_mode_total_[T1];
  const uint32_t c1_total = this->diag_15m_mode_total_[C1];
  const uint32_t t1_ok = this->diag_15m_mode_ok_[T1];
  const uint32_t c1_ok = this->diag_15m_mode_ok_[C1];
  const uint32_t t1_drop = this->diag_15m_mode_dropped_[T1];
  const uint32_t c1_drop = this->diag_15m_mode_dropped_[C1];
  const uint32_t t1_crc = this->diag_15m_mode_crc_failed_[T1];
  const uint32_t c1_crc = this->diag_15m_mode_crc_failed_[C1];
  const uint32_t t1_per_pct = (t1_total == 0) ? 0 : (t1_drop * 100U) / t1_total;
  const uint32_t c1_per_pct = (c1_total == 0) ? 0 : (c1_drop * 100U) / c1_total;
  const uint32_t t1_crc_pct = (t1_total == 0) ? 0 : (t1_crc * 100U) / t1_total;
  const uint32_t c1_crc_pct = (c1_total == 0) ? 0 : (c1_crc * 100U) / c1_total;
  const int32_t t1_avg_ok_rssi = (this->diag_15m_mode_rssi_ok_n_[T1] == 0) ? 0 : (this->diag_15m_mode_rssi_ok_sum_[T1] / (int32_t) this->diag_15m_mode_rssi_ok_n_[T1]);
  const int32_t c1_avg_ok_rssi = (this->diag_15m_mode_rssi_ok_n_[C1] == 0) ? 0 : (this->diag_15m_mode_rssi_ok_sum_[C1] / (int32_t) this->diag_15m_mode_rssi_ok_n_[C1]);
  const int32_t t1_avg_drop_rssi = (this->diag_15m_mode_rssi_drop_n_[T1] == 0) ? 0 : (this->diag_15m_mode_rssi_drop_sum_[T1] / (int32_t) this->diag_15m_mode_rssi_drop_n_[T1]);
  const int32_t c1_avg_drop_rssi = (this->diag_15m_mode_rssi_drop_n_[C1] == 0) ? 0 : (this->diag_15m_mode_rssi_drop_sum_[C1] / (int32_t) this->diag_15m_mode_rssi_drop_n_[C1]);

  const uint32_t t1_sym_total = this->diag_15m_t1_symbols_total_;
  const uint32_t t1_sym_invalid = this->diag_15m_t1_symbols_invalid_;
  const uint32_t t1_sym_invalid_pct = (t1_sym_total == 0) ? 0 : (t1_sym_invalid * 100U) / t1_sym_total;
  const bool is_sx1276 = (this->radio != nullptr && strcmp(this->radio->get_name(), "SX1276") == 0);
  const int32_t ok_vs_drop_rssi_gap =
      (this->diag_15m_rssi_ok_n_ == 0 || this->diag_15m_rssi_drop_n_ == 0) ? 0 : (avg_ok_rssi - avg_drop_rssi);
  const uint32_t rx_false_start_like =
      this->diag_15m_rx_path_.preamble_read_failed + this->diag_15m_rx_path_.payload_size_unknown +
      this->diag_15m_rx_path_.weak_start_aborted + this->diag_15m_rx_path_.probe_start_aborted +
      this->diag_15m_rx_path_.raw_drain_skipped_weak;
  const uint32_t raw_drain_recovery_pct =
      (this->diag_15m_rx_path_.raw_drain_attempted == 0)
          ? 0
          : (this->diag_15m_rx_path_.raw_drain_recovered * 100U) / this->diag_15m_rx_path_.raw_drain_attempted;

  const uint32_t reasons_sum =
      this->diag_15m_dropped_by_bucket_[DB_TOO_SHORT] +
      this->diag_15m_dropped_by_bucket_[DB_DECODE_FAILED] +
      this->diag_15m_dropped_by_bucket_[DB_DLL_CRC_FAILED] +
      this->diag_15m_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE] +
      this->diag_15m_dropped_by_bucket_[DB_L_FIELD_INVALID] +
      this->diag_15m_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE] +
      this->diag_15m_dropped_by_bucket_[DB_OTHER];
  const uint32_t reasons_sum_mismatch = (reasons_sum != this->diag_15m_dropped_) ? 1U : 0U;

  const char *hint_code = "OK";
  const char *hint_en = "looks good";
  const char *hint_pl = "wygląda dobrze";
  if (total == 0) {
    hint_code = "NO_DATA";
    hint_en = "no packets received yet";
    hint_pl = "brak odebranych ramek";
  } else {
    if (c1_total > 0 && c1_ok == 0 && c1_crc == c1_total) {
      if (c1_avg_drop_rssi <= -95) {
        hint_code = "C1_WEAK_SIGNAL";
        hint_en = "C1 frames fail DLL CRC at very low RSSI; improve antenna/placement";
        hint_pl = "C1: CRC DLL nie przechodzi przy bardzo niskim RSSI; popraw antenę/pozycję";
      } else {
        hint_code = "C1_INTERFERENCE_OR_RX";
        hint_en = "C1 frames fail DLL CRC despite decent RSSI; check interference/RX settings";
        hint_pl = "C1: CRC DLL nie przechodzi mimo niezłego RSSI; sprawdź zakłócenia/ustawienia RX";
      }
    } else if (c1_total > 0 && c1_crc > 0 && c1_avg_ok_rssi >= -65 && c1_avg_drop_rssi >= -80) {
      hint_code = "C1_OVERLOAD_OR_MULTIPATH";
      hint_en = "C1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move antenna 0.5-2m, change polarization, or attenuate.";
      hint_pl = "C1: CRC pada mimo dobrego RSSI; możliwy przester odbiornika lub wielodrogowość. Odsuń antenę 0,5-2m, zmień polaryzację lub stłum sygnał.";
    } else if (t1_total > 0 && t1_crc > 0 && t1_avg_ok_rssi >= -65 && t1_avg_drop_rssi >= -80) {
      hint_code = "T1_OVERLOAD_OR_MULTIPATH";
      hint_en = "T1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move/rotate antenna or attenuate.";
      hint_pl = "T1: CRC pada mimo dobrego RSSI; możliwy przester lub wielodrogowość. Przestaw/obróć antenę lub stłum sygnał.";
    } else if (is_sx1276 &&
               drop_pct >= 40 &&
               avg_drop_rssi <= -90 &&
               avg_ok_rssi >= -82 &&
               ok_vs_drop_rssi_gap >= 10 &&
               (rx_false_start_like >= 40 || this->diag_15m_rx_path_.raw_drain_attempted >= 20 || this->diag_15m_rx_path_.fifo_overrun > 0)) {
      hint_code = "SX1276_BUSY_ETHER";
      hint_en = "SX1276 is picking up many very weak/partial T1 starts while valid packets are much stronger; this looks like dense ether with distant or overlapping meters, not just a bad antenna.";
      hint_pl = "SX1276 łapie dużo bardzo słabych/częściowych startów T1, podczas gdy poprawne pakiety są znacznie silniejsze; to wygląda na gęsty eter z dalekimi lub nakładającymi się licznikami, a nie tylko złą antenę.";
    } else if (drop_pct >= 40 &&
               avg_drop_rssi <= -90 &&
               avg_ok_rssi >= -82 &&
               ok_vs_drop_rssi_gap >= 10) {
      hint_code = "BUSY_ETHER_WEAK_TRASH";
      hint_en = "many dropped packets are far weaker than valid ones; the receiver likely hears lots of distant or overlapping neighbor meters.";
      hint_pl = "wiele odrzuconych ramek jest dużo słabszych niż poprawne; odbiornik prawdopodobnie słyszy dużo dalekich lub nakładających się liczników sąsiadów.";
    } else if (drop_pct >= 60 && avg_drop_rssi <= -92 && (ok == 0 || avg_ok_rssi <= -86)) {
      hint_code = "WEAK_SIGNAL";
      hint_en = "many drops at very low RSSI and weak valid packets; improve antenna/placement";
      hint_pl = "dużo dropów przy bardzo niskim RSSI i słabych poprawnych pakietach; popraw antenę/pozycję";
    } else if (t1_total > 0 && t1_sym_total >= 200 && t1_sym_invalid_pct >= 5) {
      hint_code = "T1_SYMBOL_ERRORS";
      hint_en = "T1 has many invalid 3-of-6 symbols; likely collisions, interference, or bit errors";
      hint_pl = "T1: dużo błędnych symboli 3-of-6; możliwe kolizje, zakłócenia lub błędy bitów";
    } else if (t1_total > 0 && t1_crc_pct >= 10 && t1_sym_invalid_pct < 2) {
      hint_code = "T1_BITFLIPS";
      hint_en = "T1 mostly decodes but often fails DLL CRC; likely occasional bitflips";
      hint_pl = "T1: dekoduje się, ale często pada CRC DLL; możliwe sporadyczne bitflipy";
    } else if (is_sx1276 &&
               drop_pct >= 20 &&
               (this->diag_15m_rx_path_.preamble_read_failed >= 20 ||
                this->diag_15m_rx_path_.payload_size_unknown >= 10)) {
      hint_code = "SX1276_RX_NOISY";
      hint_en = "SX1276 sees many false starts or undecodable starts; RX frontend likely needs tuning";
      hint_pl = "SX1276 ma dużo fałszywych startów lub nieustalonych początków ramek; ścieżka RX wymaga strojenia";
    } else if (ok > 0 && drop_pct <= 10) {
      hint_code = "GOOD";
      hint_en = "RF link looks stable";
      hint_pl = "łącze radiowe wygląda stabilnie";
    }
  }

  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"summary\"," 
           "\"interval_s\":%u,"
           "\"uptime_ms\":%lu,"
           "\"listen_mode\":\"%s\","
           "\"total\":%u,"
           "\"ok\":%u,"
           "\"truncated\":%u,"
           "\"dropped\":%u,"
           "\"crc_failed\":%u,"
           "\"crc_fail_pct\":%u,"
           "\"drop_pct\":%u,"
           "\"trunc_pct\":%u,"
           "\"avg_ok_rssi\":%d,"
           "\"avg_drop_rssi\":%d,"
           "\"t1\":{"
             "\"total\":%u,\"ok\":%u,\"dropped\":%u,\"per_pct\":%u,"
             "\"crc_failed\":%u,\"crc_pct\":%u,\"avg_ok_rssi\":%d,\"avg_drop_rssi\":%d,"
             "\"sym_total\":%u,\"sym_invalid\":%u,\"sym_invalid_pct\":%u"
           "},"
           "\"c1\":{"
             "\"total\":%u,\"ok\":%u,\"dropped\":%u,\"per_pct\":%u,"
             "\"crc_failed\":%u,\"crc_pct\":%u,\"avg_ok_rssi\":%d,\"avg_drop_rssi\":%d"
           "},"
           "\"dropped_by_reason\":{"
             "\"too_short\":%u,"
             "\"decode_failed\":%u,"
             "\"dll_crc_failed\":%u,"
             "\"unknown_preamble\":%u,"
             "\"l_field_invalid\":%u,"
             "\"unknown_link_mode\":%u,"
             "\"other\":%u"
           "},"
           "\"dropped_by_stage\":{"
             "\"precheck\":%u,"
             "\"t1_decode3of6\":%u,"
             "\"t1_l_field\":%u,"
             "\"t1_length_check\":%u,"
             "\"c1_precheck\":%u,"
             "\"c1_preamble\":%u,"
             "\"c1_suffix\":%u,"
             "\"c1_l_field\":%u,"
             "\"c1_length_check\":%u,"
             "\"dll_crc_first\":%u,"
             "\"dll_crc_mid\":%u,"
             "\"dll_crc_final\":%u,"
             "\"dll_crc_b1\":%u,"
             "\"dll_crc_b2\":%u,"
             "\"link_mode\":%u,"
             "\"other\":%u"
           "},"
           "\"rx_path\":{"
             "\"irq_timeout\":%u,"
             "\"preamble_read_failed\":%u,"
             "\"preamble_retry_recovered\":%u,"
             "\"t1_header_read_failed\":%u,"
             "\"payload_size_unknown\":%u,"
             "\"raw_drain_attempted\":%u,"
             "\"raw_drain_recovered\":%u,"
             "\"raw_drain_recovery_pct\":%u,"
             "\"raw_drain_bytes\":%u,"
             "\"payload_read_failed\":%u,"
             "\"queue_send_failed\":%u,"
             "\"fifo_overrun\":%u,"
             "\"weak_start_aborted\":%u,"
             "\"probe_start_aborted\":%u,"
             "\"raw_drain_skipped_weak\":%u,"
             "\"false_start_like\":%u,"
             "\"probe_abort_rssi\":{\"gt70\":%u,\"70_79\":%u,\"80_89\":%u,\"90_99\":%u,\"lt100\":%u},"
             "\"weak_abort_rssi\":{\"gt70\":%u,\"70_79\":%u,\"80_89\":%u,\"90_99\":%u,\"lt100\":%u}"
           "},"
           "\"reasons_sum\":%u,"
           "\"reasons_sum_mismatch\":%u,"
           "\"hint_code\":\"%s\","
           "\"hint_en\":\"%s\",\"hint_pl\":\"%s\""
           "}",
           (unsigned) interval_s,
           (unsigned long) now_ms,
           listen_mode,
           (unsigned) total,
           (unsigned) this->diag_15m_ok_,
           (unsigned) this->diag_15m_truncated_,
           (unsigned) this->diag_15m_dropped_,
           (unsigned) crc_failed,
           (unsigned) crc_fail_pct,
           (unsigned) drop_pct,
           (unsigned) trunc_pct,
           (int) avg_ok_rssi,
           (int) avg_drop_rssi,
           (unsigned) t1_total,
           (unsigned) t1_ok,
           (unsigned) t1_drop,
           (unsigned) t1_per_pct,
           (unsigned) t1_crc,
           (unsigned) t1_crc_pct,
           (int) t1_avg_ok_rssi,
           (int) t1_avg_drop_rssi,
           (unsigned) t1_sym_total,
           (unsigned) t1_sym_invalid,
           (unsigned) t1_sym_invalid_pct,
           (unsigned) c1_total,
           (unsigned) c1_ok,
           (unsigned) c1_drop,
           (unsigned) c1_per_pct,
           (unsigned) c1_crc,
           (unsigned) c1_crc_pct,
           (int) c1_avg_ok_rssi,
           (int) c1_avg_drop_rssi,
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_TOO_SHORT],
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_DECODE_FAILED],
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_DLL_CRC_FAILED],
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE],
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_L_FIELD_INVALID],
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE],
           (unsigned) this->diag_15m_dropped_by_bucket_[DB_OTHER],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_PRECHECK],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_T1_DECODE3OF6],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_T1_L_FIELD],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_T1_LENGTH_CHECK],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_C1_PRECHECK],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_C1_PREAMBLE],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_C1_SUFFIX],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_C1_L_FIELD],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_C1_LENGTH_CHECK],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_DLL_CRC_FIRST],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_DLL_CRC_MID],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_DLL_CRC_FINAL],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_DLL_CRC_B1],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_DLL_CRC_B2],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_LINK_MODE],
           (unsigned) this->diag_15m_dropped_by_stage_[SB_OTHER],
           (unsigned) this->diag_15m_rx_path_.irq_timeout,
           (unsigned) this->diag_15m_rx_path_.preamble_read_failed,
           (unsigned) this->diag_15m_rx_path_.preamble_retry_recovered,
           (unsigned) this->diag_15m_rx_path_.t1_header_read_failed,
           (unsigned) this->diag_15m_rx_path_.payload_size_unknown,
           (unsigned) this->diag_15m_rx_path_.raw_drain_attempted,
           (unsigned) this->diag_15m_rx_path_.raw_drain_recovered,
           (unsigned) raw_drain_recovery_pct,
           (unsigned) this->diag_15m_rx_path_.raw_drain_bytes,
           (unsigned) this->diag_15m_rx_path_.payload_read_failed,
           (unsigned) this->diag_15m_rx_path_.queue_send_failed,
           (unsigned) this->diag_15m_rx_path_.fifo_overrun,
           (unsigned) this->diag_15m_rx_path_.weak_start_aborted,
           (unsigned) this->diag_15m_rx_path_.probe_start_aborted,
           (unsigned) this->diag_15m_rx_path_.raw_drain_skipped_weak,
           (unsigned) rx_false_start_like,
           // probe_abort_rssi buckets
           (unsigned) this->diag_15m_rx_path_.probe_abort_rssi[0],
           (unsigned) this->diag_15m_rx_path_.probe_abort_rssi[1],
           (unsigned) this->diag_15m_rx_path_.probe_abort_rssi[2],
           (unsigned) this->diag_15m_rx_path_.probe_abort_rssi[3],
           (unsigned) this->diag_15m_rx_path_.probe_abort_rssi[4],
           // weak_abort_rssi buckets
           (unsigned) this->diag_15m_rx_path_.weak_abort_rssi[0],
           (unsigned) this->diag_15m_rx_path_.weak_abort_rssi[1],
           (unsigned) this->diag_15m_rx_path_.weak_abort_rssi[2],
           (unsigned) this->diag_15m_rx_path_.weak_abort_rssi[3],
           (unsigned) this->diag_15m_rx_path_.weak_abort_rssi[4],
           (unsigned) reasons_sum,
           (unsigned) reasons_sum_mismatch,
           hint_code,
           hint_en,
           hint_pl);

  const std::string summary_topic = this->diag_summary_15min_topic_();
  mqtt->publish(summary_topic, payload);
  ESP_LOGI(TAG, "DIAG 15min summary / podsumowanie 15min diag: topic=%s interval=%us uptime_ms=%lu listen_mode=%s total=%u ok=%u truncated=%u dropped=%u crc_failed=%u",
           summary_topic.c_str(), (unsigned) interval_s, (unsigned long) now_ms, listen_mode,
           (unsigned) total, (unsigned) this->diag_15m_ok_,
           (unsigned) this->diag_15m_truncated_, (unsigned) this->diag_15m_dropped_, (unsigned) crc_failed);

  if (std::strcmp(hint_code, "OK") == 0 || std::strcmp(hint_code, "GOOD") == 0) {
    ESP_LOGI(TAG, "DIAG hint: %s | %s / %s", hint_code, hint_en, hint_pl);
  } else {
    ESP_LOGW(TAG, "DIAG hint: %s | %s / %s", hint_code, hint_en, hint_pl);
  }

  // Publish snapshot of all highlight meters alongside this summary (read-only, no window reset).
  if (this->diag_publish_summary_highlight_meters_ && !this->highlight_meter_stats_.empty()) {
    for (auto &kv : this->highlight_meter_stats_) {
      const uint64_t key = kv.first; // uint64_t — meter_id can exceed uint32_t range when shifted
      MeterStats &st = kv.second;
      const uint32_t meter_id = key >> 8;
      const uint8_t mode_byte = key & 0xFF;
      char id_str[12];
      snprintf(id_str, sizeof(id_str), "%08" PRIu32, meter_id);
      const char *mode_str = (mode_byte == (uint8_t) LinkMode::C1) ? "C1" : "T1";
      const uint32_t st_elapsed_s = elapsed / 1000U;
      this->publish_meter_window_for_("summary_15min", st_elapsed_s, id_str, mode_str, st,
                                      st.count_window_time, st.rssi_sum_window_time,
                                      st.rssi_n_window_time, st.interval_sum_window_time_ms,
                                      st.interval_n_window_time,
                                      false, false);
    }
  }
  this->diag_15m_total_ = 0;
  this->diag_15m_ok_ = 0;
  this->diag_15m_truncated_ = 0;
  this->diag_15m_dropped_ = 0;
  this->diag_15m_dropped_by_bucket_.fill(0);
  this->diag_15m_dropped_by_stage_.fill(0);
  this->diag_15m_rssi_ok_sum_ = 0;
  this->diag_15m_rssi_ok_n_ = 0;
  this->diag_15m_rssi_drop_sum_ = 0;
  this->diag_15m_rssi_drop_n_ = 0;
  this->diag_15m_mode_total_.fill(0);
  this->diag_15m_mode_ok_.fill(0);
  this->diag_15m_mode_dropped_.fill(0);
  this->diag_15m_mode_crc_failed_.fill(0);
  this->diag_15m_mode_rssi_ok_sum_.fill(0);
  this->diag_15m_mode_rssi_ok_n_.fill(0);
  this->diag_15m_mode_rssi_drop_sum_.fill(0);
  this->diag_15m_mode_rssi_drop_n_.fill(0);
  this->diag_15m_t1_symbols_total_ = 0;
  this->diag_15m_t1_symbols_invalid_ = 0;
  this->diag_15m_rx_path_ = {};
}


void Radio::maybe_publish_diag_60min_summary_(uint32_t now_ms) {
  if (!this->diag_publish_summary_) return;
  if (!this->diag_publish_summary_60min_) return;
  if (this->diag_topic_.empty()) return;
  if (this->last_diag_60min_summary_ms_ == 0) {
    this->last_diag_60min_summary_ms_ = now_ms;
    return;
  }
  uint32_t elapsed = now_ms - this->last_diag_60min_summary_ms_;
  if (elapsed < this->DIAG_60MIN_INTERVAL_MS_) return;
  this->last_diag_60min_summary_ms_ = now_ms;

  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  const char *listen_mode = (this->radio != nullptr)
                                ? listen_mode_to_string_(this->radio->get_listen_mode())
                                : "unknown";
  const uint32_t interval_s = elapsed / 1000U;

  char payload[2048];
  const uint32_t crc_failed = this->diag_60min_dropped_by_bucket_[DB_DLL_CRC_FAILED];
  const uint32_t total = this->diag_60min_total_;
  const uint32_t ok = this->diag_60min_ok_;
  const uint32_t crc_fail_pct = (total == 0) ? 0 : (crc_failed * 100U) / total;
  const uint32_t drop_pct = (total == 0) ? 0 : (this->diag_60min_dropped_ * 100U) / total;
  const uint32_t trunc_pct = (total == 0) ? 0 : (this->diag_60min_truncated_ * 100U) / total;
  const int32_t avg_ok_rssi = (this->diag_60min_rssi_ok_n_ == 0) ? 0 : (this->diag_60min_rssi_ok_sum_ / (int32_t) this->diag_60min_rssi_ok_n_);
  const int32_t avg_drop_rssi = (this->diag_60min_rssi_drop_n_ == 0) ? 0 : (this->diag_60min_rssi_drop_sum_ / (int32_t) this->diag_60min_rssi_drop_n_);

  const uint8_t T1 = (uint8_t) LinkMode::T1;
  const uint8_t C1 = (uint8_t) LinkMode::C1;
  const uint32_t t1_total = this->diag_60min_mode_total_[T1];
  const uint32_t c1_total = this->diag_60min_mode_total_[C1];
  const uint32_t t1_ok = this->diag_60min_mode_ok_[T1];
  const uint32_t c1_ok = this->diag_60min_mode_ok_[C1];
  const uint32_t t1_drop = this->diag_60min_mode_dropped_[T1];
  const uint32_t c1_drop = this->diag_60min_mode_dropped_[C1];
  const uint32_t t1_crc = this->diag_60min_mode_crc_failed_[T1];
  const uint32_t c1_crc = this->diag_60min_mode_crc_failed_[C1];
  const uint32_t t1_per_pct = (t1_total == 0) ? 0 : (t1_drop * 100U) / t1_total;
  const uint32_t c1_per_pct = (c1_total == 0) ? 0 : (c1_drop * 100U) / c1_total;
  const uint32_t t1_crc_pct = (t1_total == 0) ? 0 : (t1_crc * 100U) / t1_total;
  const uint32_t c1_crc_pct = (c1_total == 0) ? 0 : (c1_crc * 100U) / c1_total;
  const int32_t t1_avg_ok_rssi = (this->diag_60min_mode_rssi_ok_n_[T1] == 0) ? 0 : (this->diag_60min_mode_rssi_ok_sum_[T1] / (int32_t) this->diag_60min_mode_rssi_ok_n_[T1]);
  const int32_t c1_avg_ok_rssi = (this->diag_60min_mode_rssi_ok_n_[C1] == 0) ? 0 : (this->diag_60min_mode_rssi_ok_sum_[C1] / (int32_t) this->diag_60min_mode_rssi_ok_n_[C1]);
  const int32_t t1_avg_drop_rssi = (this->diag_60min_mode_rssi_drop_n_[T1] == 0) ? 0 : (this->diag_60min_mode_rssi_drop_sum_[T1] / (int32_t) this->diag_60min_mode_rssi_drop_n_[T1]);
  const int32_t c1_avg_drop_rssi = (this->diag_60min_mode_rssi_drop_n_[C1] == 0) ? 0 : (this->diag_60min_mode_rssi_drop_sum_[C1] / (int32_t) this->diag_60min_mode_rssi_drop_n_[C1]);

  const uint32_t t1_sym_total = this->diag_60min_t1_symbols_total_;
  const uint32_t t1_sym_invalid = this->diag_60min_t1_symbols_invalid_;
  const uint32_t t1_sym_invalid_pct = (t1_sym_total == 0) ? 0 : (t1_sym_invalid * 100U) / t1_sym_total;
  const bool is_sx1276 = (this->radio != nullptr && strcmp(this->radio->get_name(), "SX1276") == 0);
  const int32_t ok_vs_drop_rssi_gap =
      (this->diag_60min_rssi_ok_n_ == 0 || this->diag_60min_rssi_drop_n_ == 0) ? 0 : (avg_ok_rssi - avg_drop_rssi);
  const uint32_t rx_false_start_like =
      this->diag_60min_rx_path_.preamble_read_failed + this->diag_60min_rx_path_.payload_size_unknown +
      this->diag_60min_rx_path_.weak_start_aborted + this->diag_60min_rx_path_.probe_start_aborted +
      this->diag_60min_rx_path_.raw_drain_skipped_weak;
  const uint32_t raw_drain_recovery_pct =
      (this->diag_60min_rx_path_.raw_drain_attempted == 0)
          ? 0
          : (this->diag_60min_rx_path_.raw_drain_recovered * 100U) / this->diag_60min_rx_path_.raw_drain_attempted;

  const uint32_t reasons_sum =
      this->diag_60min_dropped_by_bucket_[DB_TOO_SHORT] +
      this->diag_60min_dropped_by_bucket_[DB_DECODE_FAILED] +
      this->diag_60min_dropped_by_bucket_[DB_DLL_CRC_FAILED] +
      this->diag_60min_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE] +
      this->diag_60min_dropped_by_bucket_[DB_L_FIELD_INVALID] +
      this->diag_60min_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE] +
      this->diag_60min_dropped_by_bucket_[DB_OTHER];
  const uint32_t reasons_sum_mismatch = (reasons_sum != this->diag_60min_dropped_) ? 1U : 0U;

  const char *hint_code = "OK";
  const char *hint_en = "looks good";
  const char *hint_pl = "wygląda dobrze";
  if (total == 0) {
    hint_code = "NO_DATA";
    hint_en = "no packets received yet";
    hint_pl = "brak odebranych ramek";
  } else {
    if (c1_total > 0 && c1_ok == 0 && c1_crc == c1_total) {
      if (c1_avg_drop_rssi <= -95) {
        hint_code = "C1_WEAK_SIGNAL";
        hint_en = "C1 frames fail DLL CRC at very low RSSI; improve antenna/placement";
        hint_pl = "C1: CRC DLL nie przechodzi przy bardzo niskim RSSI; popraw antenę/pozycję";
      } else {
        hint_code = "C1_INTERFERENCE_OR_RX";
        hint_en = "C1 frames fail DLL CRC despite decent RSSI; check interference/RX settings";
        hint_pl = "C1: CRC DLL nie przechodzi mimo niezłego RSSI; sprawdź zakłócenia/ustawienia RX";
      }
    } else if (c1_total > 0 && c1_crc > 0 && c1_avg_ok_rssi >= -65 && c1_avg_drop_rssi >= -80) {
      hint_code = "C1_OVERLOAD_OR_MULTIPATH";
      hint_en = "C1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move antenna 0.5-2m, change polarization, or attenuate.";
      hint_pl = "C1: CRC pada mimo dobrego RSSI; możliwy przester odbiornika lub wielodrogowość. Odsuń antenę 0,5-2m, zmień polaryzację lub stłum sygnał.";
    } else if (t1_total > 0 && t1_crc > 0 && t1_avg_ok_rssi >= -65 && t1_avg_drop_rssi >= -80) {
      hint_code = "T1_OVERLOAD_OR_MULTIPATH";
      hint_en = "T1 CRC fails despite strong RSSI; possible receiver overload or multipath. Move/rotate antenna or attenuate.";
      hint_pl = "T1: CRC pada mimo dobrego RSSI; możliwy przester lub wielodrogowość. Przestaw/obróć antenę lub stłum sygnał.";
    } else if (is_sx1276 &&
               drop_pct >= 40 &&
               avg_drop_rssi <= -90 &&
               avg_ok_rssi >= -82 &&
               ok_vs_drop_rssi_gap >= 10 &&
               (rx_false_start_like >= 40 || this->diag_60min_rx_path_.raw_drain_attempted >= 20 || this->diag_60min_rx_path_.fifo_overrun > 0)) {
      hint_code = "SX1276_BUSY_ETHER";
      hint_en = "SX1276 is picking up many very weak/partial T1 starts while valid packets are much stronger; this looks like dense ether with distant or overlapping meters, not just a bad antenna.";
      hint_pl = "SX1276 łapie dużo bardzo słabych/częściowych startów T1, podczas gdy poprawne pakiety są znacznie silniejsze; to wygląda na gęsty eter z dalekimi lub nakładającymi się licznikami, a nie tylko złą antenę.";
    } else if (drop_pct >= 40 &&
               avg_drop_rssi <= -90 &&
               avg_ok_rssi >= -82 &&
               ok_vs_drop_rssi_gap >= 10) {
      hint_code = "BUSY_ETHER_WEAK_TRASH";
      hint_en = "many dropped packets are far weaker than valid ones; the receiver likely hears lots of distant or overlapping neighbor meters.";
      hint_pl = "wiele odrzuconych ramek jest dużo słabszych niż poprawne; odbiornik prawdopodobnie słyszy dużo dalekich lub nakładających się liczników sąsiadów.";
    } else if (drop_pct >= 60 && avg_drop_rssi <= -92 && (ok == 0 || avg_ok_rssi <= -86)) {
      hint_code = "WEAK_SIGNAL";
      hint_en = "many drops at very low RSSI and weak valid packets; improve antenna/placement";
      hint_pl = "dużo dropów przy bardzo niskim RSSI i słabych poprawnych pakietach; popraw antenę/pozycję";
    } else if (t1_total > 0 && t1_sym_total >= 200 && t1_sym_invalid_pct >= 5) {
      hint_code = "T1_SYMBOL_ERRORS";
      hint_en = "T1 has many invalid 3-of-6 symbols; likely collisions, interference, or bit errors";
      hint_pl = "T1: dużo błędnych symboli 3-of-6; możliwe kolizje, zakłócenia lub błędy bitów";
    } else if (t1_total > 0 && t1_crc_pct >= 10 && t1_sym_invalid_pct < 2) {
      hint_code = "T1_BITFLIPS";
      hint_en = "T1 mostly decodes but often fails DLL CRC; likely occasional bitflips";
      hint_pl = "T1: dekoduje się, ale często pada CRC DLL; możliwe sporadyczne bitflipy";
    } else if (is_sx1276 &&
               drop_pct >= 20 &&
               (this->diag_60min_rx_path_.preamble_read_failed >= 20 ||
                this->diag_60min_rx_path_.payload_size_unknown >= 10)) {
      hint_code = "SX1276_RX_NOISY";
      hint_en = "SX1276 sees many false starts or undecodable starts; RX frontend likely needs tuning";
      hint_pl = "SX1276 ma dużo fałszywych startów lub nieustalonych początków ramek; ścieżka RX wymaga strojenia";
    } else if (ok > 0 && drop_pct <= 10) {
      hint_code = "GOOD";
      hint_en = "RF link looks stable";
      hint_pl = "łącze radiowe wygląda stabilnie";
    }
  }

  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"summary\"," 
           "\"interval_s\":%u,"
           "\"uptime_ms\":%lu,"
           "\"listen_mode\":\"%s\","
           "\"total\":%u,"
           "\"ok\":%u,"
           "\"truncated\":%u,"
           "\"dropped\":%u,"
           "\"crc_failed\":%u,"
           "\"crc_fail_pct\":%u,"
           "\"drop_pct\":%u,"
           "\"trunc_pct\":%u,"
           "\"avg_ok_rssi\":%d,"
           "\"avg_drop_rssi\":%d,"
           "\"t1\":{"
             "\"total\":%u,\"ok\":%u,\"dropped\":%u,\"per_pct\":%u,"
             "\"crc_failed\":%u,\"crc_pct\":%u,\"avg_ok_rssi\":%d,\"avg_drop_rssi\":%d,"
             "\"sym_total\":%u,\"sym_invalid\":%u,\"sym_invalid_pct\":%u"
           "},"
           "\"c1\":{"
             "\"total\":%u,\"ok\":%u,\"dropped\":%u,\"per_pct\":%u,"
             "\"crc_failed\":%u,\"crc_pct\":%u,\"avg_ok_rssi\":%d,\"avg_drop_rssi\":%d"
           "},"
           "\"dropped_by_reason\":{"
             "\"too_short\":%u,"
             "\"decode_failed\":%u,"
             "\"dll_crc_failed\":%u,"
             "\"unknown_preamble\":%u,"
             "\"l_field_invalid\":%u,"
             "\"unknown_link_mode\":%u,"
             "\"other\":%u"
           "},"
           "\"dropped_by_stage\":{"
             "\"precheck\":%u,"
             "\"t1_decode3of6\":%u,"
             "\"t1_l_field\":%u,"
             "\"t1_length_check\":%u,"
             "\"c1_precheck\":%u,"
             "\"c1_preamble\":%u,"
             "\"c1_suffix\":%u,"
             "\"c1_l_field\":%u,"
             "\"c1_length_check\":%u,"
             "\"dll_crc_first\":%u,"
             "\"dll_crc_mid\":%u,"
             "\"dll_crc_final\":%u,"
             "\"dll_crc_b1\":%u,"
             "\"dll_crc_b2\":%u,"
             "\"link_mode\":%u,"
             "\"other\":%u"
           "},"
           "\"rx_path\":{"
             "\"irq_timeout\":%u,"
             "\"preamble_read_failed\":%u,"
             "\"preamble_retry_recovered\":%u,"
             "\"t1_header_read_failed\":%u,"
             "\"payload_size_unknown\":%u,"
             "\"raw_drain_attempted\":%u,"
             "\"raw_drain_recovered\":%u,"
             "\"raw_drain_recovery_pct\":%u,"
             "\"raw_drain_bytes\":%u,"
             "\"payload_read_failed\":%u,"
             "\"queue_send_failed\":%u,"
             "\"fifo_overrun\":%u,"
             "\"weak_start_aborted\":%u,"
             "\"probe_start_aborted\":%u,"
             "\"raw_drain_skipped_weak\":%u,"
             "\"false_start_like\":%u,"
             "\"probe_abort_rssi\":{\"gt70\":%u,\"70_79\":%u,\"80_89\":%u,\"90_99\":%u,\"lt100\":%u},"
             "\"weak_abort_rssi\":{\"gt70\":%u,\"70_79\":%u,\"80_89\":%u,\"90_99\":%u,\"lt100\":%u}"
           "},"
           "\"reasons_sum\":%u,"
           "\"reasons_sum_mismatch\":%u,"
           "\"hint_code\":\"%s\","
           "\"hint_en\":\"%s\",\"hint_pl\":\"%s\""
           "}",
           (unsigned) interval_s,
           (unsigned long) now_ms,
           listen_mode,
           (unsigned) total,
           (unsigned) this->diag_60min_ok_,
           (unsigned) this->diag_60min_truncated_,
           (unsigned) this->diag_60min_dropped_,
           (unsigned) crc_failed,
           (unsigned) crc_fail_pct,
           (unsigned) drop_pct,
           (unsigned) trunc_pct,
           (int) avg_ok_rssi,
           (int) avg_drop_rssi,
           (unsigned) t1_total,
           (unsigned) t1_ok,
           (unsigned) t1_drop,
           (unsigned) t1_per_pct,
           (unsigned) t1_crc,
           (unsigned) t1_crc_pct,
           (int) t1_avg_ok_rssi,
           (int) t1_avg_drop_rssi,
           (unsigned) t1_sym_total,
           (unsigned) t1_sym_invalid,
           (unsigned) t1_sym_invalid_pct,
           (unsigned) c1_total,
           (unsigned) c1_ok,
           (unsigned) c1_drop,
           (unsigned) c1_per_pct,
           (unsigned) c1_crc,
           (unsigned) c1_crc_pct,
           (int) c1_avg_ok_rssi,
           (int) c1_avg_drop_rssi,
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_TOO_SHORT],
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_DECODE_FAILED],
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_DLL_CRC_FAILED],
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_UNKNOWN_PREAMBLE],
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_L_FIELD_INVALID],
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_UNKNOWN_LINK_MODE],
           (unsigned) this->diag_60min_dropped_by_bucket_[DB_OTHER],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_PRECHECK],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_T1_DECODE3OF6],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_T1_L_FIELD],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_T1_LENGTH_CHECK],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_C1_PRECHECK],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_C1_PREAMBLE],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_C1_SUFFIX],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_C1_L_FIELD],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_C1_LENGTH_CHECK],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_DLL_CRC_FIRST],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_DLL_CRC_MID],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_DLL_CRC_FINAL],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_DLL_CRC_B1],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_DLL_CRC_B2],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_LINK_MODE],
           (unsigned) this->diag_60min_dropped_by_stage_[SB_OTHER],
           (unsigned) this->diag_60min_rx_path_.irq_timeout,
           (unsigned) this->diag_60min_rx_path_.preamble_read_failed,
           (unsigned) this->diag_60min_rx_path_.preamble_retry_recovered,
           (unsigned) this->diag_60min_rx_path_.t1_header_read_failed,
           (unsigned) this->diag_60min_rx_path_.payload_size_unknown,
           (unsigned) this->diag_60min_rx_path_.raw_drain_attempted,
           (unsigned) this->diag_60min_rx_path_.raw_drain_recovered,
           (unsigned) raw_drain_recovery_pct,
           (unsigned) this->diag_60min_rx_path_.raw_drain_bytes,
           (unsigned) this->diag_60min_rx_path_.payload_read_failed,
           (unsigned) this->diag_60min_rx_path_.queue_send_failed,
           (unsigned) this->diag_60min_rx_path_.fifo_overrun,
           (unsigned) this->diag_60min_rx_path_.weak_start_aborted,
           (unsigned) this->diag_60min_rx_path_.probe_start_aborted,
           (unsigned) this->diag_60min_rx_path_.raw_drain_skipped_weak,
           (unsigned) rx_false_start_like,
           // probe_abort_rssi buckets
           (unsigned) this->diag_60min_rx_path_.probe_abort_rssi[0],
           (unsigned) this->diag_60min_rx_path_.probe_abort_rssi[1],
           (unsigned) this->diag_60min_rx_path_.probe_abort_rssi[2],
           (unsigned) this->diag_60min_rx_path_.probe_abort_rssi[3],
           (unsigned) this->diag_60min_rx_path_.probe_abort_rssi[4],
           // weak_abort_rssi buckets
           (unsigned) this->diag_60min_rx_path_.weak_abort_rssi[0],
           (unsigned) this->diag_60min_rx_path_.weak_abort_rssi[1],
           (unsigned) this->diag_60min_rx_path_.weak_abort_rssi[2],
           (unsigned) this->diag_60min_rx_path_.weak_abort_rssi[3],
           (unsigned) this->diag_60min_rx_path_.weak_abort_rssi[4],
           (unsigned) reasons_sum,
           (unsigned) reasons_sum_mismatch,
           hint_code,
           hint_en,
           hint_pl);

  const std::string summary_topic = this->diag_summary_60min_topic_();
  mqtt->publish(summary_topic, payload);
  ESP_LOGI(TAG, "DIAG 60min summary / podsumowanie 60min diag: topic=%s interval=%us uptime_ms=%lu listen_mode=%s total=%u ok=%u truncated=%u dropped=%u crc_failed=%u",
           summary_topic.c_str(), (unsigned) interval_s, (unsigned long) now_ms, listen_mode,
           (unsigned) total, (unsigned) this->diag_60min_ok_,
           (unsigned) this->diag_60min_truncated_, (unsigned) this->diag_60min_dropped_, (unsigned) crc_failed);

  if (std::strcmp(hint_code, "OK") == 0 || std::strcmp(hint_code, "GOOD") == 0) {
    ESP_LOGI(TAG, "DIAG hint: %s | %s / %s", hint_code, hint_en, hint_pl);
  } else {
    ESP_LOGW(TAG, "DIAG hint: %s | %s / %s", hint_code, hint_en, hint_pl);
  }

  // Publish snapshot of all highlight meters alongside this summary (read-only, no window reset).
  if (this->diag_publish_summary_highlight_meters_ && !this->highlight_meter_stats_.empty()) {
    for (auto &kv : this->highlight_meter_stats_) {
      const uint64_t key = kv.first; // uint64_t — meter_id can exceed uint32_t range when shifted
      MeterStats &st = kv.second;
      const uint32_t meter_id = key >> 8;
      const uint8_t mode_byte = key & 0xFF;
      char id_str[12];
      snprintf(id_str, sizeof(id_str), "%08" PRIu32, meter_id);
      const char *mode_str = (mode_byte == (uint8_t) LinkMode::C1) ? "C1" : "T1";
      const uint32_t st_elapsed_s = elapsed / 1000U;
      this->publish_meter_window_for_("summary_60min", st_elapsed_s, id_str, mode_str, st,
                                      st.count_window_time, st.rssi_sum_window_time,
                                      st.rssi_n_window_time, st.interval_sum_window_time_ms,
                                      st.interval_n_window_time,
                                      false, false);
    }
  }

  this->diag_60min_total_ = 0;
  this->diag_60min_ok_ = 0;
  this->diag_60min_truncated_ = 0;
  this->diag_60min_dropped_ = 0;
  this->diag_60min_dropped_by_bucket_.fill(0);
  this->diag_60min_dropped_by_stage_.fill(0);
  this->diag_60min_rssi_ok_sum_ = 0;
  this->diag_60min_rssi_ok_n_ = 0;
  this->diag_60min_rssi_drop_sum_ = 0;
  this->diag_60min_rssi_drop_n_ = 0;
  this->diag_60min_mode_total_.fill(0);
  this->diag_60min_mode_ok_.fill(0);
  this->diag_60min_mode_dropped_.fill(0);
  this->diag_60min_mode_crc_failed_.fill(0);
  this->diag_60min_mode_rssi_ok_sum_.fill(0);
  this->diag_60min_mode_rssi_ok_n_.fill(0);
  this->diag_60min_mode_rssi_drop_sum_.fill(0);
  this->diag_60min_mode_rssi_drop_n_.fill(0);
  this->diag_60min_t1_symbols_total_ = 0;
  this->diag_60min_t1_symbols_invalid_ = 0;
  this->diag_60min_rx_path_ = {};
}


std::string Radio::meter_window_topic_for_(const char *id_str, const char *trigger, const char *mode_str) const {
  if (this->diag_topic_.empty() || id_str == nullptr || id_str[0] == '\0') return {};
  const char *trig = (trigger != nullptr && trigger[0] != '\0') ? trigger : "unknown";
  const char *ms = (mode_str != nullptr && mode_str[0] != '\0') ? mode_str : "unknown";
  // Topic includes mode so dual-mode meters (T1+C1 same ID) get separate paths.
  return this->diag_topic_ + "/meter/" + std::string(id_str) + "/" + ms + "/window/" + trig;
}

// Publish windowed stats for a single meter and reset its window counters.
// trigger: "count" = packet threshold reached, "time" = periodic timer fired
void Radio::publish_meter_window_for_(const char *trigger, uint32_t elapsed_s,
                                      const char *id_str, const char *mode_str, MeterStats &st,
                                      uint32_t count_window, int32_t rssi_sum_window,
                                      uint32_t rssi_n_window,
                                      uint32_t interval_sum_window_ms,
                                      uint32_t interval_n_window,
                                      bool reset_time_window,
                                      bool reset_count_window) {
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  const int32_t win_avg_rssi = (rssi_n_window > 0)
      ? (rssi_sum_window / (int32_t) rssi_n_window) : 0;
  const uint32_t avg_interval_s = (st.interval_n > 0)
      ? (st.interval_sum_ms / st.interval_n) / 1000 : 0;
  const uint32_t win_avg_interval_s = (interval_n_window > 0)
      ? (interval_sum_window_ms / interval_n_window) / 1000 : 0;

  const uint32_t now_ms = (uint32_t) esphome::millis();
  const char *listen_mode = (this->radio != nullptr)
                                ? listen_mode_to_string_(this->radio->get_listen_mode())
                                : "unknown";

  char payload[512];
  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"meter_window\","
           "\"uptime_ms\":%lu,"
           "\"listen_mode\":\"%s\","
           "\"trigger\":\"%s\","
           "\"id\":\"%s\","
           "\"mode\":\"%s\","
           "\"elapsed_s\":%u,"
           "\"count_window\":%u,"
           "\"count_total\":%u,"
           "\"avg_interval_s\":%u,"
           "\"win_avg_interval_s\":%u,"
           "\"win_interval_n\":%u,"
           "\"last_rssi\":%d,"
           "\"win_avg_rssi\":%d"
           "}",
           (unsigned long) now_ms,
           listen_mode,
           trigger, id_str, mode_str,
           (unsigned) elapsed_s,
           (unsigned) count_window,
           (unsigned) st.count,
           (unsigned) avg_interval_s,
           (unsigned) win_avg_interval_s,
           (unsigned) interval_n_window,
           (int) st.rssi_last,
           (int) win_avg_rssi);

  const std::string meter_window_topic = this->meter_window_topic_for_(id_str, trigger, mode_str);
  if (!meter_window_topic.empty()) {
    mqtt->publish(meter_window_topic, payload);
  }
  ESP_LOGI(TAG, "METER / LICZNIK [%s] uptime_ms=%lu listen_mode=%s id=%s mode=%s win=%us count_window=%u total=%u avg_interval=%us win_avg_interval=%us win_avg_rssi=%ddBm",
           trigger, (unsigned long) now_ms, listen_mode, id_str, mode_str,
           (unsigned) elapsed_s,
           (unsigned) count_window,
           (unsigned) st.count,
           (unsigned) avg_interval_s,
           (unsigned) win_avg_interval_s,
           (int) win_avg_rssi);

  if (reset_time_window) {
    st.count_window_time = 0;
    st.rssi_sum_window_time = 0;
    st.rssi_n_window_time = 0;
    st.interval_sum_window_time_ms = 0;
    st.interval_n_window_time = 0;
  }
  if (reset_count_window) {
    st.count_window_count = 0;
    st.rssi_sum_window_count = 0;
    st.rssi_n_window_count = 0;
    st.interval_sum_window_count_ms = 0;
    st.interval_n_window_count = 0;
    st.count_window_started_ms = 0;
  }
}

void Radio::maybe_publish_meter_windows_(uint32_t now_ms) {
  if (this->highlight_meter_stats_.empty()) return;
  if (this->diag_topic_.empty()) return;

  if (this->last_meter_window_ms_ == 0) {
    this->last_meter_window_ms_ = now_ms;
    return;
  }
  if (now_ms - this->last_meter_window_ms_ < this->meter_window_interval_ms_) return;
  const uint32_t elapsed_s = (now_ms - this->last_meter_window_ms_) / 1000;
  this->last_meter_window_ms_ = now_ms;

  for (auto &kv : this->highlight_meter_stats_) {
    // Key = (meter_id << 8) | link_mode_byte. Decode both.
    const uint32_t key_id   = (uint32_t) (kv.first >> 8);
    const uint8_t  key_mode = (uint8_t)  (kv.first & 0xFF);
    char id_str[9];
    snprintf(id_str, sizeof(id_str), "%08" PRIu32, key_id);
    const char *mode_str = (key_mode == (uint8_t) LinkMode::T1) ? "T1"
                         : (key_mode == (uint8_t) LinkMode::C1) ? "C1" : "UNK";
    auto &st = kv.second;
    this->publish_meter_window_for_("time", elapsed_s, id_str, mode_str, st,
                                    st.count_window_time,
                                    st.rssi_sum_window_time,
                                    st.rssi_n_window_time,
                                    st.interval_sum_window_time_ms,
                                    st.interval_n_window_time,
                                    true, false);
  }
}

void Radio::setup() {
  // Parse optional highlight meter list (CSV provided by python/YAML).
  parse_meter_id_csv_(this->highlight_meters_csv_, this->highlight_meter_ids_);
  if (!this->target_meter_id_str_.empty()) {
    std::vector<uint32_t> tmp;
    parse_meter_id_csv_(this->target_meter_id_str_, tmp);
    if (!tmp.empty()) {
      this->target_meter_id_ = tmp.front();
      this->target_meter_enabled_ = true;
      if (this->target_topic_.empty()) {
        this->target_topic_ = this->derived_target_topic_();
      }
      char id_buf[9];
      snprintf(id_buf, sizeof(id_buf), "%08u", (unsigned) this->target_meter_id_);
      ESP_LOGI(TAG, "Target meter forwarding enabled / wlaczono przekazywanie docelowego licznika id=%s topic=%s",
               id_buf, this->target_topic_.empty() ? "<derived at runtime>" : this->target_topic_.c_str());
    }
  }

  if (!this->telegram_topic_.empty()) {
    ESP_LOGI(TAG, "Frame RAW forwarding topic / topic publikacji RAW: %s", this->telegram_topic_.c_str());
  }

  if (!this->highlight_meter_ids_.empty()) {
    // meter_window_interval_ms_ defaults to 15 min; cap it at diag_summary_interval_ms_ minimum
    if (this->meter_window_interval_ms_ < this->diag_summary_interval_ms_)
      this->meter_window_interval_ms_ = this->diag_summary_interval_ms_;
    ESP_LOGI(TAG, "Highlight meters enabled / wlaczono wyroznione liczniki (%u ids) tag=%s ansi=%s window=%us",
             (unsigned) this->highlight_meter_ids_.size(),
             this->highlight_tag_.empty() ? "wmbus_user" : this->highlight_tag_.c_str(),
             this->highlight_ansi_ ? "true" : "false",
             (unsigned) (this->meter_window_interval_ms_ / 1000));
  }

  ASSERT_SETUP(this->packet_queue_ = xQueueCreate(3, sizeof(Packet *)));

  ASSERT_SETUP(xTaskCreate((TaskFunction_t)this->receiver_task, "radio_recv",
                           3 * 1024, this, 2, &(this->receiver_task_handle_)));

  ESP_LOGI(TAG, "Receiver task created / utworzono task odbiornika [%p]", this->receiver_task_handle_);

  this->radio->attach_data_interrupt(Radio::wakeup_receiver_task_from_isr,
                                     &(this->receiver_task_handle_));

  // One-shot publication of SX1262 device errors before/after boot clear.
  // This is best-effort; if MQTT isn't ready yet we publish from loop().
  if (this->publish_dev_err_after_clear_ && this->radio != nullptr) {
    uint16_t before = 0, after = 0;
    if (this->radio->get_boot_device_errors(before, after)) {
      this->dev_err_before_ = before;
      this->dev_err_after_ = after;
      this->dev_err_cleared_pending_ = true;
    }
  }

  this->boot_log_done_ = false;
  this->boot_log_last_ms_ = (uint32_t) esphome::millis();
  this->boot_log_count_ = 0;
  this->boot_info_mqtt_pending_ = true;
  this->boot_info_event_pending_ = true;
}

void Radio::dump_config() {
  ESP_LOGCONFIG(TAG, "wM-Bus Radio:");
  if (this->radio == nullptr) {
    ESP_LOGCONFIG(TAG, "  Radio: <null>");
    return;
  }

  ESP_LOGCONFIG(TAG, "  Radio type: %s", this->radio->get_name());
  ESP_LOGCONFIG(TAG, "  Listen mode: %s",
                listen_mode_to_string_(this->radio->get_listen_mode()));
  const char *busy_mode = (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::NORMAL) ? "normal"
                           : (this->sx1276_busy_ether_mode_ == SX1276BusyEtherMode::AGGRESSIVE) ? "aggressive"
                           : "adaptive";
  ESP_LOGCONFIG(TAG, "  SX1276 busy ether mode: %s", busy_mode);
  if (!this->diag_topic_.empty()) {
    ESP_LOGCONFIG(TAG, "  MQTT boot topic: %s/boot", this->diag_topic_.c_str());
    ESP_LOGCONFIG(TAG, "  Summary interval: %us -> %s", (unsigned) (this->diag_summary_interval_ms_ / 1000), this->diag_summary_topic_().c_str());
    ESP_LOGCONFIG(TAG, "  15min summary: %s -> %s", this->diag_publish_summary_15min_ ? "enabled (900s)" : "disabled", this->diag_summary_15min_topic_().c_str());
    ESP_LOGCONFIG(TAG, "  60min summary: %s -> %s", this->diag_publish_summary_60min_ ? "enabled (3600s)" : "disabled", this->diag_summary_60min_topic_().c_str());
  }
}

void Radio::loop() {
  const uint32_t loop_now_ms = (uint32_t) esphome::millis();

  if (!this->boot_log_done_ && this->radio != nullptr) {
    if (loop_now_ms - this->boot_log_last_ms_ >= 5000) {
      ESP_LOGI(TAG, "Radio active / radio aktywne: %s | Listen mode / tryb nasluchu: %s",
               this->radio->get_name(),
               listen_mode_to_string_(this->radio->get_listen_mode()));
      this->boot_log_last_ms_ = loop_now_ms;
      this->boot_log_count_++;
      if (this->boot_log_count_ >= 3) {
        this->boot_log_done_ = true;
      }
    }
  }

  auto *mqtt = mqtt::global_mqtt_client;
  if (this->radio != nullptr && mqtt != nullptr && mqtt->is_connected() && !this->diag_topic_.empty()) {
    std::string boot_payload = str_sprintf(
        "{\"event\":\"boot\",\"radio\":\"%s\",\"listen_mode\":\"%s\",\"uptime_ms\":%lu}",
        this->radio->get_name(),
        listen_mode_to_string_(this->radio->get_listen_mode()),
        (unsigned long) loop_now_ms);

    if (this->boot_info_mqtt_pending_) {
      std::string boot_topic = this->diag_topic_ + "/boot";
      mqtt->publish(boot_topic, boot_payload, static_cast<uint8_t>(0), true);
      this->boot_info_mqtt_pending_ = false;
    }
    if (this->boot_info_event_pending_) {
      mqtt->publish(this->diag_topic_, std::string(boot_payload), static_cast<uint8_t>(0), false);
      this->boot_info_event_pending_ = false;
    }
  }

  if (this->dev_err_cleared_pending_ && mqtt != nullptr && mqtt->is_connected() && !this->diag_topic_.empty()) {
    const char *listen_mode = (this->radio != nullptr)
                                  ? listen_mode_to_string_(this->radio->get_listen_mode())
                                  : "unknown";
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"event\":\"dev_err_cleared\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"before\":%u,\"before_hex\":\"%04X\",\"after\":%u,\"after_hex\":\"%04X\"}",
             (unsigned long) loop_now_ms, listen_mode,
             (unsigned) this->dev_err_before_, (unsigned) this->dev_err_before_,
             (unsigned) this->dev_err_after_, (unsigned) this->dev_err_after_);
    mqtt->publish(this->diag_topic_, payload);
    this->dev_err_cleared_pending_ = false;
  }

  this->maybe_publish_diag_summary_(loop_now_ms);
  this->maybe_publish_diag_15min_summary_(loop_now_ms);
  this->maybe_publish_diag_60min_summary_(loop_now_ms);
  this->maybe_publish_meter_windows_(loop_now_ms);
  Packet *p;
  if (xQueueReceive(this->packet_queue_, &p, 0) != pdPASS)
    return;

  const uint8_t mode_idx = (uint8_t) p->get_link_mode();

  // Filter by listen_mode BEFORE convert_to_frame() and before any counters.
  // Use packet's raw link mode (detected from preamble) — valid even if frame
  // conversion later fails (e.g. T1 3-of-6 decode error).
  if (this->radio != nullptr) {
    const auto want = this->radio->get_listen_mode();
    const LinkMode got = p->get_link_mode();
    const bool reject =
        (want == LISTEN_MODE_C1 && got != LinkMode::C1) ||
        (want == LISTEN_MODE_T1 && got != LinkMode::T1);
    if (reject) {
      ESP_LOGD(TAG, "Filtered by listen_mode: want=%s got=%s RSSI=%ddBm",
               (want == LISTEN_MODE_C1) ? "C1" : "T1",
               link_mode_name(got),
               (int) p->get_rssi());
      delete p;
      return;
    }
  }

  // Count only frames that pass the listen_mode filter
  this->diag_total_++;
  this->diag_15m_total_++;
  this->diag_60min_total_++;
  if (mode_idx < this->diag_mode_total_.size()) this->diag_mode_total_[mode_idx]++;
  if (mode_idx < this->diag_15m_mode_total_.size()) this->diag_15m_mode_total_[mode_idx]++;
  if (mode_idx < this->diag_60min_mode_total_.size()) this->diag_60min_mode_total_[mode_idx]++;

  auto frame = p->convert_to_frame();

  if (mode_idx == (uint8_t) LinkMode::T1) {
    this->diag_t1_symbols_total_ += (uint32_t) p->t1_symbols_total();
    this->diag_t1_symbols_invalid_ += (uint32_t) p->t1_symbols_invalid();
    this->diag_15m_t1_symbols_total_ += (uint32_t) p->t1_symbols_total();
    this->diag_15m_t1_symbols_invalid_ += (uint32_t) p->t1_symbols_invalid();
    this->diag_60min_t1_symbols_total_ += (uint32_t) p->t1_symbols_total();
    this->diag_60min_t1_symbols_invalid_ += (uint32_t) p->t1_symbols_invalid();
  }

  if (!frame) {
    const char *mode = link_mode_name(p->get_link_mode());
    const char *listen_mode = (this->radio != nullptr)
                                  ? listen_mode_to_string_(this->radio->get_listen_mode())
                                  : "unknown";
    this->diag_dropped_by_stage_[bucket_for_stage_(p->drop_stage())]++;
    this->diag_15m_dropped_by_stage_[bucket_for_stage_(p->drop_stage())]++;
    this->diag_60min_dropped_by_stage_[bucket_for_stage_(p->drop_stage())]++;

    if (p->is_truncated()) {
      this->diag_truncated_++;
      this->diag_15m_truncated_++;
      this->diag_60min_truncated_++;
      if (this->should_publish_packet_event_(p) && mqtt::global_mqtt_client != nullptr && !this->diag_topic_.empty()) {
        char payload[1280];
        if (this->diag_publish_raw_) {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"truncated\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"reason\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"decoded_len\":%u,\"final_len\":%u,\"dll_crc_removed\":%u,\"suffix_ignored\":%u,\"raw\":\"%s\"}",
                   (unsigned long) loop_now_ms, listen_mode,
                   p->drop_reason().c_str(), p->drop_stage().c_str(), p->drop_detail().c_str(),
                   mode, (int) p->get_rssi(), (unsigned) p->want_len(),
                   (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                   (unsigned) p->decoded_len(), (unsigned) p->final_len(),
                   (unsigned) p->dll_crc_removed(), (unsigned) p->suffix_ignored(),
                   p->raw_hex().c_str());
        } else {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"truncated\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"reason\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"decoded_len\":%u,\"final_len\":%u,\"dll_crc_removed\":%u,\"suffix_ignored\":%u}",
                   (unsigned long) loop_now_ms, listen_mode,
                   p->drop_reason().c_str(), p->drop_stage().c_str(), p->drop_detail().c_str(),
                   mode, (int) p->get_rssi(), (unsigned) p->want_len(),
                   (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                   (unsigned) p->decoded_len(), (unsigned) p->final_len(),
                   (unsigned) p->dll_crc_removed(), (unsigned) p->suffix_ignored());
        }
        mqtt::global_mqtt_client->publish(this->diag_topic_, payload);
      }

      if (this->diag_verbose_) {
        ESP_LOGW(TAG,
                 "TRUNCATED frame / ucieta ramka: uptime_ms=%lu listen_mode=%s stage=%s reason=%s mode=%s want=%u got=%u raw_got=%u decoded_len=%u final_len=%u RSSI=%ddBm detail=%s",
                 (unsigned long) loop_now_ms, listen_mode, p->drop_stage().c_str(), p->drop_reason().c_str(), mode,
                 (unsigned) p->want_len(), (unsigned) p->got_len(),
                 (unsigned) p->raw_got_len(), (unsigned) p->decoded_len(),
                 (unsigned) p->final_len(), (int) p->get_rssi(), p->drop_detail().c_str());
        if (this->diag_publish_raw_) {
          ESP_LOGW(TAG, "TRUNCATED raw(hex) / ucieta ramka raw(hex)=%s", p->raw_hex().c_str());
        }
      }
    } else if (!p->drop_reason().empty()) {
      this->diag_dropped_++;
      this->diag_15m_dropped_++;
      this->diag_60min_dropped_++;
      this->diag_rssi_drop_sum_ += (int32_t) p->get_rssi();
      this->diag_rssi_drop_n_++;
      this->diag_15m_rssi_drop_sum_ += (int32_t) p->get_rssi();
      this->diag_15m_rssi_drop_n_++;
      this->diag_60min_rssi_drop_sum_ += (int32_t) p->get_rssi();
      this->diag_60min_rssi_drop_n_++;
      if (mode_idx < this->diag_mode_dropped_.size()) {
        this->diag_mode_dropped_[mode_idx]++;
        this->diag_mode_rssi_drop_sum_[mode_idx] += (int32_t) p->get_rssi();
        this->diag_mode_rssi_drop_n_[mode_idx]++;
      }
      if (mode_idx < this->diag_15m_mode_dropped_.size()) {
        this->diag_15m_mode_dropped_[mode_idx]++;
        this->diag_15m_mode_rssi_drop_sum_[mode_idx] += (int32_t) p->get_rssi();
        this->diag_15m_mode_rssi_drop_n_[mode_idx]++;
      }
      if (mode_idx < this->diag_60min_mode_dropped_.size()) {
        this->diag_60min_mode_dropped_[mode_idx]++;
        this->diag_60min_mode_rssi_drop_sum_[mode_idx] += (int32_t) p->get_rssi();
        this->diag_60min_mode_rssi_drop_n_[mode_idx]++;
      }
      auto bucket = bucket_for_reason_(p->drop_reason());
      this->diag_dropped_by_bucket_[bucket]++;
      this->diag_15m_dropped_by_bucket_[bucket]++;
      this->diag_60min_dropped_by_bucket_[bucket]++;
      if (bucket == DB_DLL_CRC_FAILED && mode_idx < this->diag_mode_crc_failed_.size()) {
        this->diag_mode_crc_failed_[mode_idx]++;
      }
      if (bucket == DB_DLL_CRC_FAILED && mode_idx < this->diag_15m_mode_crc_failed_.size()) {
        this->diag_15m_mode_crc_failed_[mode_idx]++;
      }
      if (bucket == DB_DLL_CRC_FAILED && mode_idx < this->diag_60min_mode_crc_failed_.size()) {
        this->diag_60min_mode_crc_failed_[mode_idx]++;
      }

      if (this->should_publish_packet_event_(p) && mqtt::global_mqtt_client != nullptr && !this->diag_topic_.empty()) {
        char payload[1280];
        if (this->diag_publish_raw_) {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"dropped\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"reason\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"decoded_len\":%u,\"final_len\":%u,\"dll_crc_removed\":%u,\"suffix_ignored\":%u,\"raw\":\"%s\"}",
                   (unsigned long) loop_now_ms, listen_mode,
                   p->drop_reason().c_str(), p->drop_stage().c_str(), p->drop_detail().c_str(),
                   mode, (int) p->get_rssi(), (unsigned) p->want_len(),
                   (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                   (unsigned) p->decoded_len(), (unsigned) p->final_len(),
                   (unsigned) p->dll_crc_removed(), (unsigned) p->suffix_ignored(),
                   p->raw_hex().c_str());
        } else {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"dropped\",\"uptime_ms\":%lu,\"listen_mode\":\"%s\",\"reason\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"decoded_len\":%u,\"final_len\":%u,\"dll_crc_removed\":%u,\"suffix_ignored\":%u}",
                   (unsigned long) loop_now_ms, listen_mode,
                   p->drop_reason().c_str(), p->drop_stage().c_str(), p->drop_detail().c_str(),
                   mode, (int) p->get_rssi(), (unsigned) p->want_len(),
                   (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                   (unsigned) p->decoded_len(), (unsigned) p->final_len(),
                   (unsigned) p->dll_crc_removed(), (unsigned) p->suffix_ignored());
        }
        mqtt::global_mqtt_client->publish(this->diag_topic_, payload);
      }

      if (this->diag_verbose_) {
        ESP_LOGW(TAG,
                 "DROPPED packet / odrzucony pakiet: uptime_ms=%lu listen_mode=%s stage=%s reason=%s mode=%s want=%u got=%u raw_got=%u decoded_len=%u final_len=%u RSSI=%ddBm detail=%s",
                 (unsigned long) loop_now_ms, listen_mode, p->drop_stage().c_str(), p->drop_reason().c_str(), mode,
                 (unsigned) p->want_len(), (unsigned) p->got_len(),
                 (unsigned) p->raw_got_len(), (unsigned) p->decoded_len(),
                 (unsigned) p->final_len(), (int) p->get_rssi(), p->drop_detail().c_str());
        if (this->diag_publish_raw_) {
          ESP_LOGW(TAG, "DROPPED raw(hex) / odrzucony pakiet raw(hex)=%s", p->raw_hex().c_str());
        }
      }
    }

    this->collect_radio_rx_diag_();
    delete p;
    return;
  }

  this->diag_ok_++;
  this->diag_15m_ok_++;
  this->diag_60min_ok_++;
  this->diag_rssi_ok_sum_ += (int32_t) frame->rssi();
  this->diag_rssi_ok_n_++;
  this->diag_15m_rssi_ok_sum_ += (int32_t) frame->rssi();
  this->diag_15m_rssi_ok_n_++;
  this->diag_60min_rssi_ok_sum_ += (int32_t) frame->rssi();
  this->diag_60min_rssi_ok_n_++;
  if (!this->recent_ok_rssi_valid_) {
    this->recent_ok_rssi_avg_ = (int32_t) frame->rssi();
    this->recent_ok_rssi_valid_ = true;
  } else {
    this->recent_ok_rssi_avg_ = ((this->recent_ok_rssi_avg_ * 7) + (int32_t) frame->rssi()) / 8;
  }
  if (mode_idx < this->diag_mode_ok_.size()) {
    this->diag_mode_ok_[mode_idx]++;
    this->diag_mode_rssi_ok_sum_[mode_idx] += (int32_t) frame->rssi();
    this->diag_mode_rssi_ok_n_[mode_idx]++;
  }
  if (mode_idx < this->diag_15m_mode_ok_.size()) {
    this->diag_15m_mode_ok_[mode_idx]++;
    this->diag_15m_mode_rssi_ok_sum_[mode_idx] += (int32_t) frame->rssi();
    this->diag_15m_mode_rssi_ok_n_[mode_idx]++;
  }
  if (mode_idx < this->diag_60min_mode_ok_.size()) {
    this->diag_60min_mode_ok_[mode_idx]++;
    this->diag_60min_mode_rssi_ok_sum_[mode_idx] += (int32_t) frame->rssi();
    this->diag_60min_mode_rssi_ok_n_[mode_idx]++;
  }

  this->collect_radio_rx_diag_();

  auto &d = frame->data();

  const char *mfr = "???";
  char id_str[9] = "????????";
  uint8_t ver = 0xFF;
  uint8_t dev = 0xFF;
  uint8_t ci = 0xFF;
  uint32_t id_val = 0;

  auto is_bcd = [](uint8_t b) -> bool {
    return ((b & 0x0F) <= 9) && (((b >> 4) & 0x0F) <= 9);
  };

  auto decode_mfr = [](uint16_t m, char out[4]) {
    out[0] = (char)(((m >> 10) & 0x1F) + 64);
    out[1] = (char)(((m >> 5) & 0x1F) + 64);
    out[2] = (char)((m & 0x1F) + 64);
    out[3] = 0;
    auto ok = [](char c) { return c >= 'A' && c <= 'Z'; };
    if (!ok(out[0]) || !ok(out[1]) || !ok(out[2])) {
      out[0] = out[1] = out[2] = '?';
    }
  };

  char mfr_buf[4] = "???";

  int base = -1;
  if (d.size() >= 10 && (size_t) (d[0] + 1) == d.size())
    base = 1;
  else if (d.size() >= 9)
    base = 0;

  if (base >= 0 && (int) d.size() >= base + 10) {
    uint16_t m = (uint16_t) d[base + 1] | ((uint16_t) d[base + 2] << 8);
    decode_mfr(m, mfr_buf);
    mfr = mfr_buf;

    bool bcd_ok = is_bcd(d[base + 3]) && is_bcd(d[base + 4]) && is_bcd(d[base + 5]) && is_bcd(d[base + 6]);
    if (bcd_ok) {
      id_val = (uint32_t) ((((d[base + 6] >> 4) & 0x0F) * 10000000U) + ((d[base + 6] & 0x0F) * 1000000U) +
                           (((d[base + 5] >> 4) & 0x0F) * 100000U) + ((d[base + 5] & 0x0F) * 10000U) +
                           (((d[base + 4] >> 4) & 0x0F) * 1000U) + ((d[base + 4] & 0x0F) * 100U) +
                           (((d[base + 3] >> 4) & 0x0F) * 10U) + (d[base + 3] & 0x0F));
      snprintf(id_str, sizeof(id_str), "%01u%01u%01u%01u%01u%01u%01u%01u",
               (d[base + 6] >> 4) & 0x0F, d[base + 6] & 0x0F,
               (d[base + 5] >> 4) & 0x0F, d[base + 5] & 0x0F,
               (d[base + 4] >> 4) & 0x0F, d[base + 4] & 0x0F,
               (d[base + 3] >> 4) & 0x0F, d[base + 3] & 0x0F);
    } else {
      snprintf(id_str, sizeof(id_str), "%02X%02X%02X%02X",
               d[base + 6], d[base + 5], d[base + 4], d[base + 3]);
    }

    ver = d[base + 7];
    dev = d[base + 8];
    ci = d[base + 9];
  }

  bool highlight = false;
  if (id_val != 0 && !this->highlight_meter_ids_.empty()) {
    highlight = std::binary_search(this->highlight_meter_ids_.begin(), this->highlight_meter_ids_.end(), id_val);
  }

  // Update per-meter statistics for highlighted meters.
  if (highlight) {
    uint32_t now_ms = (uint32_t) esphome::millis();
    // Composite key keeps T1 and C1 streams separate for dual-mode meters.
    const uint64_t stats_key = ((uint64_t) id_val << 8) | (uint8_t) frame->link_mode();
    auto &stats = this->highlight_meter_stats_[stats_key];
    stats.count++;
    stats.rssi_last = frame->rssi();
    stats.rssi_sum += (int32_t) frame->rssi();
    stats.rssi_n++;
    // Independent windowed counters for time-trigger and count-trigger.
    stats.count_window_time++;
    stats.rssi_sum_window_time += (int32_t) frame->rssi();
    stats.rssi_n_window_time++;

    if (stats.count_window_count == 0) {
      stats.count_window_started_ms = now_ms;
    }
    stats.count_window_count++;
    stats.rssi_sum_window_count += (int32_t) frame->rssi();
    stats.rssi_n_window_count++;

    if (stats.last_seen_ms != 0) {
      stats.last_interval_ms = now_ms - stats.last_seen_ms;
      stats.interval_sum_ms += stats.last_interval_ms;
      stats.interval_n++;
      stats.interval_sum_window_time_ms += stats.last_interval_ms;
      stats.interval_n_window_time++;
      if (stats.count_window_count > 1) {
        stats.interval_sum_window_count_ms += stats.last_interval_ms;
        stats.interval_n_window_count++;
      }
    }
    stats.last_seen_ms = now_ms;

    // Count-based trigger: publish when the dedicated count-window reaches threshold.
    if (this->meter_window_count_threshold_ > 0 &&
        stats.count_window_count >= this->meter_window_count_threshold_) {
      const uint32_t elapsed_s = (stats.count_window_started_ms > 0)
          ? ((uint32_t) esphome::millis() - stats.count_window_started_ms) / 1000 : 0;
      const char *count_mode_str = (((uint8_t) frame->link_mode()) == (uint8_t) LinkMode::T1) ? "T1" : "C1";
      this->publish_meter_window_for_("count", elapsed_s, id_str, count_mode_str, stats,
                                      stats.count_window_count,
                                      stats.rssi_sum_window_count,
                                      stats.rssi_n_window_count,
                                      stats.interval_sum_window_count_ms,
                                      stats.interval_n_window_count,
                                      false, true);
    }
  }

  const char *log_tag = TAG;
  if (highlight) {
    if (!this->highlight_tag_.empty()) log_tag = this->highlight_tag_.c_str();
    const char *ansi_pre = this->highlight_ansi_ ? "\033[1;32m" : "";
    const char *ansi_suf = this->highlight_ansi_ ? "\033[0m" : "";
    ESP_LOGI(log_tag, "%s%sHave data / odebrano dane (%zu bytes) [RSSI: %ddBm, mode: %s %s, mfr:%s id:%s ver:%u type:%u ci:%02X]%s",
             ansi_pre, this->highlight_prefix_.c_str(),
             d.size(), frame->rssi(),
             link_mode_name(frame->link_mode()),
             frame->format().c_str(),
             mfr, id_str, (unsigned) ver, (unsigned) dev, (unsigned) ci,
             ansi_suf);

    // Log and optionally publish per-meter interval statistics.
    const uint64_t stats_key_ro = ((uint64_t) id_val << 8) | (uint8_t) frame->link_mode();
    const auto &stats = this->highlight_meter_stats_[stats_key_ro];
    if (stats.count == 1) {
      ESP_LOGI(log_tag, "%s[id:%s] first packet seen / pierwszy pakiet (count=1)",
               this->highlight_prefix_.c_str(), id_str);
    } else {
      const uint32_t interval_s  = stats.last_interval_ms / 1000;
      const uint32_t interval_ms = stats.last_interval_ms % 1000;
      const uint32_t avg_interval_s = (stats.interval_n > 0)
          ? (stats.interval_sum_ms / stats.interval_n) / 1000 : 0;
      const int32_t avg_rssi = (stats.rssi_n > 0)
          ? (stats.rssi_sum / (int32_t) stats.rssi_n) : stats.rssi_last;
      ESP_LOGI(log_tag, "%s[id:%s] stats / statystyki: count=%u interval=%u.%03us avg_interval=%us avg_rssi=%ddBm",
               this->highlight_prefix_.c_str(), id_str,
               (unsigned) stats.count,
               (unsigned) interval_s, (unsigned) interval_ms,
               (unsigned) avg_interval_s,
               (int) avg_rssi);
    }

    // Publish per-meter stats to MQTT if connected.
    if (!this->diag_topic_.empty()) {
      auto *mqtt = esphome::mqtt::global_mqtt_client;
      if (mqtt != nullptr && mqtt->is_connected()) {
        const uint64_t stats_key_win = ((uint64_t) id_val << 8) | (uint8_t) frame->link_mode();
        const auto &st = this->highlight_meter_stats_[stats_key_win];
        const uint32_t avg_interval_s = (st.interval_n > 0)
            ? (st.interval_sum_ms / st.interval_n) / 1000 : 0;
        const int32_t avg_rssi = (st.rssi_n > 0)
            ? (st.rssi_sum / (int32_t) st.rssi_n) : st.rssi_last;
        char meter_payload[256];
        snprintf(meter_payload, sizeof(meter_payload),
                 "{"
                 "\"event\":\"meter_stats\","
                 "\"id\":\"%s\","
                 "\"count\":%u,"
                 "\"last_interval_s\":%u,"
                 "\"avg_interval_s\":%u,"
                 "\"last_rssi\":%d,"
                 "\"avg_rssi\":%d"
                 "}",
                 id_str,
                 (unsigned) st.count,
                 (unsigned) (st.last_interval_ms / 1000),
                 (unsigned) avg_interval_s,
                 (int) st.rssi_last,
                 (int) avg_rssi);
        std::string meter_topic = this->diag_topic_ + "/meter/" + std::string(id_str);
        mqtt->publish(meter_topic, meter_payload);
      }
    }
  } else {
    ESP_LOGI(TAG, "Have data / odebrano dane (%zu bytes) [RSSI: %ddBm, mode: %s %s, mfr:%s id:%s ver:%u type:%u ci:%02X]",
             d.size(), frame->rssi(),
             link_mode_name(frame->link_mode()),
             frame->format().c_str(),
             mfr, id_str, (unsigned) ver, (unsigned) dev, (unsigned) ci);
  }

  this->maybe_forward_frame_(frame.value(), id_val, id_str, log_tag);

  for (auto &handler : this->handlers_)
    handler(&frame.value());

  if (frame->handlers_count())
    ESP_LOGI(TAG, "Telegram handled / obsluzono przez %d handlers", frame->handlers_count());
  else
    ESP_LOGD(TAG, "Telegram not handled by any handler");

  delete p;
}

void Radio::wakeup_receiver_task_from_isr(TaskHandle_t *arg) {
  BaseType_t xHigherPriorityTaskWoken;
  vTaskNotifyGiveFromISR(*arg, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void Radio::receive_frame() {
  const uint32_t total_wait_ms = 60000;
  const uint32_t hop_ms = 500;
  uint32_t waited = 0;
  bool got_irq = false;
  while (waited < total_wait_ms) {
    this->radio->restart_rx();
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(hop_ms))) {
      got_irq = true;
      break;
    }
    waited += hop_ms;
  }
  if (!got_irq) {
    this->diag_rx_path_.irq_timeout++;
    this->diag_15m_rx_path_.irq_timeout++;
    this->diag_60min_rx_path_.irq_timeout++;
    this->collect_radio_rx_diag_();
    this->publish_rx_path_event_("rx_path", "receive_wait", "interrupt_timeout");
    ESP_LOGD(TAG, "Radio interrupt timeout");
    return;
  }

  auto packet = std::make_unique<Packet>();

  auto queue_packet = [this](std::unique_ptr<Packet> &pkt) -> bool {
    pkt->set_rssi(this->radio->get_rssi());
    auto packet_ptr = pkt.get();
    if (xQueueSend(this->packet_queue_, &packet_ptr, 0) == pdTRUE) {
      ESP_LOGV(TAG, "Queue items: %zu", uxQueueMessagesWaiting(this->packet_queue_));
      ESP_LOGV(TAG, "Queue send success");
      this->collect_radio_rx_diag_();
      pkt.release();
      return true;
    }

    this->diag_rx_path_.queue_send_failed++;
    this->diag_15m_rx_path_.queue_send_failed++;
    this->diag_60min_rx_path_.queue_send_failed++;
    this->collect_radio_rx_diag_();
    this->publish_rx_path_event_("rx_path", "queue_send", "queue_full_or_busy", this->radio->get_rssi());
    ESP_LOGW(TAG, "Queue send failed / wyslanie do kolejki nie powiodlo sie");
    return false;
  };

  auto raw_drain_fallback = [this, &packet, &queue_packet](const char *stage, const char *reason_detail,
                                                           size_t already_read, bool is_c_mode) -> bool {
    const int current_rssi = this->radio->get_rssi();
    if (!this->should_attempt_raw_drain_(current_rssi, already_read, is_c_mode)) {
      this->diag_rx_path_.raw_drain_skipped_weak++;
      this->diag_15m_rx_path_.raw_drain_skipped_weak++;
      this->diag_60min_rx_path_.raw_drain_skipped_weak++;
      return false;
    }

    this->diag_rx_path_.raw_drain_attempted++;
    this->diag_15m_rx_path_.raw_drain_attempted++;
    this->diag_60min_rx_path_.raw_drain_attempted++;
    const size_t max_extra = (already_read < WMBUS_RAW_DRAIN_MAX_BYTES)
                                 ? (WMBUS_RAW_DRAIN_MAX_BYTES - already_read)
                                 : 0;
    if (max_extra == 0) return false;

    auto *tail = packet->append_space(max_extra);
    size_t extra_read = 0;
    this->radio->read_in_task_partial(tail, max_extra, extra_read, 1, 1);
    packet->resize(already_read + extra_read);
    this->diag_rx_path_.raw_drain_bytes += (uint32_t) extra_read;
    this->diag_15m_rx_path_.raw_drain_bytes += (uint32_t) extra_read;
    this->diag_60min_rx_path_.raw_drain_bytes += (uint32_t) extra_read;

    char detail[144];
    snprintf(detail, sizeof(detail), "%s already_read=%u extra=%u final_raw=%u",
             reason_detail != nullptr ? reason_detail : "raw_drain",
             (unsigned) already_read, (unsigned) extra_read, (unsigned) packet->size());
    this->publish_rx_path_event_("rx_path", stage, detail, current_rssi);

    if (packet->size() > already_read) {
      this->diag_rx_path_.raw_drain_recovered++;
      this->diag_15m_rx_path_.raw_drain_recovered++;
      this->diag_60min_rx_path_.raw_drain_recovered++;
      ESP_LOGD(TAG, "Queued raw-drain fallback packet (%u -> %u bytes)",
               (unsigned) already_read, (unsigned) packet->size());
      return queue_packet(packet);
    }

    return false;
  };

  auto *preamble = packet->append_space(WMBUS_PREAMBLE_SIZE);
  size_t got_preamble = 0;
  this->radio->read_in_task_partial(preamble, WMBUS_PREAMBLE_SIZE, got_preamble, 1, 1);

  if (got_preamble < WMBUS_PREAMBLE_SIZE && radio_supports_preamble_retry_(this->radio)) {
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2))) {
      size_t got_retry = 0;
      this->radio->read_in_task_partial(preamble + got_preamble, WMBUS_PREAMBLE_SIZE - got_preamble,
                                        got_retry, 1, 1);
      got_preamble += got_retry;
      if (got_preamble == WMBUS_PREAMBLE_SIZE) {
        this->diag_rx_path_.preamble_retry_recovered++;
        this->diag_15m_rx_path_.preamble_retry_recovered++;
        this->diag_60min_rx_path_.preamble_retry_recovered++;
      }
    }
  }

  if (got_preamble < WMBUS_PREAMBLE_SIZE) {
    packet->resize(got_preamble);
    this->diag_rx_path_.preamble_read_failed++;
    this->diag_15m_rx_path_.preamble_read_failed++;
    this->diag_60min_rx_path_.preamble_read_failed++;
    const int current_rssi = this->radio->get_rssi();
    char detail[128];
    snprintf(detail, sizeof(detail), "got=%u need=%u", (unsigned) got_preamble, (unsigned) WMBUS_PREAMBLE_SIZE);
    if (this->should_abort_weak_partial_start_(current_rssi, got_preamble, false)) {
      this->diag_rx_path_.weak_start_aborted++;
      this->diag_15m_rx_path_.weak_start_aborted++;
      this->diag_60min_rx_path_.weak_start_aborted++;
      this->diag_rx_path_.weak_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->diag_15m_rx_path_.weak_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->diag_60min_rx_path_.weak_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      strlcat(detail, " weak_partial_start", sizeof(detail));
    }
    this->collect_radio_rx_diag_();
    this->publish_rx_path_event_("rx_path", "receive_preamble", detail, current_rssi);
    ESP_LOGV(TAG, "Failed to read preamble");
    return;
  }

  const bool is_c_mode = (preamble[0] == WMBUS_MODE_C_PREAMBLE);
  size_t already_read = WMBUS_PREAMBLE_SIZE;
  if (!is_c_mode) {
    const int current_rssi = this->radio->get_rssi();
    if (this->should_abort_t1_probe_start_(current_rssi)) {
      this->diag_rx_path_.probe_start_aborted++;
      this->diag_15m_rx_path_.probe_start_aborted++;
      this->diag_60min_rx_path_.probe_start_aborted++;
      this->diag_rx_path_.probe_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->diag_15m_rx_path_.probe_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->diag_60min_rx_path_.probe_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->collect_radio_rx_diag_();
      this->publish_rx_path_event_("rx_path", "receive_probe_start", "weak_t1_probe_start", current_rssi);
      ESP_LOGV(TAG, "Abort weak T1 start before probe read");
      return;
    }
  }
  if (!is_c_mode && WMBUS_T1_LEN_PROBE_BYTES > WMBUS_PREAMBLE_SIZE) {
    const size_t extra = WMBUS_T1_LEN_PROBE_BYTES - WMBUS_PREAMBLE_SIZE;
    auto *hdr = packet->append_space(extra);
    size_t got_hdr = 0;
    this->radio->read_in_task_partial(hdr, extra, got_hdr, 1, 1);
    already_read += got_hdr;
    if (got_hdr < extra) {
      this->diag_rx_path_.t1_header_read_failed++;
      this->diag_15m_rx_path_.t1_header_read_failed++;
      this->diag_60min_rx_path_.t1_header_read_failed++;
      packet->resize(already_read);
      ESP_LOGV(TAG, "Short T1 probe read: got=%u need=%u", (unsigned) got_hdr, (unsigned) extra);
    }
  }

  const size_t total_len = packet->expected_size();
  if (total_len == 0 || total_len < already_read) {
    this->diag_rx_path_.payload_size_unknown++;
    this->diag_15m_rx_path_.payload_size_unknown++;
    this->diag_60min_rx_path_.payload_size_unknown++;
    const int current_rssi = this->radio->get_rssi();
    char detail[144];
    snprintf(detail, sizeof(detail), "total_len=%u already_read=%u", (unsigned) total_len, (unsigned) already_read);

    if (this->should_abort_weak_partial_start_(current_rssi, already_read, is_c_mode)) {
      this->diag_rx_path_.weak_start_aborted++;
      this->diag_15m_rx_path_.weak_start_aborted++;
      this->diag_60min_rx_path_.weak_start_aborted++;
      this->diag_rx_path_.weak_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->diag_15m_rx_path_.weak_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      this->diag_60min_rx_path_.weak_abort_rssi[rssi_abort_bucket_(current_rssi)]++;
      strlcat(detail, " weak_partial_start", sizeof(detail));
      this->collect_radio_rx_diag_();
      this->publish_rx_path_event_("rx_path", "receive_expected_size", detail, current_rssi);
      ESP_LOGD(TAG, "Abort weak partial start before raw-drain");
      return;
    }

    if (raw_drain_fallback("receive_expected_size", detail, already_read, is_c_mode)) {
      return;
    }
    if (this->diag_rx_path_.raw_drain_skipped_weak > 0 &&
        this->should_abort_weak_partial_start_(current_rssi, already_read, is_c_mode)) {
      strlcat(detail, " raw_drain_skipped_weak", sizeof(detail));
    }

    this->collect_radio_rx_diag_();
    this->publish_rx_path_event_("rx_path", "receive_expected_size", detail, current_rssi);
    ESP_LOGD(TAG, "Cannot calculate payload size");
    return;
  }

  const size_t remaining = total_len - already_read;
  if (remaining > 0) {
    auto *rest = packet->append_space(remaining);
    if (!this->radio->read_in_task(rest, remaining)) {
      packet->resize(already_read);
      this->diag_rx_path_.payload_read_failed++;
      this->diag_15m_rx_path_.payload_read_failed++;
      this->diag_60min_rx_path_.payload_read_failed++;
      char detail[112];
      snprintf(detail, sizeof(detail), "remaining=%u total_len=%u already_read=%u", (unsigned) remaining,
               (unsigned) total_len, (unsigned) already_read);

      if (raw_drain_fallback("receive_payload", detail, already_read, is_c_mode)) {
        return;
      }
      if (this->diag_rx_path_.raw_drain_skipped_weak > 0 &&
          this->should_abort_weak_partial_start_(this->radio->get_rssi(), already_read, is_c_mode)) {
        strlcat(detail, " raw_drain_skipped_weak", sizeof(detail));
      }

      this->collect_radio_rx_diag_();
      this->publish_rx_path_event_("rx_path", "receive_payload", detail, this->radio->get_rssi());
      ESP_LOGW(TAG, "Failed to read data / nie udalo sie odczytac danych");
      return;
    }
  }

  queue_packet(packet);
}

void Radio::receiver_task(Radio *arg) {
  while (true)
    arg->receive_frame();
}

void Radio::add_frame_handler(std::function<void(Frame *)> &&callback) {
  this->handlers_.push_back(std::move(callback));
}

} // namespace wmbus_radio
} // namespace esphome
