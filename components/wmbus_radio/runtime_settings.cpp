// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime_settings.h"

#include <cctype>

#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace wmbus_radio {
namespace runtime_settings {

// One preference per setting, keyed by a fixed name so a firmware update keeps
// the stored value. Created on first use: global_preferences is ready by the
// time any component's setup() runs.
static ESPPreferenceObject &listen_mode_pref() {
  static ESPPreferenceObject pref =
      global_preferences->make_preference<uint8_t>(fnv1_hash("wmbus_radio.listen_mode"), true);
  return pref;
}

uint8_t load_listen_mode() {
  uint8_t value = NONE;
  if (!listen_mode_pref().load(&value)) return NONE;
  return value;
}

bool save_listen_mode(uint8_t value) {
  const bool ok = listen_mode_pref().save(&value);
  // A restart follows a change; flush now so it is not lost with it. save()
  // only queues the value on ESP32, so the flush is what says whether it
  // reached flash: on a failure the caller must not restart for nothing.
  const bool synced = global_preferences->sync();
  return ok && synced;
}

uint8_t load_u8(const char *key) {
  auto pref = global_preferences->make_preference<uint8_t>(fnv1_hash(key), true);
  uint8_t value = NONE;
  if (!pref.load(&value)) return NONE;
  return value;
}

bool save_u8(const char *key, uint8_t value) {
  auto pref = global_preferences->make_preference<uint8_t>(fnv1_hash(key), true);
  const bool ok = pref.save(&value);
  const bool synced = global_preferences->sync();
  return ok && synced;
}

void Option::load() {
  const uint8_t v = load_u8(this->pref);
  if (v == NONE) return;
  if (v >= this->options.size()) {
    save_u8(this->pref, NONE);
    return;
  }
  this->value = v;
  this->stored = true;
}

std::string Option::json() const {
  std::string out = std::string(",\"") + this->key + "\":{\"value\":\"" + this->options[this->value] +
                    "\",\"source\":\"" + (this->stored ? "stored" : "yaml") + "\",\"yaml\":\"" +
                    this->options[this->yaml] + "\",\"options\":[";
  bool first = true;
  for (size_t i = 0; i < this->options.size(); i++) {
    if (i >= this->offered && i != this->value && i != this->yaml) continue;
    if (!first) out += ",";
    first = false;
    out += std::string("\"") + this->options[i] + "\"";
  }
  out += "],\"apply\":\"restart\"}";
  return out;
}

int Option::set(const std::string &raw) const {
  std::string v;
  for (char c : raw) {
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t' && c != '"') v += (char) tolower((unsigned char) c);
  }
  uint8_t code = NONE;
  if (v != "yaml" && v != "default") {
    size_t i = 0;
    while (i < this->options.size() && v != this->options[i]) i++;
    if (i == this->options.size()) return 0;
    code = (uint8_t) i;
  }
  // The YAML value is not an override: it clears the stored one.
  if (code == this->yaml) code = NONE;
  const uint8_t stored_now = load_u8(this->pref);
  const uint8_t effective_after = code == NONE ? this->yaml : code;
  if (stored_now == code && this->value == effective_after) return 1;
  return save_u8(this->pref, code) ? 2 : 3;
}

void Option::patch(std::vector<std::string> &lines) const {
  if (!this->stored) return;
  const std::string head = std::string("  ") + this->key + ": ";
  for (auto &line : lines) {
    if (line.rfind(head, 0) == 0) {
      line = head + this->options[this->value] + " (CHANGED, stored by the add-on; YAML: " +
             this->options[this->yaml] + ")";
      return;
    }
  }
}

}  // namespace runtime_settings
}  // namespace wmbus_radio
}  // namespace esphome
