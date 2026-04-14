# Migration note

If you previously used one of these old external component lists:

```yaml
components: [wmbus_common, wmbus_radio]
```

or

```yaml
components: [wmbus_bridge_common, wmbus_radio]
```

switch to:

```yaml
components: [wmbus_radio]
```

Current repo state:

- one public ESPHome component: `wmbus_radio`
- built-in RAW MQTT publish via `telegram_topic`
- `on_frame` kept only as an optional advanced callback
- diagnostics now also include the SX1276 adaptive busy-ether state topic (`diagnostic_topic/busy_ether_changed`) and the derived helper topic `diagnostic_topic/suggestion`

This matches the current repository layout and avoids stale cached names from older builds.
