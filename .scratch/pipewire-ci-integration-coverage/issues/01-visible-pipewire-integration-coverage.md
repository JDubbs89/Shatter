# 01: Make PipeWire integration coverage visible in CI

**What to build:** Release checks clearly show whether PipeWire routing and capture integration coverage ran, and a passing CI job cannot silently omit those tests because required tools were not found.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [x] CI installs or provisions every dependency required to configure and run the PipeWire integration suite.
- [x] Missing integration-test dependencies fail CI or produce an explicit, visible skip that cannot be mistaken for a complete passing suite.
- [x] The configured PipeWire CI job registers and runs the application playback and microphone capture integration tests.
- [x] Host or test setup failures are reported as failures with diagnostics, and CTest identifies each integration test by name instead of silently skipping it.
