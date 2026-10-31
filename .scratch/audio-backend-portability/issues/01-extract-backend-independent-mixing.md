# 01: Extract backend-independent realtime mixing

**What to build:** Shatter's existing playback and capture mixing behavior runs through processing code that does not depend on PipeWire buffers.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [ ] Playback and capture mixing calculations are separated from PipeWire buffer acquisition and release.
- [ ] Existing routing, gain, mute, and send behavior is preserved.
- [ ] The extracted processing behavior can be exercised without a PipeWire server.
- [ ] Realtime processing remains bounded and does not allocate, block, or lock in callbacks.
