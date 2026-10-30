# Shatter

A single-user Linux desktop mixer daemon for persistent application and device routing. The initial target is Fedora with PipeWire; JACK applications are in scope when running through PipeWire's JACK compatibility layer.

## Project status

This repository is an early prototype. Shatter groups PipeWire playback streams into per-application channels and provides per-stream and per-channel gain and mute controls. App channels can be sent to named output channels, including the built-in `speakers` output, which follows the default playback sink. You can create additional outputs, route them to multiple playback sinks, and control each output's gain and mute independently. A selected PipeWire capture source feeds an independent capture channel with its own gain, mute, and optional sends.

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

The background engine discovers application playback streams and assigns each stream to a per-application channel. Use `shatter list in` to find app names, channel IDs, and stream names. Gain and mute accept either the current user-facing name or the corresponding ID. Rename an app channel or stream to set a display alias; this does not change app grouping or audio processing. Names must match exactly; if a name is ambiguous, use the channel or PipeWire node ID. Quote names that contain spaces.

```sh
shatter start
shatter restart
shatter status
shatter stop
shatter list in
shatter list out
shatter list all
shatter set gain app Brave 0
shatter set gain stream Brave-1 0
shatter set gain output Headphones -3
shatter set mute app Brave on
shatter set mute capture capture on
shatter set mute output Headphones off
shatter rename app Brave Browser
shatter rename stream Brave-1 "Browser music"
shatter list in # choose a capture source node ID
shatter set capture source 57
shatter set gain capture -6
shatter set capture send on
shatter make out Headphones
shatter assign out Headphones sink-57
shatter assign app Brave Headphones
shatter unassign app Brave speakers
shatter list out
shatter start profile
# use audio normally
shatter stop profile
```

The older dash-prefixed command forms remain accepted as compatibility aliases.

`shatter list out` shows Shatter outputs and available playback sinks. Use the generated `sink-<PipeWire node ID>` value shown there when routing an output. User-created outputs remain disconnected until you assign one to a playback sink. Use `shatter unassign out <output> <sink>` to detach it, and `shatter delete out <output>` to remove it.

Profiling requires the engine's PipeWire output to be ready. The stop command prints callback timing percentiles, callback budget overruns, process CPU time, dropped and unfilled ring-buffer frames, Shatter input ring occupancy, and PipeWire's reported device delay and queued audio. Unfilled ring frames can include idle or silent streams. Callback timing measures Shatter's processing work; ring occupancy, PipeWire delay, and queued audio are separate parts of the audio path, and this summary is not an end-to-end loopback latency measurement.

Gain controls range from -60 dB to +12 dB and are smoothed during playback. The capture channel's speaker send starts off and resets to off whenever its source is changed or removed. The engine creates one virtual PipeWire ingress sink for each playback stream and returns streams to ordinary PipeWire routing when stopped. Its control socket lives in `$XDG_RUNTIME_DIR/shatter/control.sock` and is accessible only to the current user.
