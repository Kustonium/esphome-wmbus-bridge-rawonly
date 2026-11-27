# Support

[Polska wersja](SUPPORT_PL.md)

This project is maintained in spare time and is not a general support desk.

- **Best effort.** One person maintains it after hours. I answer when I can; there is no guaranteed response time.
- **Not a business.** I do not run a company and do not issue invoices. There is no paid support or service contract.
- **No warranty.** The code is provided under GPL-3.0-or-later, without warranty of any kind (see [LICENSE](LICENSE)). Readings used for billing are your responsibility.
- If the project helps you, [Buy Me a Coffee](https://buymeacoffee.com/Kustonium) is welcome but never required.

## Before opening an issue

Read:

- [README](README.md)
- [Start here](docs/START_HERE.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Diagnostics](docs/DIAGNOSTIC.md)
- [Radio options](docs/RADIO_OPTIONS_MINIMAL.md)
- [Chip selection](docs/CHIP_SELECTION.md)
- [Examples](examples/README.md)

## Open an issue for

- reproducible bugs in `wmbus_radio`,
- regressions between versions,
- broken examples from this repository,
- documentation mistakes.

## Do not open an issue for

- general ESPHome help,
- general Home Assistant help,
- MQTT basics, TLS setup or remote broker configuration,
- random YAML from forums,
- “it does not work” without logs and versions.

## If you report a bug, include

- board / hardware,
- radio type (`SX1262`, `SX1276`, `CC1101` or `LR1121`),
- ESPHome version,
- project version / release / commit,
- relevant YAML,
- startup log including the radio sanity report,
- `Have data / odebrano dane` lines if RX works locally,
- MQTT errors if publishing fails,
- diagnostic output if relevant,
- for RX problems: `summary` plus at least one `dropped` / `truncated` event if available,
- expected behaviour,
- actual behaviour.

## Scope reminder

This project is intentionally **RAW-only** and does not aim to replace `wmbusmeters` on the ESP.

MQTT connection details, including TLS certificates and remote brokers, belong
to ESPHome's standard `mqtt:` component. `wmbus_radio` only publishes when the
MQTT client is connected and keeps radio reception running when MQTT is unavailable.
