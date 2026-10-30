# Shatter

A single-user Linux desktop mixer daemon for persistent application and device routing. The initial target is Fedora with PipeWire; JACK applications are in scope when running through PipeWire's JACK compatibility layer.

## Project status

This repository currently contains the design spec and an architecture scaffold. The module translation units are placeholders; the audio engine, PipeWire integration, local API, and state handling are not implemented yet.

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

The initial command manages the background engine process; audio routing and processing are not implemented yet.

```sh
shatter start
shatter status
shatter stop
shatter --list-in
shatter --list-out
shatter --list-all
shatter --set-gain app-playback -6
```

The app playback endpoint appears in PipeWire as `shatter.app_playback`. Route an app's playback stream to it using your desktop audio controls. Shatter sends the processed signal to the current PipeWire default playback sink. The engine control socket lives in `$XDG_RUNTIME_DIR/shatter/control.sock` and is accessible only to the current user.
