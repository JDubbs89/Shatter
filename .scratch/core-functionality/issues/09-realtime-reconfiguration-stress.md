# 09: Verify realtime safety during reconfiguration

**What to build:** Routing and configuration changes can be exercised during playback without callback blocking, unsafe allocation, glitches, or excessive xruns.

**Blocked by:** 02: Keep playback routing stable through stream and device changes.

**Status:** ready-for-agent

- [ ] Stress coverage performs repeated routing and state reconfiguration while known audio is playing.
- [ ] Realtime callbacks do not allocate, block, or take contended locks during those changes.
- [ ] The test detects missing, duplicated, or corrupted audio and reports xruns.
- [ ] The stress scenario is repeatable in an isolated PipeWire test environment.
