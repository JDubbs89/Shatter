# PipeWire integration tests

The playback and capture routing integration tests create their own isolated
PipeWire and WirePlumber session. They do not use the user's active desktop
graph. They are registered when configuring with `BUILD_TESTING=ON` and
`SHATTER_AUDIO_BACKEND=PIPEWIRE`.

Configuration requires `pipewire`, WirePlumber 0.5.0 or newer, `pw-cli`, `wpctl`,
`pw-play`, `pw-record`, `pw-link`, and `python3`. CMake reports missing tools or an
unsupported WirePlumber version instead of silently omitting the integration
tests. Upgrade WirePlumber if the version check fails, or configure with
`-DBUILD_TESTING=OFF` to build without tests. Once configured, run them
with `ctest --test-dir <build-directory> --output-on-failure`; each test reports
setup failures as failures, not skips.
