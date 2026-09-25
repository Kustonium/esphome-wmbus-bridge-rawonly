// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared internals for the wmbus_radio component, split across component.cpp and
// its sibling translation units (rf_runtime, mqtt_publish, ...). Move-only
// refactor: definitions are relocated verbatim; values and behaviour unchanged.

#include "transceiver.h"  // ListenMode

// Protocol constants (were defined at the top of component.cpp).
#define WMBUS_PREAMBLE_SIZE (3)
#define WMBUS_MODE_C_PREAMBLE (0x54)
#define WMBUS_T1_LEN_PROBE_BYTES (18)
// Raw-stream drain cap, used by two paths: the SX1276 recovery path when a
// length cannot be derived from the initial probe, and the S1 receive path,
// which is Manchester-coded and so has no early length at all.
//
// 580 is the longest frame wM-Bus can put on air in any mode, not a guess. The
// L-field is one byte, so the largest link layer is L = 255: 17 blocks, 290
// bytes with the format-A CRCs. After line coding that is 435 raw bytes in T1
// (3-of-6, x1.5), 292 in C1 (no coding, plus the two mode-C indicator bytes)
// and 580 in S1 (Manchester, x2).
//
// Was 416, which covered neither S1's maximum nor even T1's 435 - a maximum
// length telegram was truncated by the cap rather than by anything physical.
// Found 2026-09-23 while setting up the S1 maximum-frame test; the driver-side
// DRAIN_CAP had already been raised to 640 and this one would have silently
// capped the result one layer up.
#define WMBUS_RAW_DRAIN_MAX_BYTES (580)

namespace esphome {
namespace wmbus_radio {

// Human-readable name for a listen mode. Used by several translation units (was
// a file-local static in component.cpp). static inline -> each TU gets its own
// copy with no ODR or unused-function warnings.
static inline const char *listen_mode_to_string_(ListenMode mode) {
  switch (mode) {
    case LISTEN_MODE_T1:
      return "T1 only";
    case LISTEN_MODE_C1:
      return "C1 only";
    case LISTEN_MODE_S1:
      return "S1 only";
    case LISTEN_MODE_BOTH:
    default:
      return "T1+C1 (both, 3:1 bias)";
  }
}

}  // namespace wmbus_radio
}  // namespace esphome
