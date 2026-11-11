# Contributing

[Polska wersja](CONTRIBUTING_PL.md)

Thanks for looking. Bug reports, tested board configs, measurements and code
are all welcome. Issues and discussions can be in English or Polish.

## How a change gets in

This repository publishes from a separate development tree, so a contribution
does not land through the pull request branch itself — it is ported there first
and arrives here as one squashed sync commit. Your commits will not appear in
this repository's history even though the code is yours. Authorship is recorded
in two other places instead: the port commit in the development tree names your
original commits, and the comment that closes your pull request states
explicitly that the code is yours.

For a small fix, open a pull request directly. For anything that changes how
the radio receives, open an issue or a discussion first, so we can agree on how
to measure it (see below).

## Where to start reading

- [docs/RX_PIPELINE.md](docs/RX_PIPELINE.md) — how a frame travels from the
  radio interrupt to MQTT.
- [docs/CHIP_SELECTION.md](docs/CHIP_SELECTION.md) — what each radio chip does
  well and where it struggles.
- `components/wmbus_radio/transceiver_<chip>.cpp` — one driver per chip
  (SX1262, SX1276, CC1101, LR1121). `packet.cpp` and `decode3of6.cpp` hold the
  frame parsing that every chip shares.
- [docs/DIAGNOSTIC.md](docs/DIAGNOSTIC.md) — what the diagnostic counters mean.

## Checking a change

**Parser tests on your computer**, no ESP needed — the same as CI:

```bash
mkdir -p build
g++ -std=c++17 -Wall -Wextra -Wno-unused-variable -Itests/host -Icomponents/wmbus_radio \
  tests/host/test_core.cpp components/wmbus_radio/decode3of6.cpp components/wmbus_radio/packet.cpp \
  -o build/test_core && ./build/test_core
python tests/ci/check_example_defaults.py
```

More on these in [tests/README.md](tests/README.md).

**Firmware build:** compile one of the `examples/` configs with ESPHome against
your checkout. CI builds every chip and board variant on each pull request.

## Changing how the radio receives

A change can make RSSI look better and still lose frames, or the other way
round. What counts is whether more frames are received and decoded, so please
measure it like this:

1. Keep a **second, unchanged receiver** running in the same place at the same
   time. Meters go quiet at night and some at weekends, so a "before" from
   yesterday is a different set of meters.
2. Compare **counts, not RSSI**: frames received, frames dropped and why, and
   which meters were heard. `diagnostic_mode: normal` publishes these every 60 s
   (see [docs/DIAGNOSTIC.md](docs/DIAGNOSTIC.md)).
3. If you use the companion Home Assistant add-on, its RX export lists every
   frame from every board with `frame_crc32`: the same transmission has the same
   CRC on every receiver, so you can see exactly which frames one board caught
   and the other missed.
4. **One change at a time**, and compare the same kind of day (working day with
   working day).

[docs/BENCHMARKS.md](docs/BENCHMARKS.md) shows measurements done this way.

## New options

- The default must keep today's behaviour.
- A user-facing option is described in the configuration docs, in English and
  Polish. CI checks that the defaults written in the examples and reference
  tables match the schema.

## A config for your board

A tested config for a board that is not in `examples/` yet is one of the most
useful contributions: add it under `examples/<chip>/<board>/` and say which
hardware you tested it on.

## Licence

GPL-3.0. See [docs/ATTRIBUTION.md](docs/ATTRIBUTION.md) for where the code comes
from. If AI helped you write a change, that is fine — please say so in the pull
request and say what you tested on real hardware.
