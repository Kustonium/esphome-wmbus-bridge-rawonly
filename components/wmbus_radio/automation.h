// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kustonium
//
// EN: Part of esphome-wmbus-bridge-rawonly. This project was built as a
//     RAW-only RF->MQTT bridge inspired by ESPHome wM-Bus component work
//     from SzczepanLeon/esphome-components and related wmbusmeters code paths.
//     Some structure or naming may retain ancestry from that ecosystem.
// PL: Część projektu esphome-wmbus-bridge-rawonly. Projekt powstał jako
//     most RAW-only RF->MQTT inspirowany pracami ESPHome wM-Bus z repo
//     SzczepanLeon/esphome-components oraz powiązanymi ścieżkami wmbusmeters.
//     Część struktury lub nazewnictwa może zachowywać ten rodowód.

#pragma once

#include "component.h"
#include "esphome/core/automation.h"
#include "packet.h"

namespace esphome {
namespace wmbus_radio {
class FrameTrigger : public Trigger<Frame *> {
public:
  explicit FrameTrigger(wmbus_radio::Radio *radio, bool mark_handled) {
    radio->add_frame_handler([this, mark_handled](Frame *frame) {
      this->trigger(frame);
      if (mark_handled)
        frame->mark_as_handled();
    });
  }
};

} // namespace wmbus_radio
} // namespace esphome