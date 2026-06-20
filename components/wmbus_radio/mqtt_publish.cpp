// SPDX-License-Identifier: GPL-3.0-or-later
//
// MQTT publishing for the wmbus_radio component: target-topic derivation,
// raw-frame and forwarded-frame publishing, and the diagnostic/meter-window
// topic builders. Split out of component.cpp unchanged (move-only refactor);
// topic names, payloads and the MQTT contract are identical.

#include "component.h"
#include "wmbus_radio_internal.h"

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
