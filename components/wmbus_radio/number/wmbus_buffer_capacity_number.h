// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runtime control for the RAM MQTT outbox capacity (see ../mqtt_outbox.cpp).
// Declaring this under number: turns "mqtt_buffer_size" from a flash-only
// YAML value into something adjustable live - e.g. from ESPHome's own
// web_server: with auth: set, without wmbus_radio running its own HTTP
// server or handling authentication itself.
#pragma once

#include "esphome/components/number/number.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "../component.h"

namespace esphome {
namespace wmbus_radio {

class WMBusBufferCapacityNumber : public number::Number, public Component {
 public:
  void set_radio(Radio *radio) { this->radio_ = radio; }

 protected:
  void control(float value) override {
    if (this->radio_ == nullptr) {
      this->publish_state(value);
      return;
    }
    // Dragging this down below what is currently queued trims the overflow
    // immediately (see recompute_buffer_quotas_(), triggered from
    // set_mqtt_outbox_capacity() in component.h) - not silently, this logs
    // it the same way the periodic auto-size shrink does.
    const uint32_t dropped_before = this->radio_->get_mqtt_outbox_dropped_total();
    this->radio_->set_mqtt_outbox_capacity((size_t) value);
    const uint32_t trimmed = this->radio_->get_mqtt_outbox_dropped_total() - dropped_before;
    if (trimmed > 0) {
      ESP_LOGW("wmbus",
               "WMBus buffer_capacity lowered below what was queued: dropped %u message(s) / "
               "zmniejszono buffer_capacity ponizej tego, co bylo w kolejce: odrzucono %u wiadomosci",
               (unsigned) trimmed, (unsigned) trimmed);
    }
    // Echo back the effective (possibly clamped to mqtt_buffer_size) value
    // rather than the raw slider position, so the UI never claims a capacity
    // larger than what was actually compiled in.
    this->publish_state((float) this->radio_->get_mqtt_outbox_capacity());
  }

  Radio *radio_{nullptr};
};

}  // namespace wmbus_radio
}  // namespace esphome
