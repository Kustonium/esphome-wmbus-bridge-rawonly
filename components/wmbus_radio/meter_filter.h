// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace esphome {
namespace wmbus_radio {

// Meter-ID matching for `forward_meters` and `highlight_meters`.
//
// Deliberately free of ESPHome/FreeRTOS includes so tests/host can link it
// without the component runtime.
//
// A meter is addressed by two numbers, because the A-field is not always BCD:
//
//   meter_id     - BCD-decoded 8-digit ID, 0 when the A-field is not BCD.
//   meter_id_raw - the four A-field bytes as one value, in the order the log
//                  prints them (id:XXXXXXXX). Always available.
//
// Meters whose A-field is not BCD (Diehl/IZAR among others) only have the raw
// form, so matching on meter_id alone silently ignores them. Both lists must be
// sorted and deduplicated - parse_meter_id_csv_ does both.
//
// Note that a non-BCD A-field always contains a nibble > 9, so its printed form
// always contains a hex letter. Config tokens can therefore be classified
// without ambiguity: all-digits means BCD, anything with a-f means raw.
inline bool meter_id_in_lists(const std::vector<uint32_t> &bcd_ids,
                              const std::vector<uint32_t> &raw_ids,
                              uint32_t meter_id, uint32_t meter_id_raw) {
  if (meter_id != 0 && std::binary_search(bcd_ids.begin(), bcd_ids.end(), meter_id)) return true;
  if (meter_id_raw != 0 && std::binary_search(raw_ids.begin(), raw_ids.end(), meter_id_raw)) return true;
  return false;
}

// Whitelist decision for RAW telegram forwarding (`forward_meters`).
//
// With neither list configured nothing is filtered and everything is forwarded,
// which is the behaviour that predates this option. Otherwise a frame has to
// match one of the lists; a frame carrying no usable ID at all is rejected,
// because forwarding unidentified frames would defeat the whitelist.
inline bool meter_id_allowed(const std::vector<uint32_t> &bcd_ids,
                             const std::vector<uint32_t> &raw_ids,
                             uint32_t meter_id, uint32_t meter_id_raw) {
  if (bcd_ids.empty() && raw_ids.empty()) return true;
  return meter_id_in_lists(bcd_ids, raw_ids, meter_id, meter_id_raw);
}

}  // namespace wmbus_radio
}  // namespace esphome
