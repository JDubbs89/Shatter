# 01: Install and control the Shatter engine

**What to build:** You can install and run `shatter <command/args>` to start, inspect, and stop a background engine process. The CLI sends commands to the running engine over a local control connection and reports when it is unavailable.

**Blocked by:** None (can start immediately)

**Status:** ready-for-agent

- [x] A repository install script builds and installs `shatter` into the user's local command path.
- [x] `shatter start`, `shatter status`, and `shatter stop` manage a per-user background engine through a local Unix-domain control socket.
- [x] The engine reports readiness and exits cleanly on a stop command; status reports when it is not running.
- [x] The command works without an audio device or active PipeWire graph; audio processing is deferred to a later ticket.
