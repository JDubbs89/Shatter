# 05: Add peak and RMS meters

**What to build:** Users can view live peak and RMS levels for app playback channels, individual playback streams, capture, and output channels while audio is active.

**Blocked by:** None (can start immediately).

**Status:** ready-for-human

- [x] Meter readings are available for each active input and output channel.
- [x] Peak and RMS values use documented units and update at a usable rate.
- [x] Meter collection and publication do not block or allocate in realtime callbacks.
- [x] The CLI can inspect live meter values for selected channels.
- [x] Automated coverage checks meter response to known signal levels and silence.
