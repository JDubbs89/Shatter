# 02: Route app channels through named output channels

**What to build:** Users can create and remove named output channels, route an app playback channel to an output, and hear that app through the output's assigned playback sink. The built-in `speakers` output preserves the current default-speaker behavior.

**Blocked by:** None (can start immediately).

**Status:** ready-for-human

- [x] `shatter make out <name>` creates a named output channel with a PipeWire playback node.
- [x] `shatter delete out <name-or-pipewire-node-id>` removes the output channel and its PipeWire node.
- [x] `shatter assign out <output-selector> <sink-selector>` and `unassign` route or detach that output from one physical playback sink.
- [x] `shatter assign app <input-selector> <output-selector>` and `unassign` create or remove the app channel's send to that output.
- [x] Output listings show names, PipeWire node IDs, physical sink IDs, and current assignments in a readable table.
- [x] Playback app channels continue to reach the built-in `speakers` output by default; capture remains unsent by default.
- [x] Output lifecycle, routing changes, and graph operations stay off the real-time audio callback.
- [x] The app-to-output path has automated coverage for creation, assignment, unassignment, deletion, and audio delivery.
