# 10: Add absolute and relative fader control with position status

**What to build:** Users can set an input or output fader to an absolute dB value, increment it by a relative dB amount, set it to minimum with `-inf`, and inspect all fader positions in a two-column text table.

**Blocked by:** 01: Add faders to input and output channels.

**Status:** ready-for-human

- [x] `shatter set fader <app|stream|capture|output> <channel> <dB|-inf>` assigns an absolute level.
- [x] `shatter inc fader <app|stream|capture|output> <channel> <delta-dB>` adjusts the selected fader relative to its current level.
- [x] Absolute values and increments clamp to the supported fader range; `-inf` is accepted by `set` to set the fader to minimum.
- [x] `shatter fader status` displays channel names and text-art fader positions in separate two-column app, stream, capture, and output groupings.
- [x] Status shows every app channel, active playback stream, capture channel, and output channel with the current dB value and range markers.
- [x] CLI and isolated PipeWire integration provide the fader command seam; coverage should verify absolute set, relative increment, range clamping, `-inf`, and grouped status output.
