#pragma once

// Language of the device log text, chosen at build time with `log_language:`
// (en by default). LOG_TR(en, pl) keeps exactly one of its two arguments in
// the firmware, so the other costs no flash. Both arguments must be string
// literals when used as (part of) a printf format, so the compiler still
// checks the format against its arguments; outside a format they may be any
// two expressions of the same type (e.g. the hint_en/hint_pl pointers).
//
// Only log text goes through this. MQTT payloads never do: the diag JSON
// carries hint_en and hint_pl side by side, and the add-on reads both.
#ifdef WMBUS_LOG_LANG_PL
#define LOG_TR(en, pl) pl
#else
#define LOG_TR(en, pl) en
#endif
