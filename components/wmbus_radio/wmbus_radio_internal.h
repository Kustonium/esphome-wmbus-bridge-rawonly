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
// SX1276-specific recovery path: if length cannot be derived from the initial
// probe, keep draining the raw stream until idle and let the packet parser make
// the final decision. Sized to cover long T1 telegrams (>255 B after decode).
#define WMBUS_RAW_DRAIN_MAX_BYTES (416)

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
