# Source structure and seams

Shatter has three layers: Control, Mixer, and Backend. Platform-specific control
service code is separate from platform-specific audio integration. The selected
audio backend and platform service are still chosen at build time; command syntax,
show formats, and backend capabilities are unchanged by this organization.

## Header and source convention

This repository builds an executable, not an installed C++ SDK. All project
headers live under `src/`, beside the implementation they describe:

```text
src/
  main.cpp
  control/
    cli.hpp / cli.cpp
    service.hpp / service.cpp
    platform/
      service.hpp
      linux/service.cpp
      windows/service.cpp
    surfaces/
      midi.cpp
      osc.cpp
  mixer/
    model.hpp
    processor.hpp / processor.cpp
    dsp.hpp / dsp.cpp
    audio_ring.hpp
    realtime_access_gate.hpp
    health.hpp
    identifiers.hpp
    runtime_profiler.hpp
    meter/meter_bus.hpp / meter_bus.cpp
    state/store.hpp / store.cpp
    state/scenes.cpp
    fx_host.cpp
    routing_rules.cpp
  backend/
    audio_backend.hpp
    unavailable_audio_backend.cpp
    pipewire/
      pipewire_backend.hpp / pipewire_backend.cpp
      factory.cpp
      audio_callbacks.cpp
      registry.cpp
      routing.cpp
      virtual_devices.cpp
    pulseaudio/
      pulseaudio_backend.cpp
      pulseaudio_client.hpp / pulseaudio_client.cpp
      pulseaudio_types.hpp
    wasapi/
      wasapi_backend.cpp
      wasapi_types.hpp
```

- Use matching snake_case basenames for `.hpp`/`.cpp` pairs.
- Include project headers relative to `src/`, for example
  `#include "mixer/processor.hpp"` or
  `#include "backend/pipewire/pipewire_backend.hpp"`. Do not use relative `../`
  includes or create a second header tree.
- A header-only module has only an `.hpp`. A class used entirely within one
  translation unit stays in that `.cpp`; it does not need a redundant header.
  PulseAudio and WASAPI backend classes currently follow this rule.
- Namespaces follow the layer: `shatter::control`, `shatter::mixer`, and
  `shatter::backend`. Nested mixer state/meter and control platform modules use
  corresponding nested namespaces.
- Native SDK headers belong in `backend/<provider>/`, or in
  `control/platform/<os>/` for service transport. Cross-layer interfaces contain
  no native SDK types.

## Control

`control/cli.cpp` parses arguments, sends commands, and manages the daemon
lifecycle. `main.cpp` only enters the CLI. The existing `--engine` process mode
enters `control::run_service()`.

`control/service.cpp` runs the persistent command loop and dispatches the private
local command protocol through `backend::AudioBackend`. It owns command parsing,
reply framing, current/startup show tracking, and the stop sequence. It does not
handle sample buffers or include native audio headers. The same command operations
remain available to future control surfaces; OSC and MIDI files are placeholders,
not implemented interfaces or a versioned public network API.

`control/platform/service.hpp` is the OS service seam: accept/send/receive local
commands, launch the detached process, and obtain terminal width. Its Linux
adapter uses a private Unix socket; its Windows adapter uses a per-user named
pipe. These remain user-session processes, not new system-service integrations.

## Mixer

`mixer/model.hpp` defines portable application, playback-stream, capture, and
output state, routing masks, gain/mute targets, smoothing state, meters, and audio
rings. It contains no PipeWire, libpulse, or Windows types.

`mixer/processor.hpp` is the sample-processing seam. Its implementation owns:

- Playback stream gain/mute and fan-out to independent output rings.
- Summing streams into application channels and applying application gain/mute.
- Explicit stream sends and physical capture contributions.
- Output gain/mute, shutdown fades, metering, and mixer xrun accounting.

The processor accepts interleaved stereo float samples at the existing 48 kHz
format. The adapter supplies buffers and schedules calls; the processor does not
create threads or control native endpoints. This preserves the existing callback
scheduling and mixing order.

`ProcessingState` borrows stable channel records and slot views. The adapter binds
these views before starting callbacks and owns their lifetime. PipeWire endpoint
records extend portable records with native handles so existing slot reuse and
callback userdata remain stable. The processor only sees the portable records.
Adapters must preserve single-producer/single-consumer ring ownership, publish
atomic controls, and close/wait/reopen the relevant gates before teardown or reuse.
Input callbacks snapshot output generations before acquiring native buffers;
this prevents an old buffer entering a reused output slot. Audio processing does
not allocate, lock, or wait.

`dsp.cpp` contains sample math; `meter/` publishes peak/RMS windows atomically.
`state/store.cpp` handles show paths, validation, parsing, serialization, and file
replacement. Native adapters still snapshot/apply settings while coordinating
endpoint lifecycle. `fx_host.cpp`, `routing_rules.cpp`, and `state/scenes.cpp`
remain deferred modules. General mix buses, matrices, and effects are not added
by this refactor; future implementations belong in this layer.

## Backend

`backend/audio_backend.hpp` retains the existing command-facing adapter contract
and build-selected factory. Each native implementation resolves its selectors,
translates available operations into mixer controls/native routing, and reports
unsupported features as before. Control includes this contract, never a concrete
backend header.

`PipeWireBackend` is consistently named with `PulseAudioBackend` and
`WasapiBackend`. Its implementation is split by responsibility:

| File | Responsibility |
| --- | --- |
| `pipewire_backend.cpp` | Connection/control-loop lifecycle, command translation, discovery-to-channel reconciliation, show coordination, listings and native diagnostics |
| `registry.cpp` | Observed nodes, ports, links, metadata, and removal callbacks |
| `virtual_devices.cpp` | Playback ingress sinks and output endpoint lifecycle |
| `routing.cpp` | Control-loop requests, native links, output assignments, and applying shows to discovered endpoints |
| `audio_callbacks.cpp` | Acquire/release PipeWire buffers, decode native buffer layout, query native timing, and call the portable processor |
| `factory.cpp` | Construct the selected adapter |

PulseAudio retains discovery and playback stream routing, including its libpulse
connection and routing worker. WASAPI retains synchronous session/endpoint
discovery. Neither gains custom mixing or other previously unsupported features.
The unavailable adapter retains the explicit no-audio behavior.

## Build dependencies and execution

CMake expresses the layers as separate static targets:

```text
shatter (main)
  -> shatter_control
       -> shatter_control_platform (selected Linux/Windows adapter)
       -> shatter_backend (selected audio adapter)
            -> shatter_mixer
```

Only the selected audio adapter receives its native audio SDK dependency.
`shatter_mixer` has no dependency on Control, Backend, or an audio SDK and builds
in the no-audio configuration too. The command-facing contract remains broad to
preserve existing backend behavior; a future common command model can replace
that contract without changing the portable processing seam.

For PipeWire, the daemon main thread dispatches commands and performs file I/O.
A separate PipeWire control-loop thread handles discovery and structural requests.
PipeWire-managed realtime threads run native buffer callbacks and call the mixer.
Meter reads use atomic publication; there is no dedicated meter thread. PulseAudio
uses its own threaded mainloop and routing worker. Windows service transport is
independent of WASAPI discovery.

## Verification

Existing unit and integration coverage remains under `tests/`. The portable
processor has direct sample-to-sample coverage in `src/mixer/processor_test.cpp`,
which builds without a native audio SDK. PipeWire playback/capture integration
scripts cover the native-to-mixer path and persistence; PulseAudio integration
covers its existing routing behavior. Windows lifecycle and installer checks
remain in the Windows CI configuration.
