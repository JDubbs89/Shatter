# PipeWire integration tests

The playback and capture routing integration tests create their own isolated
PipeWire and WirePlumber session. They do not use the user's active desktop
graph. They are registered when configuring with `BUILD_TESTING=ON` and
`SHATTER_AUDIO_BACKEND=PIPEWIRE`.

Configuration requires `pipewire`, `wireplumber`, `pw-cli`, `wpctl`, `pw-play`,
`pw-record`, `pw-link`, and `python3`. CMake fails with the missing tool names
instead of silently omitting the integration tests. Once configured, run them
with `ctest --test-dir <build-directory> --output-on-failure`; each test reports
setup failures as failures, not skips.
