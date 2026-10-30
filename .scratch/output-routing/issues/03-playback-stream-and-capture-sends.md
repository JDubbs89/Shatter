# 03: Add playback-stream and capture sends to output channels

**What to build:** Users can independently route playback streams and the capture channel to named output channels, alongside app-channel sends. Each send contributes independently, so sending both a stream and its parent app channel to one output adds both signals.

**Blocked by:** 02: Route app channels through named output channels.

**Status:** ready-for-agent

- [ ] `shatter assign stream <input-selector> <output-selector>` and `unassign` manage a playback stream's send.
- [ ] `shatter assign capture <input-selector> <output-selector>` and `unassign` manage the capture channel's send.
- [ ] Playback stream sends tap stream audio before app-channel gain; app-channel sends use the combined post-stream signal.
- [ ] App, playback-stream, and capture sends can be enabled independently to multiple named outputs.
- [ ] Capture remains unsent by default, and removing or changing its source does not enable a send implicitly.
- [ ] Input listings show each input channel's output sends.
- [ ] Automated coverage demonstrates independent and additive sends for all input channel types.
