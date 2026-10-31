# 02: Put PipeWire behind an audio backend seam

**What to build:** Shatter uses PipeWire through an adapter, while the rest of the application accesses audio capabilities without depending on PipeWire types.

**Blocked by:** 01: Extract backend-independent realtime mixing.

**Status:** ready-for-agent

- [ ] Shatter's audio backend interface describes the audio operations the application currently requires.
- [ ] PipeWire-specific types and handles stay inside the PipeWire adapter.
- [ ] The application selects and uses the PipeWire adapter through the backend-neutral interface.
- [ ] Existing PipeWire routing and recovery behavior is preserved.
