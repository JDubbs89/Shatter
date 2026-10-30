# 07: Add real-time spectrum analysis

**What to build:** Users can inspect a live frequency spectrum for a selected input or output channel to understand its frequency content.

**Blocked by:** 05: Add peak and RMS meters.

**Status:** ready-for-agent

- [ ] Users can select an active channel for spectrum analysis.
- [ ] Spectrum data is published at a useful rate without blocking realtime audio processing.
- [ ] CLI output presents frequency bins and magnitudes in documented units.
- [ ] Analysis can be disabled and does not accumulate unbounded work or memory.
- [ ] Automated coverage verifies frequency-bin response using known test signals.
