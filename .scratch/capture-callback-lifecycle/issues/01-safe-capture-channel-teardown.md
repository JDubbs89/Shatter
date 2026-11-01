# 01: Make capture channel teardown safe for audio callbacks

**What to build:** Removing or reusing a capture channel remains safe while PipeWire audio callbacks are running, without data races or access to a partially torn-down channel.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [x] Callback-visible capture channel state is synchronized with teardown and slot reuse; callback access remains bounded and nonblocking.
- [x] Teardown closes callback access, waits for active users, and only then resets or reuses channel state.
- [x] A regression test or focused stress test covers removal/reuse while output processing is active and verifies the callback never observes torn-down state.
- [x] PipeWire integration behavior remains correct when a capture source disappears and is rediscovered.
