// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one door to MQTT for the whole component.
//
// Every publish/subscribe in wmbus_radio goes through mqtt_client(). It returns
// nullptr - and every call site already treats nullptr as "no broker, skip" -
// in two cases:
//
//   1. repeater: true. The board is a radio front end only (e.g. a receiver at
//      the meter that hands every frame to on_frame -> ESP-NOW and has no
//      network of its own). It must not queue, count or log MQTT "outages":
//      nothing is lost, the frames go another way.
//   2. No mqtt: block in the YAML at all (allowed only with repeater: true, see
//      __init__.py). USE_MQTT is then undefined and the real client type does
//      not exist, so MqttClient below is a stub with the same call shapes.
//
// Keeping the gate here instead of at ~40 call sites means a new publish
// cannot forget it: there is no other way to reach the client.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "esphome/core/defines.h"

#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif

namespace esphome {
namespace wmbus_radio {

#ifdef USE_MQTT
using MqttClient = esphome::mqtt::MQTTClientComponent;
#else
// Never instantiated: mqtt_client() always returns nullptr without USE_MQTT.
// It exists only so the call sites type-check.
struct MqttClient {
  bool is_connected() { return false; }
  bool publish(const std::string &, const std::string &, uint8_t = 0, bool = false) { return false; }
  bool publish(const std::string &, const char *, size_t, uint8_t = 0, bool = false) { return false; }
  template<typename F> bool subscribe(const std::string &, F &&, uint8_t = 0) { return false; }
};
#endif

// Set once from the YAML (Radio::set_repeater) before setup() runs.
extern bool g_repeater_mode;

inline MqttClient *mqtt_client() {
#ifdef USE_MQTT
  if (g_repeater_mode) return nullptr;
  return esphome::mqtt::global_mqtt_client;
#else
  return nullptr;
#endif
}

}  // namespace wmbus_radio
}  // namespace esphome
