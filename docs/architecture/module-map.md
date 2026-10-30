# Planned module map

This scaffold mirrors the design spec's layers. The `.cpp` files are placeholders, not implemented modules or stable APIs.

## Process and API

- `src/main.cpp`: executable entry point.
- `src/app/supervisor.cpp`: lifecycle, configuration, logging, service integration, health, and failure handling.
- `src/api/server.cpp`: documented local API and client subscriptions. The current direction is a versioned local JSON-RPC API over a Unix-domain socket; no network listener in v1.
- `src/api/osc.cpp`: optional P1 OSC control surface.
- `src/control/midi.cpp`: optional P1 MIDI and hotkey control.

## Audio and PipeWire

- `src/pw/core.cpp`: PipeWire connection and observed graph model.
- `src/pw/link_manager.cpp`: desired-versus-actual graph reconciliation.
- `src/pw/virtual_devices.cpp`: app-facing virtual playback and capture endpoints.
- `src/pw/rule_engine.cpp`: direction-specific app matching and routing decisions.
- `src/engine/mixer_node.cpp`: custom realtime `pw_filter` mixer node; callback must not block, allocate, or lock.
- `src/engine/dsp.cpp`: preallocated channel, send, bus, and matrix processing. Keep v1 DSP minimal.
- `src/engine/fx_host.cpp`: future P1 LV2 host; not part of the initial routing/mixing proof.
- `src/meter/meter_bus.cpp`: realtime-safe meter collection and publication to non-realtime clients.

## State

- `src/state/store.cpp`: persistent desired mixer state, commands, validation, and change publication.
- `src/state/scenes.cpp`: future scene support; scenes are deferred from the initial release scope.

## Tests and packaging

- `tests/unit/` and `tests/integration/`: planned unit and PipeWire/WirePlumber integration coverage.
- `packaging/systemd/`: planned user-session service packaging.
- `config/`: examples and documentation for user-editable rules/configuration. The schema is intentionally not invented in this scaffold.

## Early proof points

1. Verify PipeWire/WirePlumber routing behavior for both playback and capture stream restoration.
2. Prototype minimal mixing and structural reconfiguration without realtime allocations, locks, or xruns.
3. Prove the daemon fails open to ordinary PipeWire routing after failure; this is a gate before expanding the mixer.
