# Source structure and seams

Shatter has three layers: Control, Mixer, and Backend. Platform-specific control
service code is separate from platform-specific audio integration. The platform service is chosen at build time. Linux builds include both PipeWire
and PulseAudio; Windows builds include WASAPI. The daemon detects the running
audio server and restarts itself when the selected server changes.

## Header and source convention

This repository builds an executable, not an installed C++ SDK. All project
headers live under `src/`, beside the implementation they describe:

```text
src/
  main.cpp
  control/
    cli.hpp / cli.cpp
    service.hpp / service.cpp
    backend_watcher.hpp / backend_watcher.cpp
    platform/
      service.hpp
      linux/service.cpp
      windows/service.cpp
    surfaces/
      midi.cpp
      osc.cpp
  mixer/
    control.hpp
    engine.hpp
    audio_engine.hpp
    channel_controls.hpp / channel_controls.cpp
    presentation.hpp / presentation.cpp
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
    iaudio_backend.hpp
    endpoint.hpp
    selection.hpp / selection.cpp
    providers.hpp
    detection_linux.cpp
    detection_windows.cpp
    unavailable_audio_backend.cpp
    pipewire/
      pipewire_backend.hpp / pipewire_backend.cpp
      factory.cpp
      audio_callbacks.cpp
      registry.cpp
      endpoints.cpp
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
- Native SDK headers belong in `backend/<provider>/` and platform detection
  implementations under `backend/`, or in
  `control/platform/<os>/` for service transport. Cross-layer interfaces contain
  no native SDK types.

## Control

`control/cli.cpp` parses arguments, sends commands, and manages the daemon
lifecycle. `main.cpp` only enters the CLI. The existing `--engine` process mode
enters `control::run_service()`.

`control/service.cpp` runs the persistent command loop and dispatches the private
local command protocol through `mixer::IControl`, created by
`backend::create_engine(server.kind)`. It owns command parsing,
reply framing, current/startup show tracking, and the stop sequence. It does not
handle sample buffers or include native audio headers. The same command operations
remain available to future control surfaces; OSC and MIDI files are placeholders,
not implemented interfaces or a versioned public network API.

`control/platform/service.hpp` is the OS service seam: accept/send/receive local
commands, launch/restart the detached process, and obtain terminal width. Its Linux
adapter uses a private Unix socket; its Windows adapter uses a per-user named
pipe. These remain user-session processes, not new system-service integrations.

## Mixer

`mixer/control.hpp` defines `IControl`, the virtual application command interface.
`mixer/engine.hpp` implements it as `Engine<Backend>`, owning exactly one concrete
backend. `IControl::backend()` exposes that same object's `IAudioBackend`
interface for non-realtime discovery and lifecycle. Backend classes implement
`IAudioBackend` directly; they do not inherit application gain, mute, show, or
presentation methods from a broad audio base class.

`mixer/channel_controls.cpp` owns selector policy, gain/mute, rename, fader
validation and clamping, internal send-mask updates, meter queries, and shutdown
fade requests. It borrows the portable processing state and the native adapter's
structural mutex. `Engine<Backend>` routes supported mixer commands here; the
prototype backends retain their previous unsupported-feature replies. Native
creation/routing/show coordination stays with concrete adapter methods, bound by
the selected engine specialization rather than required by `IAudioBackend`.

`mixer/audio_engine.hpp` defines the separate realtime `AudioEngine<Backend>`
template. Its C++20 input/output concepts require nonthrowing buffer acquisition,
release/submission, sample access, frame counts, and output-slot lookup. Concrete
backend operations are statically dispatched; this path never uses a virtual
backend pointer. An optional concrete timing hook preserves native profiling.
Template definitions remain in the header to allow specialization and inlining;
this does not guarantee inlining of native library calls or the separately
compiled processor/DSP implementation.

The callback paths are independent:

```text
Native input callback -> AudioEngine<Backend> -> acquire input -> Processor input -> release input
Native output callback -> AudioEngine<Backend> -> acquire output -> Processor output -> submit output
```

A successfully acquired buffer must be returned once even if its sample pointer
is null. An empty acquisition owns nothing. Process gates surround acquisition,
processing, and release; output generations are captured before input acquisition.
This preserves callback lifetime and output-slot reuse protection.


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

`backend/iaudio_backend.hpp` defines the small virtual interface: start, stop,
availability, and structured endpoint enumeration. `endpoint.hpp` defines Shatter
endpoint IDs, native identities, names, roles, optional default status, and formats.
Enumeration replaces the caller's snapshot only on success. Unknown native
formats/defaults remain unspecified; the processor's 48 kHz stereo format is not
reported as a device capability. Enumeration currently covers each adapter's
implemented discovery scope; capture enumeration is not added to the playback-only
prototypes. Discovery IDs for transient streams are not persistent identities.

CLI listing methods retain their previous text for compatibility. Future control
surfaces can use structured enumeration through `IAudioBackend` without parsing
those tables. `backend/selection.cpp` selects a provider factory at runtime. Each factory
constructs a compiled `Engine<ConcreteBackend>` specialization and returns
`IControl`. Template specialization remains a compile-time operation; runtime
selection chooses between those already compiled types.

`PipeWireBackend` is consistently named with `PulseAudioBackend` and
`WasapiBackend`. Its implementation is split by responsibility:

| File | Responsibility |
| --- | --- |
| `pipewire_backend.cpp` | Connection/control-loop lifecycle, discovery-to-channel reconciliation, show coordination, compatibility listings and native diagnostics |
| `registry.cpp` | Observed nodes, ports, links, metadata, and removal callbacks |
| `virtual_devices.cpp` | Playback ingress sinks and output endpoint lifecycle |
| `routing.cpp` | Control-loop requests, native links, output assignments, and applying shows to discovered endpoints |
| `audio_callbacks.cpp` | Concrete buffer acquisition/release/submission and timing operations, plus native callbacks entering `AudioEngine<PipeWireBackend>` |
| `endpoints.cpp` | Translate registry observations into structured Shatter endpoints |
| `factory.cpp` | Construct the mixer engine owning the selected adapter |

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
       -> shatter_backend (all adapters for the platform)
            -> shatter_mixer
```

