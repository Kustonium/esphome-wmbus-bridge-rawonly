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
  if (mqtt == nullptr || !mqtt->is_connected()) return;

  std::string hex = frame.as_hex();
  if (want_all) {
    mqtt->publish(this->telegram_topic_, hex);
    this->publish_rx_metadata_(frame, id_str);

    // Keep the telegram payload contract unchanged. RSSI is a separate,
    // retained scalar and is published only when this frame carries a real
    // radio measurement (-126/-127 are sentinels, not signal levels).
    const int rssi_dbm = (int) frame.rssi();
    if (this->publish_rssi_ && !this->rssi_topic_.empty() && id_str != nullptr && id_str[0] != '\0' &&
        rssi_dbm >= -125 && rssi_dbm <= -1) {
      const std::string topic = this->rssi_topic_ + "/" + id_str;
      mqtt->publish(topic, std::to_string(rssi_dbm), static_cast<uint8_t>(0), true);
    }
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

void Radio::publish_rx_metadata_(Frame &frame, const char *id_str) {
  auto *mqtt = esphome::mqtt::global_mqtt_client;
  if (mqtt == nullptr || !mqtt->is_connected() || this->rx_topic_.empty() ||
      id_str == nullptr || id_str[0] == '\0')
    return;

  const auto &data = frame.data();
  const uint32_t crc = frame_crc32(data.data(), data.size());
  const int rssi = (int) frame.rssi();
  const std::string rssi_json = (rssi >= -125 && rssi <= -1) ? std::to_string(rssi) : "null";

  char payload[320];
  snprintf(payload, sizeof(payload),
           "{\"schema\":1,\"boot_id\":\"%08lX\",\"seq\":%lu,"
           "\"rx_task_wakeup_us\":%llu,\"meter_id\":\"%s\",\"mode\":\"%s\","
           "\"rssi_dbm\":%s,\"frame_crc32\":\"%08lX\",\"frame_length\":%u}",
           (unsigned long) this->rx_boot_id_, (unsigned long) this->rx_publish_seq_,
           (unsigned long long) frame.rx_task_wakeup_us(), id_str,
           link_mode_name(frame.link_mode()), rssi_json.c_str(),
           (unsigned long) crc, (unsigned) data.size());

  mqtt->publish(this->rx_topic_, std::string(payload), static_cast<uint8_t>(1), false);
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
