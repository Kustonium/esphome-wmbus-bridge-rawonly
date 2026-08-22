// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace wmbus_radio {

// IEEE CRC-32 of the final normalized frame (the same bytes /telegram turns
// into HEX). Keeping this helper platform-neutral lets host CI verify that the
// identifier is stable without pulling ESPHome or MQTT into the test binary.
inline uint32_t frame_crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return crc ^ 0xFFFFFFFFU;
}

}  // namespace wmbus_radio
}  // namespace esphome
