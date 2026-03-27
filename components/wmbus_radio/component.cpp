#include "component.h"

#include "freertos/queue.h"
#include "freertos/task.h"

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <memory>

// Optional: publish diagnostics via ESPHome MQTT if mqtt component is present.
#include "esphome/components/mqtt/mqtt_client.h"

#define WMBUS_PREAMBLE_SIZE (3)
#define WMBUS_MODE_C_PREAMBLE (0x54)
#define WMBUS_T1_LEN_PROBE_BYTES (18)

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
  if (stage == "link_mode") return SB_LINK_MODE;
  return SB_OTHER;
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

  char payload[320];
  if (detail != nullptr && detail[0] != '\0') {
    snprintf(payload, sizeof(payload),
             "{\"event\":\"%s\",\"stage\":\"%s\",\"rssi\":%d,\"detail\":\"%s\"}",
             event, stage, rssi, detail);
  } else {
    snprintf(payload, sizeof(payload),
             "{\"event\":\"%s\",\"stage\":\"%s\",\"rssi\":%d}",
             event, stage, rssi);
  }
  mqtt->publish(this->diag_topic_, payload);
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

  char payload[1800];
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
    } else if (drop_pct >= 60 && avg_drop_rssi <= -92) {
      hint_code = "WEAK_SIGNAL";
      hint_en = "many drops at very low RSSI; improve antenna/placement";
      hint_pl = "dużo dropów przy bardzo niskim RSSI; popraw antenę/pozycję";
    } else if (t1_total > 0 && t1_sym_total >= 200 && t1_sym_invalid_pct >= 5) {
      hint_code = "T1_SYMBOL_ERRORS";
      hint_en = "T1 has many invalid 3-of-6 symbols; likely bit errors/interference";
      hint_pl = "T1: dużo błędnych symboli 3-of-6; możliwe błędy bitów/zakłócenia";
    } else if (t1_total > 0 && t1_crc_pct >= 10 && t1_sym_invalid_pct < 2) {
      hint_code = "T1_BITFLIPS";
      hint_en = "T1 mostly decodes but often fails DLL CRC; likely occasional bitflips";
      hint_pl = "T1: dekoduje się, ale często pada CRC DLL; możliwe sporadyczne bitflipy";
    } else if (ok > 0 && drop_pct <= 10) {
      hint_code = "GOOD";
      hint_en = "RF link looks stable";
      hint_pl = "łącze radiowe wygląda stabilnie";
    }
  }

  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"summary\","
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
             "\"t1_header_read_failed\":%u,"
             "\"payload_size_unknown\":%u,"
             "\"payload_read_failed\":%u,"
             "\"queue_send_failed\":%u"
           "},"
           "\"reasons_sum\":%u,"
           "\"reasons_sum_mismatch\":%u,"
           "\"hint_code\":\"%s\","
           "\"hint_en\":\"%s\",\"hint_pl\":\"%s\""
           "}",
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
           (unsigned) this->diag_rx_path_.t1_header_read_failed,
           (unsigned) this->diag_rx_path_.payload_size_unknown,
           (unsigned) this->diag_rx_path_.payload_read_failed,
           (unsigned) this->diag_rx_path_.queue_send_failed,
           (unsigned) reasons_sum,
           (unsigned) reasons_sum_mismatch,
           hint_code,
           hint_en,
           hint_pl);

  mqtt->publish(this->diag_topic_, payload);
  ESP_LOGI(TAG, "DIAG summary published to %s (total=%u ok=%u truncated=%u dropped=%u crc_failed=%u)",
           this->diag_topic_.c_str(), (unsigned) total, (unsigned) this->diag_ok_,
           (unsigned) this->diag_truncated_, (unsigned) this->diag_dropped_, (unsigned) crc_failed);

  if (std::strcmp(hint_code, "OK") == 0) {
    ESP_LOGI(TAG, "DIAG hint: %s | %s", hint_code, hint_pl);
  } else {
    ESP_LOGW(TAG, "DIAG hint: %s | %s", hint_code, hint_pl);
  }

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

