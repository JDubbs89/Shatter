# 05: Save and restore output channels and sends in configs

**What to build:** Users can save named configs containing output channels, physical sink bindings, and all input sends, then reload them at runtime or have the default config restored at startup and saved at stop.

**Blocked by:**

- 03: Add playback-stream and capture sends to output channels.
- 04: Route an output channel to multiple playback sinks.

**Status:** ready-for-human

- [x] Named config save and load round-trip output names, generated output identities, sink bindings, and app, stream, and capture sends.
- [x] Persist sink bindings using stable PipeWire device identity when available; runtime node IDs remain selectors rather than persistent identity.
- [x] Startup loads the default config while preserving the built-in `speakers` route and capture's send-off default when no saved override exists.
- [x] `shatter set default show <name>` persists the named show selected for the next engine start.
- [x] Stop saves the current output routing state to the default config.
- [x] Loading a config at runtime updates the output graph and sends without doing file I/O or graph changes in the real-time callback.
- [x] Automated coverage verifies config round-trip, missing sinks, and outputs or inputs that are not currently active.
