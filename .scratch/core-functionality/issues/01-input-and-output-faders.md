# 01: Add faders to input and output channels

**What to build:** Users can control the level and mute state of each app playback channel, playback stream, capture channel, and output channel independently. The controls respond smoothly during playback and appear in input/output listings.

**Blocked by:** None (can start immediately).

**Status:** ready-for-human

- [x] App, playback-stream, and capture input channels expose independent gain and mute controls.
- [x] Each output channel exposes independent gain and mute controls that affect its delivered signal.
- [x] Fader updates are smoothed and safe to apply while audio is playing.
- [x] CLI commands and listings let users address controls unambiguously and inspect current values.
- [x] Automated coverage verifies control state and audible level/mute behavior through public interfaces.
