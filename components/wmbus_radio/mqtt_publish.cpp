// SPDX-License-Identifier: GPL-3.0-or-later
//
// MQTT publishing for the wmbus_radio component: target-topic derivation,
// raw-frame and forwarded-frame publishing, and the diagnostic/meter-window
// topic builders. Split out of component.cpp unchanged (move-only refactor);
// topic names, payloads and the MQTT contract are identical.

#include "component.h"
#include "meter_filter.h"
#include "wmbus_radio_internal.h"
#include "rx_metadata.h"

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/mqtt/mqtt_client.h"

#include <cstdio>
#include <string>
#include <ctime>
#include <esp_timer.h>
#include <sys/time.h>

namespace esphome {
namespace wmbus_radio {

static const char *TAG = "wmbus";

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


// Whitelist gate for the RAW telegram topic. Applied after decoding and DLL CRC,
// so the meter ID it matches on is one the parser already validated. The decision
// itself lives in meter_filter.h so tests/host can exercise it.
bool Radio::forward_meter_allowed_(uint32_t meter_id, uint32_t meter_id_raw) const {
  return meter_id_allowed(this->forward_meter_ids_, this->forward_meter_raw_ids_, meter_id, meter_id_raw);
}

void Radio::maybe_publish_radio_raw_(Packet *packet, uint32_t now_ms) {
  if (!this->publish_radio_raw_ || packet == nullptr) return;

  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  const std::string raw = packet->packet_hex();
  const char *chip = (this->radio != nullptr) ? this->radio->get_name() : "unknown";
  const char *listen_mode = (this->radio != nullptr) ? listen_mode_to_string_(this->radio->get_listen_mode()) : "unknown";
  const char *mode = link_mode_name(packet->get_link_mode());

  std::string payload = str_sprintf(
      "{\"event\":\"radio_raw\",\"uptime_ms\":%lu,\"chip\":\"%s\",\"listen_mode\":\"%s\",\"mode\":\"%s\",\"rssi\":%d,\"raw_len\":%u,\"hex_len\":%u,\"raw\":\"%s\"}",
      (unsigned long) now_ms,
      chip,
      listen_mode,
      mode,
      (int) packet->get_rssi(),
      (unsigned) packet->size(),
      (unsigned) raw.size(),
      raw.c_str());

  mqtt->publish("wmbus_bridge/raw", payload, static_cast<uint8_t>(0), false);
}

void Radio::maybe_forward_frame_(Frame &frame, uint32_t meter_id, uint32_t meter_id_raw, const char *id_str,
                                 const char *log_tag) {
  const bool want_all = !this->telegram_topic_.empty() && this->forward_meter_allowed_(meter_id, meter_id_raw);
  // target_meter_id keeps its own topic and is deliberately not filtered by the
  // whitelist: it is an explicit per-meter selection the user already made.
  const bool want_target = this->target_meter_enabled_ && meter_id == this->target_meter_id_;
  if (!want_all && !want_target) return;

  // Count every validated, whitelist-eligible frame, including frames received
  // while MQTT is disconnected. The next published sequence number then makes
  // that outage visible instead of silently compressing time.
  if (want_all) {
    this->rx_publish_seq_++;
    if (this->rx_publish_seq_ == 0) this->rx_publish_seq_ = 1;
  }

  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr) return;

  std::string hex = frame.as_hex();
  if (want_all) {
    // Not gated on mqtt->is_connected(): enqueue_or_publish_ publishes
    // immediately when connected (identical to the previous behaviour) and
    // buffers in RAM when not, instead of the frame being silently lost.
    // See mqtt_outbox.cpp for the buffering policy.
    this->enqueue_or_publish_(this->telegram_topic_, hex, this->telegram_qos_, false, meter_id, meter_id_raw);
    this->publish_rx_metadata_(frame, id_str, meter_id, meter_id_raw);

    // Keep the telegram payload contract unchanged. RSSI is a separate,
    // retained scalar and is published only when this frame carries a real
    // radio measurement (-126/-127 are sentinels, not signal levels).
    //
    // Deliberately NOT buffered: this topic is retained "latest known", not
    // an event stream, so a value queued during an outage could land after a
    // fresher one on reconnect. Skipping it here is fine - the next frame
    // (buffered or live) republishes it.
    const int rssi_dbm = (int) frame.rssi();
    if (this->publish_rssi_ && !this->rssi_topic_.empty() && id_str != nullptr && id_str[0] != '\0' &&
        rssi_dbm >= -125 && rssi_dbm <= -1 && mqtt->is_connected()) {
      const std::string topic = this->rssi_topic_ + "/" + id_str;
      mqtt->publish(topic, std::to_string(rssi_dbm), this->rssi_qos_, true);
    }
  }

