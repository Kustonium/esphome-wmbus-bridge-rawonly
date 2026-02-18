# Migration note

If you previously used:
  components: [wmbus_common, wmbus_radio]

Switch to:
  components: [wmbus_bridge_common, wmbus_radio]

This avoids name collisions with older cached builds or other external components also named `wmbus_common`.