The backend target receives the platform's native audio SDK dependencies: both
libpipewire and libpulse on Linux, and Windows audio libraries on Windows.
The compiled `shatter_mixer` library has no native audio SDK dependency. Its portable controls, processor, and DSP do
not depend on concrete adapters. The assembly template in `mixer/engine.hpp`
knows the SDK-free `IAudioBackend` contract and is instantiated in the selected
provider's factory; this is why the link graph has Backend depending on Mixer,
while application commands enter the mixer engine first.

`backend/detection_linux.cpp` makes bounded native protocol probes, preferring
PipeWire over PulseAudio and disabling PulseAudio autospawn. It compares server
cookies, rather than endpoint lists, to distinguish server instances. Windows
tracks the Windows Audio service and its process ID. An unavailable server selects
the no-audio adapter while the command service stays accessible.

`control/backend_watcher.cpp` polls on a separate non-realtime thread once per
second. A different selection must remain stable for two seconds, and the daemon
must have run for at least five seconds, before it requests a restart. Device
hotplug and default-device changes within the same server do not cause restarts.
The watcher sends a private wake-up command without waiting for a reply. Only
the daemon command thread saves the startup show, fades and stops the engine,
and destroys native state and transport. Linux then executes `/proc/self/exe`
in the same process, preserving systemd's main PID. Windows launches a replacement
process. An explicit stop stops the watcher and never requests replacement.
Existing show-loading and autosave rules apply; unsupported adapters cannot
persist or restore mixer features they do not implement. Restart detection does
not confer feature parity on PulseAudio or WASAPI.

For PipeWire, the daemon main thread dispatches commands and performs file I/O.
A separate PipeWire control-loop thread handles discovery and structural requests.
PipeWire-managed realtime threads run native buffer callbacks and call the mixer.
Meter reads use atomic publication; there is no dedicated meter thread. PulseAudio
uses its own threaded mainloop and routing worker. Windows service transport is
independent of WASAPI discovery.

## Verification

Existing unit and integration coverage remains under `tests/`. The portable
processor has direct sample-to-sample coverage in `src/mixer/processor_test.cpp`.
`audio_engine_test.cpp` checks the static buffer seam, including invalid buffers,
closed gates, and slot reuse. `engine_test.cpp` checks virtual control/discovery
against the same concrete backend, shared controls, and discovery-only adapters
without mixer/buffer stubs. These tests use an in-memory adapter without an audio
SDK. `backend/contract_test.cpp` exercises pre-start discovery on the actual
runtime-selected adapter. PipeWire playback/capture integration
scripts cover the native-to-mixer path and persistence; PulseAudio integration
covers its existing routing behavior. Windows lifecycle and installer checks
remain in the Windows CI configuration.

`backend_selection_test` verifies debounce, minimum lifetime, server replacement,
and recovery without timed sleeps. `automatic_backend_restart` exercises one
Linux binary through isolated PulseAudio and PipeWire servers, no-server recovery,
same-provider replacement, show recovery, and explicit shutdown.
