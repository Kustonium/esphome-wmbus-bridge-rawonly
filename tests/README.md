# Tests

This directory contains development-time checks for the component. They are not
included in ESPHome firmware and are only run by CI or a local host compiler.

## Host Parser Tests

`tests/host/test_core.cpp` builds selected parser code outside ESPHome and checks:

- 3-of-6 T-mode decoding,
- DLL CRC stripping for Format A and Format B,
- positive T1, C1, and S1 parser paths,
- rejection paths for bad CRC, truncated T1, bad C1 preamble, and invalid S1 Manchester,
- the `forward_meters` whitelist decision (`meter_filter.h`) and matching of non-BCD (hex) meter IDs,
- real RAW telegram golden samples from field captures.

The GitHub workflow at `.github/workflows/ci.yml` compiles and runs these tests
on Ubuntu.

## CI-only Configs

`tests/ci/` holds configurations that exist purely to widen CI coverage. They
are not examples and must not be flashed.

- `forward_meters_ci.yaml` — puts `forward_meters` through real ESPHome
  validation. The example configs never set the option, so without this entry
  the firmware matrix only ever compiled the default (empty) path. It declares
  two radios via `MULTI_CONF` so a single compile covers both the inherited
  (`forward_meters: true`) and the explicit-list form, each mixing a decimal
  and a quoted hex meter ID.

## Adding Golden Samples

Golden samples are normalized RAW HEX telegrams as published by
`wmbus/<device>/telegram`, after DLL CRC bytes have already been stripped by the
component.

To add another sample:

1. Open `tests/host/test_core.cpp`.
2. Add one entry to `golden_frame_fixtures()`:

```cpp
{
    "short-name",
    "RAW_HEX_WITHOUT_SPACES",
    EXPECTED_METER_ID,
},
```

3. Commit and push to `dev`.
4. Check that GitHub Actions is green.

The test verifies that the sample's L-field matches its byte length, the meter
ID is extracted as expected, and the parser can round-trip the sample through
synthetic C1 and T1 radio encodings back to the exact same normalized RAW HEX.
