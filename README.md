# Shatter

A single-user desktop mixer daemon for persistent application and device routing. Linux with PipeWire is the full audio target. A Linux PulseAudio backend prototype supports listing active playback streams and sinks and moving active application streams between sinks. Windows builds can run the command server and lifecycle with audio explicitly unavailable while a native audio backend is developed.

## Project status

This repository is an early prototype. Shatter groups PipeWire playback streams into per-application channels and provides per-stream and per-channel gain and mute controls. App channels can be sent to named output channels, including the built-in `speakers` output, which follows the default playback sink. You can create additional outputs, route them to multiple playback sinks, and control each output's gain and mute independently. Every available PipeWire capture source automatically gets its own input channel with independent gain, mute, meter, and sends.

## Architecture and domain

- [`CONTEXT.md`](CONTEXT.md) defines the domain vocabulary.
- [`docs/adr/0001-realtime-mixer-and-failure-fallback.md`](docs/adr/0001-realtime-mixer-and-failure-fallback.md) records the realtime engine direction and the crash-fallback gate.
- [`docs/architecture/module-map.md`](docs/architecture/module-map.md) explains the planned source layout and module boundaries.
- [`PipeWire Mixer Backend Design Spec.md`](PipeWire%20Mixer%20Backend%20Design%20Spec.md) contains the broader feature and technology design.

## Build

The Linux PipeWire build requires CMake, a C++20 compiler, pkg-config, and PipeWire development headers. On Fedora, install the PipeWire headers with `sudo dnf install pipewire-devel`. The PulseAudio build uses the native PulseAudio client library and requires its development headers. On Fedora, install them with `sudo dnf install pulseaudio-libs-devel`.

```sh
cmake -S . -B build
cmake --build build
```

The platform and audio backend are selected independently with `SHATTER_PLATFORM` and `SHATTER_AUDIO_BACKEND`. Linux defaults to the PipeWire backend. Windows defaults to `NONE` and does not require PipeWire; audio commands report that no backend is configured. PipeWire and PulseAudio are currently supported only on Linux.

```powershell
cmake -S . -B build -DSHATTER_PLATFORM=WINDOWS -DSHATTER_AUDIO_BACKEND=NONE `
  -DCMAKE_INSTALL_PREFIX="$env:LOCALAPPDATA/Programs/Shatter"
cmake --build build --config Release
```

Linux may also build the control service without an audio backend using `-DSHATTER_AUDIO_BACKEND=NONE`. A PipeWire session and WirePlumber are required for Linux audio routing.

## Install the local command

Run the build-install script to select the platform and audio backend, build only the selected implementation, and install `shatter`:

```sh
sh build-install.sh --platform linux --audio-backend pipewire
```

`--platform` accepts `auto`, `linux`, or `windows`; `--audio-backend` accepts `auto`, `pipewire`, `pulseaudio`, or `none`. `auto` uses the native platform and its default backend (PipeWire on Linux, no audio backend on Windows). The platform must match the native toolchain; this script does not cross-compile.

For a Windows build from a Windows shell with CMake available:

```sh
sh build-install.sh --platform windows --audio-backend none
```

The Windows install prefix defaults to `%LOCALAPPDATA%/Programs/Shatter`. Set `SHATTER_INSTALL_PREFIX` to choose a different location. Linux installs to `~/.local` by default.

### PulseAudio playback routing prototype

Install with `sh build-install.sh --platform linux --audio-backend pulseaudio`. Shatter requires a running PulseAudio-compatible server. Use `shatter list in` and `shatter list out`, then route active streams with `shatter assign app <application-name-or-binary> <sink-name-or-description>`. `shatter unassign app <application-name-or-binary> <sink>` returns matching active streams to the default sink.

This prototype only routes playback streams. App assignments cover streams created while Shatter is running, but they are not persisted across restarts. Saved shows, capture routing, custom mixer outputs, gain, mute, sends, stream rename, and profiling remain unsupported. The PipeWire backend remains the full-featured Linux implementation.

With the Linux PipeWire backend, the background engine discovers application playback streams and assigns each stream to a per-application channel. Use `shatter list in` to find app names, channel IDs, and stream IDs. Gain and mute accept either the current user-facing name or the corresponding ID. Rename an app channel or stream to set a display alias; this does not change app grouping or audio processing. Names must match exactly; if a name is ambiguous, use the listed Shatter ID. Stream selectors identify currently active streams and can change after a restart. Quote names that contain spaces.

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
shatter set fader app Brave -6
shatter inc fader app Brave +6
shatter set fader app Brave -inf
shatter fader status
shatter set mute app Brave on
shatter set mute capture "USB Microphone" on
shatter set mute output Headphones off
shatter rename app Brave Browser
shatter rename stream Brave-1 "Browser music"
shatter list in # capture devices appear as input channels with names and IDs
shatter set fader capture "USB Microphone" -6
shatter assign capture "USB Microphone" speakers
shatter make out Headphones
shatter make out "Virtual Microphone" capture
shatter assign output Headphones "Studio Headphones"
shatter assign app Brave Headphones
shatter unassign app Brave speakers
shatter assign capture "USB Microphone" "Virtual Microphone"
shatter list out
shatter start profile
# use audio normally
shatter stop profile
```

The older dash-prefixed command forms remain accepted as compatibility aliases.

`shatter list out` shows Shatter outputs and available playback devices. Routing accepts a displayed device name or the stable ID shown in the listing; if names are ambiguous, use the ID. User-created playback outputs remain disconnected until you assign one to a playback device. Create a capture-type output with `shatter make out <name> capture`; it appears to applications as a virtual microphone source, which each application can select in its input settings. Every physical capture source appears as an input channel in `shatter list in`, so `shatter assign capture "USB Microphone" "Virtual Microphone"` routes it directly without selecting it first. `shatter assign output Headphones "Studio Headphones"` routes a playback output to a device. Use `shatter unassign <type> <source> <destination>` to remove a route, and `shatter delete out <output>` to remove an output channel.

Profiling requires the engine's PipeWire output to be ready. The stop command prints callback timing percentiles, callback budget overruns, process CPU time, dropped and unfilled ring-buffer frames, Shatter input ring occupancy, and PipeWire's reported device delay and queued audio. Unfilled ring frames can include idle or silent streams. Callback timing measures Shatter's processing work; ring occupancy, PipeWire delay, and queued audio are separate parts of the audio path, and this summary is not an end-to-end loopback latency measurement.

Gain controls range from -60 dB to +12 dB and are smoothed during playback. Each capture channel's sends start off. The engine creates one virtual PipeWire ingress sink for each playback stream and returns streams to ordinary PipeWire routing when stopped. On Linux, the control socket lives in `$XDG_RUNTIME_DIR/shatter/control.sock` and is accessible only to the current user. Windows uses a current-user named pipe.
