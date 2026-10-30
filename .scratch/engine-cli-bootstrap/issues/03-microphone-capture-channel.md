# 03: Add a microphone capture channel to the speaker mix

**What to build:** A physical capture device feeds a second input channel in the same engine, with its own processing controls. Its send to the speaker destination is explicit and initially off; turning it on routes the processed microphone signal to the speakers.

**Blocked by:** 02: Route an app playback channel through the engine to speakers

**Status:** ready-for-human

- [x] A selected PipeWire capture source feeds a distinct capture channel in the engine.
- [x] The capture channel has an independent gain control.
- [x] Its send to the speaker destination is off by default and can be enabled explicitly.
- [x] Enabling the send makes the processed capture signal audible at the speaker destination.

## Implementation notes

- Select a source with `shatter --set-capture-source <pipewire-node-id>`; set channel gain with `shatter --set-capture-gain <decibels>`; enable or disable the speaker send with `shatter --set-capture-send <on|off>`.
- `--list-in` shows available PipeWire capture sources and the selected source. Source changes and source removal leave the speaker send off.
- Verified in an isolated PipeWire/WirePlumber graph with a synthetic capture source and speaker monitor recording: send-off silence, send-on signal, and gain attenuation.
