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
#include <string>
#include <vector>

namespace esphome {
namespace wmbus_radio {
namespace runtime_settings {

// Nothing stored: the YAML value applies.
static constexpr uint8_t NONE = 0xFF;

// listen_mode as a ListenMode value (transceiver.h), or NONE.
uint8_t load_listen_mode();
bool save_listen_mode(uint8_t value);

// Any other one-byte setting, keyed by name (e.g. "wmbus_radio.cc1101_rx_bw").
// NONE = nothing stored. Saving NONE drops the stored value.
uint8_t load_u8(const char *key);
bool save_u8(const char *key, uint8_t value);

// One option the add-on may switch, kept as the index of its value in
// `options` (the values as written in YAML). The driver or component that owns
// it calls set_yaml() from its YAML setter and load() at the start of its
// setup(), then reads `value`. Everything the add-on sees and sends goes
// through json() / set(); patch() rewrites its line in the boot configuration
// report so the report shows what runs (YAML in brackets).
struct Option {
  Option(const char *key, const char *pref, std::vector<const char *> options, uint8_t offered = 0xFF)
      : key(key), pref(pref), options(std::move(options)), offered(offered) {}
  const char *key;
  const char *pref;
  std::vector<const char *> options;
  // Only the first `offered` values are offered as buttons (e.g. not the
  // developer preset); a YAML value beyond them is still shown and accepted.
  uint8_t offered;
  uint8_t yaml{0};
  uint8_t value{0};
  bool stored{false};

  void set_yaml(uint8_t v) { this->yaml = this->value = v; }
  // Applies a stored value; an invalid one is dropped.
  void load();
  // ',"<key>":{...}' for <diag>/settings.
  std::string json() const;
  // 0 = invalid value, 1 = already in effect, 2 = stored (restart to apply),
  // 3 = flash write failed. "yaml" drops the stored value.
  int set(const std::string &raw) const;
  void patch(std::vector<std::string> &lines) const;
};

}  // namespace runtime_settings
}  // namespace wmbus_radio
}  // namespace esphome
