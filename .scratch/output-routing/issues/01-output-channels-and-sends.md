# 01: Add named output channels, sink routing, and input sends

**Type:** task
**Status:** ready-for-agent

Implement the requirements in [spec.md](../spec.md): named Shatter output channels backed by PipeWire playback nodes, assignment of outputs to physical playback sinks, and sends from app, capture, and playback-stream inputs to named outputs.

- Preserve audible startup behavior with the built-in `speakers` output.
- Allow one output to feed multiple sinks and one input to send to multiple outputs.
- Persist output definitions, sink bindings, and sends in named configs and the default config.
- Expose generated sink IDs and output names/node IDs in table listings.
- Keep all graph mutations and persistence work off the realtime callback.
- Update the domain docs and architecture decision to record output channels and realtime fan-out.
