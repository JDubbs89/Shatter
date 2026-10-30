# Shatter

A single-user Linux desktop mixer daemon for persistent application and device routing. The initial target is Fedora with PipeWire; JACK applications are in scope when running through PipeWire's JACK compatibility layer.

## Project status

This repository is an early prototype. The engine groups PipeWire playback streams into per-application channels, provides per-stream and per-channel gain and mute controls, and routes the mix to the PipeWire default playback sink. Capture routing and mix buses are not implemented yet.

## Architecture and domain

- [`CONTEXT.md`](CONTEXT.md) defines the domain vocabulary.
- [`docs/adr/0001-realtime-mixer-and-failure-fallback.md`](docs/adr/0001-realtime-mixer-and-failure-fallback.md) records the realtime engine direction and the crash-fallback gate.
- [`docs/architecture/module-map.md`](docs/architecture/module-map.md) explains the planned source layout and module boundaries.
- [`PipeWire Mixer Backend Design Spec.md`](PipeWire%20Mixer%20Backend%20Design%20Spec.md) contains the broader feature and technology design.

## Build

The build requires CMake, a C++20 compiler, pkg-config, and PipeWire development headers. On Fedora, install the PipeWire headers with `sudo dnf install pipewire-devel`.

```sh
cmake -S . -B build
cmake --build build
```

The executable can start without a PipeWire session, but audio routing requires PipeWire and WirePlumber to be running for the current user.

## Install the local command

Run the repository install script to build and install `shatter` into `~/.local/bin`:

```sh
sh install.sh
```

The background engine discovers application playback streams and assigns each stream to a per-application channel. Use `--list-in` to find app channel IDs and PipeWire stream node IDs, then adjust the whole app or an individual stream with `--set-gain` and `--set-mute`.

```sh
shatter start
shatter status
shatter stop
shatter --list-in
shatter --list-out
shatter --list-all
shatter --set-gain app app-id -6
shatter --set-gain stream 42 -6
shatter --set-mute app app-id on
shatter --profile-start
# use audio normally
shatter --profile-stop
```

Profiling requires the engine's PipeWire output to be ready. The stop command prints callback timing percentiles, callback budget overruns, process CPU time, dropped and unfilled ring-buffer frames, Shatter input ring occupancy, and PipeWire's reported device delay and queued audio. Unfilled ring frames can include idle or silent streams. Callback timing measures Shatter's processing work; ring occupancy, PipeWire delay, and queued audio are separate parts of the audio path, and this summary is not an end-to-end loopback latency measurement.

The engine creates one virtual PipeWire ingress sink for each playback stream and returns streams to ordinary PipeWire routing when stopped. Its control socket lives in `$XDG_RUNTIME_DIR/shatter/control.sock` and is accessible only to the current user.
