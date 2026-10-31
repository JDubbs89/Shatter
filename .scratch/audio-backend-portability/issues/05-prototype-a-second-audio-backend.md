# 05: Prototype a second audio backend

**What to build:** Shatter demonstrates one real routing workflow through a second backend, with supported features and platform-specific gaps documented.

**Blocked by:** 03: Make control and saved show state backend-neutral; 04: Make the application build and run on multiple operating systems.

**Status:** ready-for-human

- [x] Linux and the PulseAudio protocol backend are selected as the second audio target.
- [x] The backend implements the shared audio backend interface.
- [x] An end-to-end test routes a live app playback stream to a selected sink through the CLI.
- [x] Supported playback operations and current feature gaps are documented.

The prototype depends on `pactl` and a running PulseAudio-compatible server. It lists active playback streams and sinks and moves matching active streams between physical sinks. It does not persist routing for future streams or support saved shows, capture routing, custom mixer outputs, gain, mute, sends, stream rename, or profiling. Run the `pulseaudio_playback_routing` integration test on Linux with a PulseAudio-compatible server, `paplay`, and the `module-null-sink` module available.
