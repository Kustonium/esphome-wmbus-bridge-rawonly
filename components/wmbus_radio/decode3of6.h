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
#include <optional>
#include <vector>

namespace esphome {
namespace wmbus_radio {

// Diagnostics for 3-of-6 decoding (T-mode).
// symbols_total: how many 6-bit symbols were processed
// symbols_invalid: how many symbols were not in the valid 16-symbol table
struct Decode3of6Stats {
  uint16_t symbols_total{0};
  uint16_t symbols_invalid{0};
};

std::optional<std::vector<uint8_t>>
decode3of6(std::vector<uint8_t> &coded_data, Decode3of6Stats *stats = nullptr);
size_t encoded_size(size_t decoded_size);
} // namespace wmbus_radio
} // namespace esphome