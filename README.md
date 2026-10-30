# pwmixd

A single-user Linux desktop mixer daemon for persistent application and device routing. The initial target is Fedora with PipeWire; JACK applications are in scope when running through PipeWire's JACK compatibility layer.

## Project status

This repository currently contains the design spec and an architecture scaffold. The module translation units are placeholders; the audio engine, PipeWire integration, local API, and state handling are not implemented yet.

## Architecture and domain

- [`CONTEXT.md`](CONTEXT.md) defines the domain vocabulary.
- [`docs/adr/0001-realtime-mixer-and-failure-fallback.md`](docs/adr/0001-realtime-mixer-and-failure-fallback.md) records the realtime engine direction and the crash-fallback gate.
- [`docs/architecture/module-map.md`](docs/architecture/module-map.md) explains the planned source layout and module boundaries.
- [`PipeWire Mixer Backend Design Spec.md`](PipeWire%20Mixer%20Backend%20Design%20Spec.md) contains the broader feature and technology design.

## Build the scaffold

The initial scaffold only requires CMake and a C++20 compiler:

```sh
cmake -S . -B build
cmake --build build
```

PipeWire and other runtime dependencies will be added when their integration modules are implemented.
