# 10: Add relative fader control and position status

**What to build:** Users can move an input or output fader by a relative dB amount, set it to its minimum with `-inf`, and inspect all fader positions in a two-column text table.

**Blocked by:** 01: Add faders to input and output channels.

**Status:** ready-for-human

- [x] `shatter set fader <app|stream|capture|output> <channel> <delta-dB|-inf>` adjusts the selected fader relative to its current level.
- [x] Positive and negative increments clamp to the supported fader range; `-inf` sets the fader to minimum.
- [x] `shatter fader status` displays channel names and text-art fader positions in a two-column table.
- [x] Status shows every app channel, active playback stream, capture channel, and output channel with the current dB value and range markers.
- [x] Automated coverage verifies relative movement, range clamping, `-inf`, and status output through the CLI and isolated PipeWire integration.
