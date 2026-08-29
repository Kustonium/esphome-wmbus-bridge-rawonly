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

// Stable per-meter identity for anything that needs to bucket "the same
// meter" across separate calls - e.g. the RAM MQTT outbox's per-meter
// buffer_priority quotas (see mqtt_outbox.cpp).
//
// meter_id (BCD-decoded decimal) and meter_id_raw (raw A-field, always
// populated) live in two different numeric spaces: a BCD meter's decimal ID
// and some other meter's raw A-field value could coincidentally be the same
// 32-bit number without being the same meter. Tagging which space the value
// came from (bit 32/33) keeps them from ever colliding, at the cost of one
// extra bit - cheaper and simpler than converting between BCD and raw
// encodings to force everything into one namespace.
//
// meter_id is preferred when non-zero (BCD meter); meter_id_raw is used
// otherwise (non-BCD meter, where meter_id is always 0). This is consistent
// whether called with a fully-known runtime frame (both values available) or
// with only the one value a `buffer_priority` YAML entry supplies at config
// time (the other passed as 0) - the config-time entry for a BCD meter used
// its decimal id, so meter_id is non-zero there too; for a non-BCD meter it
// used the raw hex id, and meter_id is 0 there exactly like it is at runtime
// for that same meter's frames.
inline uint64_t meter_bucket_key(uint32_t meter_id, uint32_t meter_id_raw) {
  return meter_id != 0 ? ((uint64_t) 1u << 32 | meter_id) : ((uint64_t) 2u << 32 | meter_id_raw);
}

}  // namespace wmbus_radio
}  // namespace esphome
