# 03: Add a microphone capture channel to the speaker mix

**What to build:** A physical capture device feeds a second input channel in the same engine, with its own processing controls. Its send to the speaker destination is explicit and initially off; turning it on routes the processed microphone signal to the speakers.

**Blocked by:** 02: Route an app playback channel through the engine to speakers

**Status:** ready-for-agent

- [ ] A selected physical capture device feeds a distinct capture channel in the engine.
- [ ] The capture channel has an independent processing control.
- [ ] Its send to the speaker destination is off by default and can be enabled explicitly.
- [ ] Enabling the send makes the processed capture signal audible at the speaker destination.
