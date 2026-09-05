// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runtime QoS control for the two RAM-buffered MQTT topics (the raw
// telegram publish and its /rx metadata companion - see ../mqtt_outbox.cpp
// for why exactly these two and not e.g. rssi/health/diagnostics). Turns
// telegram_qos/rx_qos from flash-only YAML values into something adjustable
// live (0/1/2) - e.g. from ESPHome's own web_server: with auth: set, the
// same "lightweight authenticated portal" WMBusBufferCapacityNumber already
// uses instead of this component rolling its own HTTP server or auth
// handling.
//
// One class serves both entities (telegram_qos: / rx_qos: under select:),
// distinguished by target_ - set once at codegen time via set_target().
//
// QoS is read per-message at the moment it is enqueued or published (see
// OutboxMsg::qos in component.h): a change here takes effect for the next
// message onward. Anything already sitting in the RAM buffer keeps whichever
// QoS it was queued with - buffering does not retroactively change it, and
// this entity does not walk the buffer to rewrite already-queued messages.
#pragma once

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"
#include "../component.h"

#include <cstdlib>
#include <string>

namespace esphome {
namespace wmbus_radio {

enum class WMBusQosTarget : uint8_t { TELEGRAM = 0, RX = 1 };

class WMBusQosSelect : public select::Select, public Component {
 public:
  void set_radio(Radio *radio) { this->radio_ = radio; }
  void set_target(WMBusQosTarget target) { this->target_ = target; }

 protected:
  void control(const std::string &value) override {
    if (this->radio_ == nullptr) {
      this->publish_state(value);
      return;
    }
    // Options are fixed to "0"/"1"/"2" (see __init__.py) but clamp anyway
    // rather than trust the string verbatim - atoi("") is 0, which is a safe
    // fallback if this is ever reached with something unexpected.
    const int parsed = std::atoi(value.c_str());
    const uint8_t qos = parsed < 0 ? 0 : (parsed > 2 ? 2 : (uint8_t) parsed);
    if (this->target_ == WMBusQosTarget::TELEGRAM) {
      this->radio_->set_telegram_qos(qos);
    } else {
      this->radio_->set_rx_qos(qos);
    }
    // Echo back the clamped value actually applied, same pattern as
    // WMBusBufferCapacityNumber - the portal should never show a value that
    // was not really accepted.
    this->publish_state(std::to_string((unsigned) qos));
  }

  Radio *radio_{nullptr};
  WMBusQosTarget target_{WMBusQosTarget::TELEGRAM};
};

}  // namespace wmbus_radio
}  // namespace esphome
