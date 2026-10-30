# 02i: Bind application playback streams to per-app channels

**Parent:** 02: Route an app playback channel through the engine to speakers

**What to build:** Replace the single shared app playback endpoint with persistent playback channels bound to application identity. Detect and route each application's PipeWire playback streams to its channel automatically. Preserve controls for individual playback streams as well as the combined app channel, so multiple streams from one application can be mixed independently and then controlled together.

**Blocked by:** 01: Install and control the Shatter engine

**Status:** ready-for-human

- [x] Each application identity has its own persistent playback channel; unrelated applications never share a channel by default.
- [x] All playback streams from the same application identity are assigned to that application's channel, including streams already active at engine startup and streams created later.
- [x] Playback-stream controls (at least gain and mute) apply before streams are summed; app-channel controls (at least gain and mute) apply to the combined app signal.
- [x] Automatic binding uses PipeWire graph events and application identity rather than polling operating-system processes or relying on PID as persistent identity.
- [x] `shatter --list-in` reports each app channel and its assigned playback streams; the CLI/control interface can address both stream-level and channel-level controls unambiguously.
- [x] When Shatter exits, application streams resume ordinary PipeWire routing as required by ADR 0001.
- [x] An isolated PipeWire/WirePlumber integration test covers two apps, multiple streams for one app, and automatic binding for a stream started after the engine.
- [x] The implementation follows ADR 0001's mixer topology, or ADR 0001 is revised with the rationale before adopting a different topology.

**Out of scope:** Physical capture channels and capture routing remain in Ticket 03.

## Context

At the time this ticket was created, the prototype exposed one `shatter.app_playback` sink and WirePlumber routed Brave directly to the default sink. That shared endpoint would merge apps before Shatter could preserve application identity. This ticket refines the app-playback work around a persistent channel per app and per-stream controls within each channel.


## Implementation notes

- Each PipeWire playback stream receives a Shatter ingress sink, preserving individual stream identity. The engine's single speaker output callback sums these stream rings into a preallocated per-channel scratch block, then applies app-channel controls to each combined signal before summing channels to the speaker output.
- Application identity is selected from `application.id`, then process binary, then application name; channel IDs are deterministic hashes of that identity. PipeWire registry events bind streams that already exist and streams created later.
- `--set-gain` and `--set-mute` accept `app <channel-id>` or `stream <pipewire-node-id>`. `--list-in` shows channel IDs and assigned stream node IDs.
- ADR 0001 now clarifies that per-stream ingress nodes feed the single realtime mixer and records shutdown fallback behavior.
- The isolated PipeWire/WirePlumber test covers startup and late stream binding, two application identities, two streams from one app, stream-slot reuse after removal, graph routing, app and stream gain/mute measurements, control commands, and fallback after stop.
- The engine retains up to 1,024 app channel identities for its lifetime and reuses playback-stream slots when streams disappear.