// Publish windowed stats for a single meter and reset its window counters.
// trigger: "count" = packet threshold reached, "time" = periodic timer fired
void Radio::publish_meter_window_for_(const char *trigger, uint32_t elapsed_s,
                                      const char *id_str, MeterStats &st) {
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  const int32_t win_avg_rssi = (st.rssi_n_window > 0)
      ? (st.rssi_sum_window / (int32_t) st.rssi_n_window) : 0;
  const uint32_t avg_interval_s = (st.interval_n > 0)
      ? (st.interval_sum_ms / st.interval_n) / 1000 : 0;

  char payload[280];
  snprintf(payload, sizeof(payload),
           "{"
           "\"event\":\"meter_window\","
           "\"trigger\":\"%s\","
           "\"id\":\"%s\","
           "\"elapsed_s\":%u,"
           "\"count_window\":%u,"
           "\"count_total\":%u,"
           "\"avg_interval_s\":%u,"
           "\"last_rssi\":%d,"
           "\"win_avg_rssi\":%d"
           "}",
           trigger, id_str,
           (unsigned) elapsed_s,
           (unsigned) st.count_window,
           (unsigned) st.count,
           (unsigned) avg_interval_s,
           (int) st.rssi_last,
           (int) win_avg_rssi);

  if (!this->diag_topic_.empty()) {
    std::string meter_topic = this->diag_topic_ + "/meter/" + std::string(id_str);
    mqtt->publish(meter_topic, payload);
  }
  ESP_LOGI(TAG, "METER [%s] id=%s win=%us count_window=%u total=%u avg_interval=%us win_avg_rssi=%ddBm",
           trigger, id_str,
           (unsigned) elapsed_s,
           (unsigned) st.count_window,
           (unsigned) st.count,
           (unsigned) avg_interval_s,
           (int) win_avg_rssi);

  st.count_window = 0;
  st.rssi_sum_window = 0;
  st.rssi_n_window = 0;
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
    char id_str[9];
    snprintf(id_str, sizeof(id_str), "%08u", (unsigned) kv.first);
    this->publish_meter_window_for_("time", elapsed_s, id_str, kv.second);
  }
}

void Radio::setup() {
  // Parse optional highlight meter list (CSV provided by python/YAML).
  parse_meter_id_csv_(this->highlight_meters_csv_, this->highlight_meter_ids_);
  if (!this->highlight_meter_ids_.empty()) {
    // meter_window_interval_ms_ defaults to 15 min; cap it at diag_summary_interval_ms_ minimum
    if (this->meter_window_interval_ms_ < this->diag_summary_interval_ms_)
      this->meter_window_interval_ms_ = this->diag_summary_interval_ms_;
    ESP_LOGI(TAG, "Highlight meters enabled (%u ids) tag=%s ansi=%s window=%us",
             (unsigned) this->highlight_meter_ids_.size(),
             this->highlight_tag_.empty() ? "wmbus_user" : this->highlight_tag_.c_str(),
             this->highlight_ansi_ ? "true" : "false",
             (unsigned) (this->meter_window_interval_ms_ / 1000));
  }

  ASSERT_SETUP(this->packet_queue_ = xQueueCreate(3, sizeof(Packet *)));

  ASSERT_SETUP(xTaskCreate((TaskFunction_t)this->receiver_task, "radio_recv",
                           3 * 1024, this, 2, &(this->receiver_task_handle_)));

  ESP_LOGI(TAG, "Receiver task created [%p]", this->receiver_task_handle_);

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
}

