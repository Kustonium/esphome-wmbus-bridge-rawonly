#include "runtime_settings.h"

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

}  // namespace runtime_settings
}  // namespace wmbus_radio
}  // namespace esphome
