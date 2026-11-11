#pragma once

// Runtime settings: radio options the add-on can change without a new
// firmware build. The board announces them on <topic>/diag/settings
// (retained) and takes changes on <topic>/set/<key>. A changed value is kept
// in flash and applied at the next start through the normal driver setup(),
// so the drivers keep a single configuration path.
//
// Only options that are register values belong here. Pins, the radio type and
// buffer sizes are fixed at build time and stay in YAML.

#include <cstdint>

namespace esphome {
namespace wmbus_radio {
namespace runtime_settings {

// Nothing stored: the YAML value applies.
static constexpr uint8_t NONE = 0xFF;

// listen_mode as a ListenMode value (transceiver.h), or NONE.
uint8_t load_listen_mode();
bool save_listen_mode(uint8_t value);

}  // namespace runtime_settings
}  // namespace wmbus_radio
}  // namespace esphome
