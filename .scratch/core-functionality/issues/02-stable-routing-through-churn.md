# 02: Keep playback routing stable through stream and device changes

**What to build:** Application playback streams remain assigned to their intended channels and outputs as applications recreate streams or physical playback devices disconnect and reconnect.

**Blocked by:** None (can start immediately).

**Status:** ready-for-human

- [x] Recreated playback streams are matched to the intended app channel using stable application identity.
- [x] Stream removal snapshots gain, mute, name, and direct sends under the app identity and original stream name; recreated streams restore that state.
- [x] A removed stream slot is closed before teardown, so stream callbacks cannot mark a freed slot active or leak state into a different app.
- [x] Output sink bindings retain device serial or object path where available, use the sink name as a fallback, and recreate managed links when a bound sink reconnects.
- [x] Listings report only active playback streams, show app channels with no active streams, and mark disconnected output sinks as reconnecting.
- [x] Isolated PipeWire integration coverage exercises stream recreation, unrelated slot reuse, and playback sink removal and reconnection.