  if (want_target && mqtt->is_connected()) {
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

// Wall-clock instant at which the frame was RECEIVED, not published.
//
// The two differ: the frame is captured in the receiver task and reaches MQTT a
// moment later, and if the broker is down it may not reach it at all until the
// connection returns. Stamping publish time would quietly relabel the frame,
// which is exactly the failure a timestamp is supposed to prevent.
//
// rx_task_wakeup_us is monotonic since boot, so the reception instant is
// "now, minus how long ago that wake-up was".
//
// Returns false when the clock has not been set yet. That is not an edge case:
// after a restart the radio receives normally for the seconds or minutes it
// takes SNTP to answer, and a frame from that window must arrive WITHOUT the
// field rather than carrying 1970 or an uptime pretending to be a date.
static bool frame_received_at_iso_(uint64_t rx_task_wakeup_us, char *out, size_t out_len) {
  struct timeval tv {};
  if (gettimeofday(&tv, nullptr) != 0)
    return false;

  const int64_t now_us = (int64_t) tv.tv_sec * 1000000 + (int64_t) tv.tv_usec;
  const int64_t age_us = (int64_t) esp_timer_get_time() - (int64_t) rx_task_wakeup_us;
  const int64_t at_us = now_us - (age_us > 0 ? age_us : 0);

  const time_t at_s = (time_t) (at_us / 1000000);
  // 2020-09-13. Below this the clock is unset or nonsense; ESP-IDF starts at
  // the epoch, so "not synced yet" and "genuinely 1970" are indistinguishable
  // and both are useless as a measurement.
  if (at_s < 1600000000)
    return false;

  struct tm tm_utc {};
  if (gmtime_r(&at_s, &tm_utc) == nullptr)
    return false;

  char base[24];
  if (std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm_utc) == 0)
    return false;

  const unsigned ms = (unsigned) ((at_us % 1000000) / 1000);
  return snprintf(out, out_len, "%s.%03uZ", base, ms) > 0;
}

void Radio::publish_rx_metadata_(Frame &frame, const char *id_str, uint32_t meter_id, uint32_t meter_id_raw) {
  // Not gated on is_connected(): this metadata (rssi_dbm + received_at,
  // "obok RSSI") is exactly the companion data a disconnect must not lose,
  // so it goes through the same RAM outbox as the telegram it describes
  // instead of being dropped when the broker is unreachable.
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || this->rx_topic_.empty() ||
      id_str == nullptr || id_str[0] == '\0')
    return;

  const auto &data = frame.data();
  const uint32_t crc = frame_crc32(data.data(), data.size());
  const int rssi = (int) frame.rssi();
  const std::string rssi_json = (rssi >= -125 && rssi <= -1) ? std::to_string(rssi) : "null";

  char received_at[32];
  const bool has_time = frame_received_at_iso_(frame.rx_task_wakeup_us(), received_at, sizeof(received_at));

  // Absent, not null: a consumer that never sees the key cannot mistake a
  // placeholder for a measurement.
  std::string received_at_json;
  if (has_time) {
    received_at_json = std::string(",\"received_at\":\"") + received_at + "\"";
  }

  char payload[384];
  snprintf(payload, sizeof(payload),
           "{\"schema\":1,\"boot_id\":\"%08lX\",\"seq\":%lu,"
           "\"rx_task_wakeup_us\":%llu,\"meter_id\":\"%s\",\"mode\":\"%s\","
           "\"rssi_dbm\":%s,\"frame_crc32\":\"%08lX\",\"frame_length\":%u%s}",
           (unsigned long) this->rx_boot_id_, (unsigned long) this->rx_publish_seq_,
           (unsigned long long) frame.rx_task_wakeup_us(), id_str,
           link_mode_name(frame.link_mode()), rssi_json.c_str(),
           (unsigned long) crc, (unsigned) data.size(), received_at_json.c_str());

  this->enqueue_or_publish_(this->rx_topic_, std::string(payload), this->rx_qos_, false, meter_id, meter_id_raw);
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

std::string Radio::meter_window_topic_for_(const char *id_str, const char *trigger, const char *mode_str) const {
  if (this->diag_topic_.empty() || id_str == nullptr || id_str[0] == '\0') return {};
  const char *trig = (trigger != nullptr && trigger[0] != '\0') ? trigger : "unknown";
  const char *ms = (mode_str != nullptr && mode_str[0] != '\0') ? mode_str : "unknown";
  // Topic includes mode so dual-mode meters (T1+C1 same ID) get separate paths.
  return this->diag_topic_ + "/meter/" + std::string(id_str) + "/" + ms + "/window/" + trig;
}

}  // namespace wmbus_radio
}  // namespace esphome
