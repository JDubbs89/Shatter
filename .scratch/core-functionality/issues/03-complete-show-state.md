# 03: Persist complete show state robustly

**What to build:** Saving and loading a show restores the user's routing and control setup, including current channel and output faders, while safely handling older, invalid, or partially unavailable state.

**Blocked by:** Existing output-routing ticket 05: Persist output channels and sends; 01: Add faders to input and output channels.

**Status:** ready-for-agent

- [ ] Shows persist app, playback-stream, capture, and output channel identities and settings.
- [ ] Shows persist output sink bindings and all app, stream, and capture sends.
- [ ] Physical device bindings use stable identity when available and recover when devices are absent at load time.
- [ ] State format is versioned and supports safe migration from prior versions.
- [ ] Failed loads leave the current show usable and report actionable errors.
- [ ] Automated coverage verifies round-trip state, migrations, invalid files, and unavailable devices.
