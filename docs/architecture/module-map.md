# Module map

This map distinguishes active implementations from planned modules. Active modules are the current ownership points; planned modules describe follow-up extraction work, not stable interfaces.

## Process and API

- `src/main.cpp`: executable entry point.
- `src/app/supervisor.cpp`: command-line syntax, engine process lifecycle, and user-facing command output.
- `src/api/server.cpp`: local Unix-socket command server and client request transport. The command protocol is private to the executable; a versioned JSON-RPC API remains planned.
- `src/api/osc.cpp`: optional P1 OSC control surface.
- `src/control/midi.cpp`: optional P1 MIDI and hotkey control.

## Audio and PipeWire

- `src/engine/pipewire_engine.cpp`: PipeWire connection and main-loop lifecycle, app/stream/capture channel state, config application, list presentation, and profiling. It remains the largest active module; channel state and table rendering are coupled to the engine and need an explicit data interface before moving.
- `src/engine/mixer_node.cpp`: realtime input fan-out and per-output mixing callbacks. Callbacks must not block, allocate, or lock.
- `src/engine/dsp.cpp`: planned home for reusable channel, send, bus, and matrix processing that can be expressed independently of PipeWire buffers.
- `src/engine/fx_host.cpp`: future P1 LV2 host; not part of the initial routing and mixing proof.
- `src/meter/meter_bus.cpp`: realtime-safe meter collection and publication to non-realtime clients.
- `src/pw/core.cpp`: PipeWire registry callbacks and observed node/link graph updates. Connection lifecycle remains in `PipeWireEngine`.
- `src/pw/link_manager.cpp`: observed link state, Shatter output-to-sink bookkeeping, and control-loop operations for creating, deleting, and routing output channels.
- `src/pw/virtual_devices.cpp`: app-facing virtual playback stream registration and output stream lifecycle. Capture endpoint lifecycle remains in `PipeWireEngine`.
- `src/pw/rule_engine.cpp`: planned direction-specific app matching and routing decisions.

## State

- `src/state/store.cpp`: active show/config path validation, parsing, serialization, and atomic replacement. The engine snapshots live settings and applies loaded settings; file format concerns stay here.
- `src/state/scenes.cpp`: future scene support; scenes are deferred from the initial release scope.

## Tests and packaging

- `tests/unit/` and `tests/integration/`: unit and PipeWire/WirePlumber integration coverage.
- `packaging/systemd/`: planned user-session service packaging.
- `config/`: examples and documentation for user-editable rules/configuration. The schema is intentionally not invented in this scaffold.

## Remaining extraction work

1. Move channel state and list presentation behind explicit engine data interfaces; keep config file parsing and serialization in `state::Store`.
2. Move capture endpoint lifecycle and route reconciliation behind explicit interfaces; current PipeWire graph callbacks and output management live under `src/pw`.
3. Move reusable audio mixing math from the PipeWire callbacks into `dsp.cpp`, leaving buffer acquisition and release in `mixer_node.cpp`.
4. Keep CLI parsing in `supervisor.cpp` and command dispatch/transport in `api/server.cpp`; avoid adding persistence or file-format logic back to either module.

## Early proof points

1. Verify PipeWire/WirePlumber routing behavior for both playback and capture stream restoration.
2. Prototype minimal mixing and structural reconfiguration without realtime allocations, locks, or xruns.
3. Prove the daemon fails open to ordinary PipeWire routing after failure; this is a gate before expanding the mixer.
