# 03: Gate Windows releases on WASAPI tests

**What to build:** A version release includes a Windows build with WASAPI enabled only after the Windows unit, lifecycle, and available audio integration tests pass, alongside the Linux platform builds and tests.

**Blocked by:** 02: Route application playback through WASAPI.

**Status:** ready-for-agent

- [ ] Windows release configuration selects WASAPI instead of the no-audio backend.
- [ ] Release creation runs all registered CTest suites independently on Linux/PipeWire, Linux/PulseAudio, and Windows/WASAPI.
- [ ] Windows tests include the platform-control and CLI lifecycle suites plus WASAPI backend unit and integration coverage.
- [ ] The Windows package is uploaded and attached to the release only when all required platform jobs pass.
- [ ] The test output makes environment-dependent integration skips visible; required Windows backend coverage cannot silently disappear from CTest registration.
