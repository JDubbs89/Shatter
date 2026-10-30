# 02: Route an app playback channel through the engine to speakers

**What to build:** An app can target a Shatter virtual playback endpoint, its audio enters a named input channel, and the engine can process it and send it to the physical playback destination. The CLI can change at least one channel parameter, such as gain. Ordinary playback resumes if the engine exits.

**Blocked by:** 01: Install and control the Shatter engine

**Status:** ready-for-agent

- [ ] An app playback stream can be assigned to a persistent Shatter playback channel.
- [ ] Audio passes through that input channel and reaches the selected physical playback destination.
- [ ] A CLI command changes a channel parameter and the effect is audible.
- [ ] If the engine exits, ordinary PipeWire playback is restored as required by ADR 0001.
