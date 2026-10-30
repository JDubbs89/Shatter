# 02: Keep playback routing stable through stream and device changes

**What to build:** Application playback streams remain assigned to their intended channels and outputs as applications recreate streams or physical playback devices disconnect and reconnect.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [ ] Recreated playback streams are matched to the intended app channel using stable application identity.
- [ ] Stream removal and slot reuse do not transfer controls or sends to an unrelated stream.
- [ ] Playback device removal and reconnection preserve intended output bindings where identity is available.
- [ ] Listings accurately report active, missing, and reconnecting routes.
- [ ] Automated PipeWire integration coverage exercises stream recreation and device churn.
