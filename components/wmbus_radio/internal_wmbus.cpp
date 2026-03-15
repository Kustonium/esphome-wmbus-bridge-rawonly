#include "internal_wmbus.h"

namespace esphome {
namespace wmbus_radio {

static const char *const TAG = "wmbus_internal";

bool isValidWMBusCField(int c_field) {
  // For the raw-only bridge we only need the C-fields seen in normal wM-Bus
  // data telegrams. Keeping this list intentionally small avoids dragging the
  // full upstream parser tree into this component.
  return c_field == 0x44 || c_field == 0x46;
}

void debugPayload(const std::string &intro, const std::vector<uint8_t> &payload) {
  ESP_LOGV(TAG, "%s \"%s\"", intro.c_str(), format_hex(payload).c_str());
}

FrameStatus checkWMBusFrame(std::vector<uint8_t> &data, size_t *frame_length,
                            int *payload_len_out, int *payload_offset,
                            bool only_test) {
  // This validator is intentionally minimal. At this stage the bridge has
  // already:
  //   1) identified a likely T1/C1 candidate,
  //   2) removed DLL CRC bytes,
  //   3) normalised the frame length field.
  //
  // So here we only need a cheap "does this still look like a full frame?"
  // check, plus a small out-of-sync recovery loop mirroring the upstream logic.
  debugPayload("(wmbus) checkWMBUSFrame", data);

  if (data.size() < 11) {
    ESP_LOGD(TAG, "(wmbus) less than 11 bytes, partial frame");
    return PartialFrame;
  }

  int payload_len = data[0];
  int type = data[1];
  int offset = 1;

  // Basic MBUS short/long frame guard copied from upstream behaviour.
  if (data.size() > 3 && data[0] == 0x68 && data[3] == 0x68 && data[1] == data[2]) {
    return PartialFrame;
  }

  if (!isValidWMBusCField(type)) {
    bool found = false;
    for (size_t i = 0; i + 1 < data.size(); ++i) {
      if (!isValidWMBusCField(data[i + 1]))
        continue;

      payload_len = data[i];
      const size_t remaining = data.size() - i;
      if (static_cast<size_t>(payload_len + 1) == remaining) {
        found = true;
        offset = static_cast<int>(i + 1);
        ESP_LOGV(TAG, "(wmbus) out of sync, skipping %zu bytes", i);
        break;
      }
    }

    if (!found) {
      if (!only_test) {
        ESP_LOGV(TAG, "(wmbus) no sensible telegram found, clearing buffer");
        data.clear();
      } else {
        ESP_LOGD(TAG, "(wmbus) not a proper wmbus frame");
      }
      return ErrorInFrame;
    }
  }

  *payload_len_out = payload_len;
  *payload_offset = offset;
  *frame_length = static_cast<size_t>(payload_len + offset);

  if (data.size() < *frame_length) {
    if (only_test) {
      // Keep the permissive upstream behaviour for offline/analyze-style use.
      payload_len = static_cast<int>(data.size()) - offset;
      *payload_len_out = payload_len;
      *frame_length = static_cast<size_t>(payload_len + offset);
      data[offset - 1] = static_cast<uint8_t>(payload_len);
      ESP_LOGW(TAG,
               "(wmbus) not enough bytes, adjusted frame length to available payload (%d)",
               payload_len);
      return FullFrame;
    }
    ESP_LOGD(TAG, "(wmbus) not enough bytes, partial frame have=%zu need=%zu",
             data.size(), *frame_length);
    return PartialFrame;
  }

  ESP_LOGV(TAG, "(wmbus) received full frame");
  return FullFrame;
}

}  // namespace wmbus_radio
}  // namespace esphome
