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
#include <cstdint>

namespace esphome {
namespace wmbus_radio {

enum class LinkMode : uint8_t {
  UNKNOWN = 0,
  T1 = 1,
  C1 = 2,
};

inline const char *link_mode_name(LinkMode m) {
  switch (m) {
    case LinkMode::T1:
      return "T1";
    case LinkMode::C1:
      return "C1";
    default:
      return "??";
  }
}

}  // namespace wmbus_radio
}  // namespace esphome
