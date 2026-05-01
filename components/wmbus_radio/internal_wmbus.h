#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "link_mode.h"

namespace esphome {
namespace wmbus_radio {

// Minimal helpers extracted locally for the raw-only bridge.
//
// Goal: keep `wmbus_radio` self-contained so the YAML can continue to load a
// single public component (`components: [wmbus_radio]`) without exposing extra
// top-level helper components like `wmbus_common`.
//
// We intentionally keep only the tiny subset needed by the bridge parser:
// - LinkMode name helper used by log/output formatting
// - payload debug dump helper
// - minimal wM-Bus frame validation after DLL CRC removal
//
// This is NOT a full port of wmbusmeters/wmbus_common.

enum FrameStatus {
  PartialFrame,
  FullFrame,
  ErrorInFrame,
  TextAndNotFrame,
};

inline const char *linkModeName(LinkMode mode) { return link_mode_name(mode); }

bool isValidWMBusCField(int c_field);
void debugPayload(const std::string &intro, const std::vector<uint8_t> &payload);
FrameStatus checkWMBusFrame(std::vector<uint8_t> &data, size_t *frame_length,
                            int *payload_len_out, int *payload_offset,
                            bool only_test);

}  // namespace wmbus_radio
}  // namespace esphome
