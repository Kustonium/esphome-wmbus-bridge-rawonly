// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace esphome {
namespace wmbus_radio {

// Status registers can change during SPI reads (TI SWRS061I, section 10.3).
// Require two consecutive observations, with a bounded wait even on a dead chip.
template<typename Read, typename Now, typename Delay>
bool cc1101_wait_state(Read read, Now now, Delay delay, uint8_t expected, uint32_t timeout_us) {
  const auto start = now();
  bool previous_match = false;
  do {
    const bool match = (read() & 0x1F) == expected;
    if (match && previous_match) return true;
    previous_match = match;
    delay();
  } while (now() - start < timeout_us);
  return false;
}

// Never intentionally flush a packet already acquiring sync or filling FIFO.
// Two passes also reject unstable RXBYTES/status observations. There remains
// an unavoidable interval between the last GPIO read and the SIDLE strobe.
template<typename Sync, typename Bytes, typename State>
bool cc1101_idle_refresh_safe(Sync sync, Bytes bytes, State state) {
  for (unsigned i = 0; i < 2; i++) {
    if (sync() || bytes() != 0 || (state() & 0x1F) != 0x0D) return false;
  }
  return !sync();
}

}  // namespace wmbus_radio
}  // namespace esphome
