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

// A status register the radio updates while it is read (RXBYTES, MARCSTATE,
// PKTSTATUS) can come back corrupt from a single SPI read - datasheet
// section 10.3, about 80 ppm per read at the highest data rate, details in
// the CC1101 errata. Read until two consecutive reads agree. Bounded, so a
// value that keeps changing (bytes arriving) cannot spin forever: after
// max_reads the last read is returned, which is what a single read gave.
template<typename Read>
uint8_t cc1101_stable_read(Read read, unsigned max_reads = 6) {
  uint8_t value = read();
  for (unsigned i = 1; i < max_reads; i++) {
    const uint8_t again = read();
    if (again == value) return value;
    value = again;
  }
  return value;
}

}  // namespace wmbus_radio
}  // namespace esphome
