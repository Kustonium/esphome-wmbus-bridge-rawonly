#pragma once
#include "transceiver.h"

namespace esphome {
namespace wmbus_radio {
class SX1276 : public RadioTransceiver {
public:
  void setup() override;
  optional<uint8_t> read() override;
  void restart_rx() override;
  int8_t get_rssi() override;
  const char *get_name() override;

 protected:
  // Bias towards Block B (0x3D). Every 4th hop switches to Block A (0xCD).
  uint8_t sync_cycle_{0};
};
} // namespace wmbus_radio
} // namespace esphome