void Radio::loop() {
  if (this->dev_err_cleared_pending_ && mqtt::global_mqtt_client != nullptr && !this->diag_topic_.empty()) {
    char payload[180];
    snprintf(payload, sizeof(payload),
             "{\"event\":\"dev_err_cleared\",\"before\":%u,\"before_hex\":\"%04X\",\"after\":%u,\"after_hex\":\"%04X\"}",
             (unsigned) this->dev_err_before_, (unsigned) this->dev_err_before_,
             (unsigned) this->dev_err_after_, (unsigned) this->dev_err_after_);
    mqtt::global_mqtt_client->publish(this->diag_topic_, payload);
    this->dev_err_cleared_pending_ = false;
  }

  const uint32_t loop_now_ms = (uint32_t) esphome::millis();
  this->maybe_publish_diag_summary_(loop_now_ms);
  this->maybe_publish_meter_windows_(loop_now_ms);
  Packet *p;
  if (xQueueReceive(this->packet_queue_, &p, 0) != pdPASS)
    return;

  this->diag_total_++;
  const uint8_t mode_idx = (uint8_t) p->get_link_mode();
  if (mode_idx < this->diag_mode_total_.size()) this->diag_mode_total_[mode_idx]++;

  auto frame = p->convert_to_frame();

  if (mode_idx == (uint8_t) LinkMode::T1) {
    this->diag_t1_symbols_total_ += (uint32_t) p->t1_symbols_total();
    this->diag_t1_symbols_invalid_ += (uint32_t) p->t1_symbols_invalid();
  }

  if (!frame) {
    const char *mode = link_mode_name(p->get_link_mode());
    this->diag_dropped_by_stage_[bucket_for_stage_(p->drop_stage())]++;

    if (p->is_truncated()) {
      this->diag_truncated_++;
      if (this->should_publish_packet_event_(p) && mqtt::global_mqtt_client != nullptr && !this->diag_topic_.empty()) {
        char payload[1100];
        if (this->diag_publish_raw_) {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"truncated\",\"reason\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"decoded_len\":%u,\"final_len\":%u,\"dll_crc_removed\":%u,\"suffix_ignored\":%u,\"raw\":\"%s\"}",
                   p->drop_reason().c_str(), p->drop_stage().c_str(), p->drop_detail().c_str(),
                   mode, (int) p->get_rssi(), (unsigned) p->want_len(),
                   (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                   (unsigned) p->decoded_len(), (unsigned) p->final_len(),
                   (unsigned) p->dll_crc_removed(), (unsigned) p->suffix_ignored(),
                   p->raw_hex().c_str());
        } else {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"truncated\",\"reason\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"decoded_len\":%u,\"final_len\":%u,\"dll_crc_removed\":%u,\"suffix_ignored\":%u}",
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
                 "TRUNCATED frame: stage=%s reason=%s mode=%s want=%u got=%u raw_got=%u decoded_len=%u final_len=%u RSSI=%ddBm detail=%s",
                 p->drop_stage().c_str(), p->drop_reason().c_str(), mode,
                 (unsigned) p->want_len(), (unsigned) p->got_len(),
                 (unsigned) p->raw_got_len(), (unsigned) p->decoded_len(),
                 (unsigned) p->final_len(), (int) p->get_rssi(), p->drop_detail().c_str());
        if (this->diag_publish_raw_) {
          ESP_LOGW(TAG, "TRUNCATED raw(hex)=%s", p->raw_hex().c_str());
        }
      }
    } else if (!p->drop_reason().empty()) {
      this->diag_dropped_++;
      this->diag_rssi_drop_sum_ += (int32_t) p->get_rssi();
      this->diag_rssi_drop_n_++;
      if (mode_idx < this->diag_mode_dropped_.size()) {
        this->diag_mode_dropped_[mode_idx]++;
        this->diag_mode_rssi_drop_sum_[mode_idx] += (int32_t) p->get_rssi();
        this->diag_mode_rssi_drop_n_[mode_idx]++;
      }
      auto bucket = bucket_for_reason_(p->drop_reason());
      this->diag_dropped_by_bucket_[bucket]++;
      if (bucket == DB_DLL_CRC_FAILED && mode_idx < this->diag_mode_crc_failed_.size()) {
        this->diag_mode_crc_failed_[mode_idx]++;
      }

      if (this->should_publish_packet_event_(p) && mqtt::global_mqtt_client != nullptr && !this->diag_topic_.empty()) {
        char payload[1100];
        if (this->diag_publish_raw_) {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"dropped\",\"reason\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"decoded_len\":%u,\"final_len\":%u,\"dll_crc_removed\":%u,\"suffix_ignored\":%u,\"raw\":\"%s\"}",
                   p->drop_reason().c_str(), p->drop_stage().c_str(), p->drop_detail().c_str(),
                   mode, (int) p->get_rssi(), (unsigned) p->want_len(),
                   (unsigned) p->got_len(), (unsigned) p->raw_got_len(),
                   (unsigned) p->decoded_len(), (unsigned) p->final_len(),
                   (unsigned) p->dll_crc_removed(), (unsigned) p->suffix_ignored(),
                   p->raw_hex().c_str());
        } else {
          snprintf(payload, sizeof(payload),
                   "{\"event\":\"dropped\",\"reason\":\"%s\",\"stage\":\"%s\",\"detail\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"want\":%u,\"got\":%u,\"raw_got\":%u,\"decoded_len\":%u,\"final_len\":%u,\"dll_crc_removed\":%u,\"suffix_ignored\":%u}",
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
                 "DROPPED packet: stage=%s reason=%s mode=%s want=%u got=%u raw_got=%u decoded_len=%u final_len=%u RSSI=%ddBm detail=%s",
                 p->drop_stage().c_str(), p->drop_reason().c_str(), mode,
                 (unsigned) p->want_len(), (unsigned) p->got_len(),
                 (unsigned) p->raw_got_len(), (unsigned) p->decoded_len(),
                 (unsigned) p->final_len(), (int) p->get_rssi(), p->drop_detail().c_str());
        if (this->diag_publish_raw_) {
          ESP_LOGW(TAG, "DROPPED raw(hex)=%s", p->raw_hex().c_str());
        }
      }
    }

    delete p;
    return;
  }

  this->diag_ok_++;
  this->diag_rssi_ok_sum_ += (int32_t) frame->rssi();
  this->diag_rssi_ok_n_++;
  if (mode_idx < this->diag_mode_ok_.size()) {
    this->diag_mode_ok_[mode_idx]++;
    this->diag_mode_rssi_ok_sum_[mode_idx] += (int32_t) frame->rssi();
    this->diag_mode_rssi_ok_n_[mode_idx]++;
  }

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
    auto &stats = this->highlight_meter_stats_[id_val];
    stats.count++;
    stats.rssi_last = frame->rssi();
    stats.rssi_sum += (int32_t) frame->rssi();
    stats.rssi_n++;
    // Windowed counters — reset on count threshold or periodic timer
    stats.count_window++;
    stats.rssi_sum_window += (int32_t) frame->rssi();
    stats.rssi_n_window++;

    // Count-based trigger: publish when window reaches threshold
    if (this->meter_window_count_threshold_ > 0 &&
        stats.count_window >= this->meter_window_count_threshold_) {
      const uint32_t elapsed_s = (stats.last_seen_ms > 0 && this->last_meter_window_ms_ > 0)
          ? ((uint32_t) esphome::millis() - this->last_meter_window_ms_) / 1000 : 0;
      this->publish_meter_window_for_("count", elapsed_s, id_str, stats);
    }

    if (stats.last_seen_ms != 0) {
      stats.last_interval_ms = now_ms - stats.last_seen_ms;
      stats.interval_sum_ms += stats.last_interval_ms;
      stats.interval_n++;
    }
    stats.last_seen_ms = now_ms;
  }

  const char *log_tag = TAG;
  if (highlight) {
    if (!this->highlight_tag_.empty()) log_tag = this->highlight_tag_.c_str();
    const char *ansi_pre = this->highlight_ansi_ ? "\033[1;32m" : "";
    const char *ansi_suf = this->highlight_ansi_ ? "\033[0m" : "";
    ESP_LOGI(log_tag, "%s%sHave data (%zu bytes) [RSSI: %ddBm, mode: %s %s, mfr:%s id:%s ver:%u type:%u ci:%02X]%s",
             ansi_pre, this->highlight_prefix_.c_str(),
             d.size(), frame->rssi(),
             link_mode_name(frame->link_mode()),
             frame->format().c_str(),
             mfr, id_str, (unsigned) ver, (unsigned) dev, (unsigned) ci,
             ansi_suf);

    // Log and optionally publish per-meter interval statistics.
    const auto &stats = this->highlight_meter_stats_[id_val];
    if (stats.count == 1) {
      ESP_LOGI(log_tag, "%s[id:%s] first packet seen (count=1)",
               this->highlight_prefix_.c_str(), id_str);
    } else {
      const uint32_t interval_s  = stats.last_interval_ms / 1000;
      const uint32_t interval_ms = stats.last_interval_ms % 1000;
      const uint32_t avg_interval_s = (stats.interval_n > 0)
          ? (stats.interval_sum_ms / stats.interval_n) / 1000 : 0;
      const int32_t avg_rssi = (stats.rssi_n > 0)
          ? (stats.rssi_sum / (int32_t) stats.rssi_n) : stats.rssi_last;
      ESP_LOGI(log_tag, "%s[id:%s] count=%u interval=%u.%03us avg_interval=%us avg_rssi=%ddBm",
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
        const auto &st = this->highlight_meter_stats_[id_val];
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
    ESP_LOGI(TAG, "Have data (%zu bytes) [RSSI: %ddBm, mode: %s %s, mfr:%s id:%s ver:%u type:%u ci:%02X]",
             d.size(), frame->rssi(),
             link_mode_name(frame->link_mode()),
             frame->format().c_str(),
             mfr, id_str, (unsigned) ver, (unsigned) dev, (unsigned) ci);
  }

  for (auto &handler : this->handlers_)
    handler(&frame.value());

  if (frame->handlers_count())
    ESP_LOGI(TAG, "Telegram handled by %d handlers", frame->handlers_count());
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
    this->publish_rx_path_event_("rx_path", "receive_wait", "interrupt_timeout");
    ESP_LOGD(TAG, "Radio interrupt timeout");
    return;
  }
  auto packet = std::make_unique<Packet>();

  auto *preamble = packet->append_space(WMBUS_PREAMBLE_SIZE);
  if (!this->radio->read_in_task(preamble, WMBUS_PREAMBLE_SIZE)) {
    this->diag_rx_path_.preamble_read_failed++;
    this->publish_rx_path_event_("rx_path", "receive_preamble", "read_failed");
    ESP_LOGV(TAG, "Failed to read preamble");
    return;
  }

  const bool is_c_mode = (preamble[0] == WMBUS_MODE_C_PREAMBLE);
  size_t already_read = WMBUS_PREAMBLE_SIZE;
  if (!is_c_mode && WMBUS_T1_LEN_PROBE_BYTES > WMBUS_PREAMBLE_SIZE) {
    const size_t extra = WMBUS_T1_LEN_PROBE_BYTES - WMBUS_PREAMBLE_SIZE;
    auto *hdr = packet->append_space(extra);
    if (!this->radio->read_in_task(hdr, extra)) {
      this->diag_rx_path_.t1_header_read_failed++;
      this->publish_rx_path_event_("rx_path", "receive_t1_header", "read_failed");
      ESP_LOGV(TAG, "Failed to read T1 header");
      return;
    }
    already_read += extra;
  }

  const size_t total_len = packet->expected_size();
  if (total_len == 0 || total_len < already_read) {
    this->diag_rx_path_.payload_size_unknown++;
    char detail[96];
    snprintf(detail, sizeof(detail), "total_len=%u already_read=%u", (unsigned) total_len, (unsigned) already_read);
    this->publish_rx_path_event_("rx_path", "receive_expected_size", detail);
    ESP_LOGD(TAG, "Cannot calculate payload size");
    return;
  }

  const size_t remaining = total_len - already_read;
  if (remaining > 0) {
    auto *rest = packet->append_space(remaining);
    if (!this->radio->read_in_task(rest, remaining)) {
      this->diag_rx_path_.payload_read_failed++;
      char detail[96];
      snprintf(detail, sizeof(detail), "remaining=%u total_len=%u already_read=%u", (unsigned) remaining, (unsigned) total_len, (unsigned) already_read);
      this->publish_rx_path_event_("rx_path", "receive_payload", detail);
      ESP_LOGW(TAG, "Failed to read data");
      return;
    }
  }

  packet->set_rssi(this->radio->get_rssi());
  auto packet_ptr = packet.get();

  if (xQueueSend(this->packet_queue_, &packet_ptr, 0) == pdTRUE) {
    ESP_LOGV(TAG, "Queue items: %zu", uxQueueMessagesWaiting(this->packet_queue_));
    ESP_LOGV(TAG, "Queue send success");
    packet.release();
  } else {
    this->diag_rx_path_.queue_send_failed++;
    this->publish_rx_path_event_("rx_path", "queue_send", "queue_full_or_busy", this->radio->get_rssi());
    ESP_LOGW(TAG, "Queue send failed");
  }
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
