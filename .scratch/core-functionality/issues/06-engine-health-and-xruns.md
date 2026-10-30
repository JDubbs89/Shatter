# 06: Report engine health and xruns

**What to build:** Users can inspect whether the engine is running, degraded, or failed open, and can see xrun counts and relevant latency information.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [ ] Engine health is queryable and distinguishes running, degraded, and failed-open states.
- [ ] Xruns are counted and reported without blocking realtime processing.
- [ ] Current I/O latency information is reported with its measurement source identified.
- [ ] CLI output makes health and xrun changes understandable.
- [ ] Automated coverage verifies state transitions and xrun reporting behavior.
