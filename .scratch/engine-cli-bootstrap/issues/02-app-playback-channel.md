# 02: Route an app playback channel through the engine to speakers

**What to build:** An app can target a Shatter virtual playback endpoint, its audio enters a named input channel, and the engine can process it and send it to the physical playback destination. The CLI can change at least one channel parameter, such as gain. Ordinary playback resumes if the engine exits.

**Blocked by:** 01: Install and control the Shatter engine

**Status:** ready-for-human

- [ ] An app playback stream can be assigned to a persistent Shatter playback channel.
- [ ] Audio passes through that input channel and reaches the selected physical playback destination.
- [ ] A CLI command changes a channel parameter and the effect is audible.
- [ ] If the engine exits, ordinary PipeWire playback is restored as required by ADR 0001.
- [x] `shatter --list-in`, `--list-out`, and `--list-all` show Shatter endpoints and available PipeWire input/output devices.

## Comments

The isolated PipeWire smoke check confirmed that Shatter creates its app playback endpoint and links its speaker output to the current default sink; listing reported that route. Unit and CLI lifecycle tests pass. End-to-end sample capture, audible gain change, and app playback recovery after engine exit still need verification. The current speaker destination is the PipeWire default sink; selecting another destination from Shatter is not implemented.
