// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-meter statistics for the wmbus_radio component: highlight-meter matching,
// the meter_snapshot batch and the per-meter reception windows. Split out of
// component.cpp unchanged (move-only refactor); MQTT topics, payloads, field
// names and the windowing behaviour are identical.

#include "component.h"
#include "wmbus_radio_internal.h"

#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/components/mqtt/mqtt_client.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <string>

namespace esphome {
namespace wmbus_radio {

static const char *TAG = "wmbus";

bool Radio::meter_is_highlighted_(uint32_t meter_id) const {
  return meter_id != 0 && !this->highlight_meter_ids_.empty() &&
         std::binary_search(this->highlight_meter_ids_.begin(), this->highlight_meter_ids_.end(), meter_id);
}

void Radio::publish_meter_window_batch_(const char *trigger, uint32_t elapsed_s, uint32_t now_ms) {
  if (!this->diag_publish_summary_highlight_meters_) return;
  if (this->highlight_meter_stats_.empty()) return;
  if (this->diag_topic_.empty()) return;
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  const char *listen_mode = (this->radio != nullptr)
                                ? listen_mode_to_string_(this->radio->get_listen_mode())
                                : "unknown";

  // Build JSON array of all highlight meters in one payload.
  // Topic: {diag_topic}/meter_snapshot
  std::string batch = "{";
  batch += "\"event\":\"meter_snapshot\",";
  batch += "\"trigger\":\"";
  batch += trigger;
  batch += "\",";
  batch += "\"uptime_ms\":";
  batch += std::to_string(now_ms);
  batch += ",\"listen_mode\":\"";
  batch += listen_mode;
  batch += "\",\"elapsed_s\":";
  batch += std::to_string(elapsed_s);
  batch += ",\"meters\":[";

  bool first = true;
  for (auto &kv : this->highlight_meter_stats_) {
    const uint64_t key = kv.first;
    MeterStats &st = kv.second;
    const uint32_t meter_id = (uint32_t)(key >> 8);
    const uint8_t mode_byte = (uint8_t)(key & 0xFF);
    char id_str[12];
    snprintf(id_str, sizeof(id_str), "%08" PRIu32, meter_id);
    const char *mode_str = (mode_byte == (uint8_t) LinkMode::C1) ? "C1" : ((mode_byte == (uint8_t) LinkMode::S1) ? "S1" : "T1");

    // Use dedicated 60min counters for summary_60min trigger to avoid
    // showing only the last 15min of data (count_window_time is reset every 15min).
    const bool is_60min = (std::strcmp(trigger, "summary_60min") == 0);
    const uint32_t count_window = is_60min ? st.count_window_60min    : st.count_window_time;
    const int32_t rssi_sum      = is_60min ? st.rssi_sum_window_60min : st.rssi_sum_window_time;
    const uint32_t rssi_n       = is_60min ? st.rssi_n_window_60min   : st.rssi_n_window_time;
    const uint32_t interval_sum_ms = is_60min ? st.interval_sum_window_60min_ms : st.interval_sum_window_time_ms;
    const uint32_t interval_n      = is_60min ? st.interval_n_window_60min      : st.interval_n_window_time;

    const int32_t win_avg_rssi = (rssi_n > 0) ? (rssi_sum / (int32_t) rssi_n) : 0;
    const uint32_t avg_interval_s = (st.interval_n > 0) ? (st.interval_sum_ms / st.interval_n) / 1000 : 0;
    const uint32_t win_avg_interval_s = (interval_n > 0) ? (interval_sum_ms / interval_n) / 1000 : 0;

    char entry[256];
    snprintf(entry, sizeof(entry),
             "%s{"
             "\"id\":\"%s\","
             "\"mode\":\"%s\","
             "\"count_window\":%u,"
             "\"count_total\":%u,"
             "\"avg_interval_s\":%u,"
             "\"win_avg_interval_s\":%u,"
             "\"win_interval_n\":%u,"
             "\"last_rssi\":%d,"
             "\"win_avg_rssi\":%d"
             "}",
             first ? "" : ",",
             id_str, mode_str,
             (unsigned) count_window,
             (unsigned) st.count,
             (unsigned) avg_interval_s,
             (unsigned) win_avg_interval_s,
             (unsigned) interval_n,
             (int) st.rssi_last,
             (int) win_avg_rssi);
    batch += entry;
    first = false;
  }
  batch += "]}";

  const std::string snapshot_topic = this->diag_topic_ + "/meter_snapshot";
  mqtt->publish(snapshot_topic, batch, static_cast<uint8_t>(0), false);
  ESP_LOGI(TAG, "METER SNAPSHOT / snapshot licznikow: trigger=%s meters=%zu", trigger, this->highlight_meter_stats_.size());
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
  if (this->diag_topic_.empty()) return;
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
  if (!this->diag_publish_summary_highlight_meters_) return;
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
                         : (key_mode == (uint8_t) LinkMode::C1) ? "C1"
                         : (key_mode == (uint8_t) LinkMode::S1) ? "S1" : "UNK";
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

}  // namespace wmbus_radio
}  // namespace esphome
