#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace esphome {

inline std::string format_hex(const std::vector<uint8_t> &data) {
  static const char *hex = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (uint8_t byte : data) {
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 0x0F]);
  }
  return out;
}

}  // namespace esphome
