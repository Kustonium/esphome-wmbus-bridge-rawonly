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
