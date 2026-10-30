# 08: Recover safely when the engine fails

**What to build:** If Shatter's engine exits unexpectedly or stops processing, managed application playback returns to ordinary PipeWire routing, and the supervisor reports the failure and supports recovery.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [ ] Unexpected engine termination returns managed playback streams to ordinary PipeWire routing.
- [ ] The supervisor detects an unresponsive or exited engine and reports the failure.
- [ ] Restart behavior does not create duplicate engines or leave stale control sockets.
- [ ] Normal shutdown remains graceful and does not truncate playback abruptly.
- [ ] Isolated PipeWire integration coverage verifies crash, recovery, and restart behavior.
