# 03: Make control and saved show state backend-neutral

**What to build:** Users can issue Shatter commands and save or restore shows without those operations depending on PipeWire-specific identities or terminology.

**Blocked by:** 02: Put PipeWire behind an audio backend seam.

**Status:** ready-for-human

- [x] CLI and control-server operations use Shatter's backend-neutral audio model.
- [x] Persisted playback, capture, device-binding, and routing identities do not rely on ephemeral PipeWire identifiers.
- [x] Loading and saving existing shows preserves their current meaning and remains backward compatible.
- [x] Backend-specific limitations are represented clearly when a requested operation is unavailable.
