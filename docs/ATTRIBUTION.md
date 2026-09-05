# Attribution and licensing

[Polska wersja](ATTRIBUTION_PL.md)

This repository is GPL-3.0-or-later.

It was inspired by the ESPHome wireless M-Bus component work from
`SzczepanLeon/esphome-components` and by related `wmbusmeters` code paths.
The current project is not a meter-decoding ESPHome all-in-one component. It is
a RAW-only RF->MQTT bridge: the ESP validates and forwards telegram HEX, while
meter decoding is intentionally left to `wmbusmeters` outside the ESP.

Some source files retain structural, naming, or code-level ancestry from the
original ecosystem. They are marked with SPDX headers. Newer parts of the
project add the RAW-only architecture, extended diagnostics, validated MQTT
publishing, and SX1262/SX1276-focused receive handling.

If you reuse code from this repository, keep the GPL-3.0-or-later license and
preserve attribution.
