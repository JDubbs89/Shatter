# 04: Route an output channel to multiple playback sinks

**What to build:** Users can send one named output channel to multiple physical PipeWire playback sinks at once, such as speakers and headphones, and remove each sink route independently.

**Blocked by:** 02: Route app channels through named output channels.

**Status:** ready-for-agent

- [ ] A named output can be assigned to more than one physical playback sink concurrently.
- [ ] Unassigning one sink leaves the output connected to its other assigned sinks.
- [ ] Output and sink selectors resolve by name or ID and report ambiguity clearly.
- [ ] Output listings show generated Shatter sink IDs alongside PipeWire node IDs and current routes.
- [ ] Sink removal and reconnection update output routes without blocking or mutating state from the real-time callback.
- [ ] Automated PipeWire integration coverage verifies simultaneous signal delivery to two sinks and independent unassignment.
