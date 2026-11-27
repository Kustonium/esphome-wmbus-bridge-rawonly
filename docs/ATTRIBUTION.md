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
publishing, SX1262/SX1276/LR1121/CC1101 receive paths with T1/C1 and S1
modes, and an ESP-NOW repeater example.

## Contributors

Code contributed by people outside the project. Each is the author of the
commit in the history and credited in the release notes.

| Who | Contribution | Where |
|---|---|---|
| Alexander Streit ([@probutus](https://github.com/probutus)) | `set_forward_meters()`: replace the forward list at runtime from your own lambda | [PR #40](https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/pull/40), release 1.37 |
| [@hackbaellchen](https://github.com/hackbaellchen) | SX1262: stream capture cut at the frame length in `listen_mode` `c1`/`both`, with a host test | [issue #45](https://github.com/Kustonium/esphome-wmbus-bridge-rawonly/issues/45), release 1.38 |

If you reuse code from this repository, keep the GPL-3.0-or-later license and
preserve attribution.
