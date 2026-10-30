# PipeWire Mixer Backend: Design Spec

Oct 30, 2026 · @Jonathan

A C++ daemon (working name `pwmixd`) turns PipeWire into a console-style mixer with persistent, rule-based routing, and exposes everything over a local API so any GUI can sit on top.

## Goals and constraints

**Goals**

- Fix the actual pain: routing and levels survive reboot, app restarts, and Discord recreating its streams.
- Model a small digital console: input strips, mix buses with sends, matrices, a main bus, groups, scenes.
- Run the DSP and metering in our own RT code, not a pile of loopback modules.
- Keep the GUI replaceable: the API is the product.

**Non-goals (v1)**

- Any GUI beyond a debug CLI.
- Windows/macOS, JACK-only setups, network audio (Dante/AES67).
- VST3 hosting, voice changers, AI assistants.

**Constraints that shape the design**

| Constraint | Consequence |
| --- | --- |
| WirePlumber's policy moves streams and restores its own state | We steer it through supported hooks (`target.object` metadata, rules) and reconcile desired vs actual state; we never race it with raw links alone |
| Discord (Electron) recreates streams and renames them | Match on stable props (binary, flatpak ID, `media.class`), debounce re-appearance, handle playback and capture separately |
| Audio must survive a daemon crash | The engine fails open: if it dies, apps fall back to the default sink instead of going silent |
| RT thread cannot block, allocate or lock | All parameter changes go through lock-free queues; DSP state is preallocated |
| Target: Fedora, PipeWire 1.x, WirePlumber 0.5 | Use `libpipewire-0.3` C API; verify metadata key names against installed versions in an early spike |

## Feature list and triage

P0 is the minimum that fixes your audio problems and proves the engine. P1 makes it a real console. P2 is polish. Cut means do not build in this project.

**Routing and strips**

| Feature | Modeled on | Tier | Rationale |
| --- | --- | --- | --- |
| Live graph model: devices, app streams, ports, links | Patchd | P0 | Foundation for everything else |
| Rule-based app-to-strip assignment (match on process binary, app ID, media class) | Patchd per-app fader | P0 | This is the Discord fix |
| Virtual input strips (named, stereo, appear as sinks apps can target) | Patchd cables | P0 | Replaces hand-built null sinks |
| Hardware input strips (mic, line) | Midas input channels | P0 | Needed for a mic through the mixer |
| Mix buses with per-strip sends | Midas mix buses | P0 | The core mixing abstraction |
| Main bus and hardware output assignment | Midas main LR | P0 | Final destination |
| Per-strip gain, mute, pan/balance, name, color | Both | P0 | Basic strip controls |
| Pre-fader / post-fader send tap | Midas | P1 | Needed for monitor mixes |
| Matrices (sum of buses to outputs) | Midas matrix | P1 | Speakers, stream, record feeds from buses |
| Solo / PFL / monitor bus | Midas | P1 | Needed to audition a strip |
| Mute groups and DCA/VCA groups | Midas DCAs | P1 | One fader moves many strips |
| Stereo-linked strips and bus-to-bus routing | Midas | P2 | Convenience |
| Insert points (route a strip through an external node) | Midas | P2 | Rare; LV2 slots cover most uses |
| Talkback and test oscillator | Midas | P2 | Cheap generator, low value for desktop use |

**Dynamics, EQ and FX**

| Feature | Modeled on | Tier | Rationale |
| --- | --- | --- | --- |
| High-pass filter and gain trim | Midas preamp | P1 | Trivial biquad, high value on mics |
| 4-band parametric EQ per strip and bus | Midas | P1 | Core channel strip |
| Compressor, gate, limiter | Both | P1 | Core channel strip |
| Sidechain ducking | Patchd Studio | P1 | Auto-duck game when voice chat is active |
| LV2 plugin slots | Patchd VST3 hosting | P1 | VST3 has no good Linux host story |
| Output delay (alignment) | Midas | P1 | Cheap delay line per output |
| De-esser, saturator | Patchd | P2 | Nice on voice |
| Graphic EQ on buses | Midas | P2 | Room correction |
| RNNoise noise suppression | Patchd | P2 | Small BSD library |
| Automix / gain sharing | Midas | P2 | Multi-mic podcasts only |

**Metering and monitoring**

| Feature | Modeled on | Tier | Rationale |
| --- | --- | --- | --- |
| Peak and RMS meters on every strip, bus, matrix, main | Both | P0 | Drives the GUI |
| Gain-reduction meters | Midas | P1 | Dynamics feedback |
| Latency and xrun reporting | Patchd | P1 | Builds trust in the engine |
| LUFS (momentary, short-term, integrated) | Pro tools | P2 | Streamers want it |
| RTA / spectrum | Midas | P2 | FFT stream to the GUI |
| Per-strip recording to WAV | Midas USB recorder | P2 | Separate podcast tracks |

**State, scenes and control**

| Feature | Modeled on | Tier | Rationale |
| --- | --- | --- | --- |
| Full-state persistence and auto-restore on login | Both | P0 | The second half of your pain |
| Scenes (save, recall, diff) | Midas scenes | P0 | Cheap once state is one serializable model |
| Recall safes and partial recall (snippets) | Midas | P2 | Only matters once scenes are heavily used |
| Local API with event stream | (new) | P0 | The product surface |
| OSC server (X32 subset) | Midas / X32 ecosystem | P1 | Existing mobile apps and control surfaces work for free |
| MIDI control surface mapping, global hotkeys | Patchd hotkeys | P1 | Motorized faders, Stream Deck |
| Remote network access with auth | Midas | P2 | Phone as a remote; needs a real security pass |

**Cut**

| Feature | Reason |
| --- | --- |
| VST3 hosting | No stable Linux host story; LV2 now, CLAP later if needed |
| Voice changer (Persona) and 111 voices | A separate product; do not drag it in |
| AI assistant (Patchy) | Out of scope for a backend |
| Skins and themes | GUI concern |
| Network audio (AES50, Dante, AES67) | PipeWire has RTP modules; not our layer |
| Timecode-driven automation | No audience on a desktop |

Two holes worth flagging. First, per-strip sends and matrices are cheap if the mixing happens in one engine process, and awkward if you try to build them from loopback modules, which is why the engine in the next section is a single node. Second, the P0 list is already large; if schedule slips, drop scenes to P1 before touching rules or persistence.

## Architecture

All mixing happens in one realtime node inside the daemon; the control plane never touches audio, it only sends parameter deltas down a lock-free queue and reads meters back. This was the main design choice, so here are the alternatives:

| Engine strategy | Pros | Cons |
| --- | --- | --- |
| One `pw_filter` engine node (pick) | Sends and matrices are trivial; sample-aligned mixing; meters computed in the callback | All managed audio passes through our process; a crash silences managed strips unless the daemon fails open |
| Graph of native nodes (null sinks, loopbacks, filter-chain) | No custom RT code; PipeWire does the work | Links carry no gain, so sends need extra mixer nodes; meters need extra streams; hard to persist and reason about |
| SPA plugin loaded inside the PipeWire process | Lowest overhead | A bug takes down PipeWire itself; hard to develop, debug and ship |

&#91;embedded content: architecture · data plane and control plane, 11 components\]

Apps play into virtual strip sinks, the engine mixes them into buses, matrices and the main bus, and the result goes to hardware sinks. Three threads matter: the PipeWire RT thread (engine), the control thread (PipeWire events, state, rules), and API threads (sockets). Only the first must never block.

## Libraries and dependencies

The default pick in each row keeps the dependency count low and everything packageable on Fedora. Versions of PipeWire and WirePlumber APIs should be checked in the first spike.

| Concern | Options | Pick | Trade-off |
| --- | --- | --- | --- |
| PipeWire access | `libpipewire-0.3` C API; `pipewire-rs` (Rust) | `libpipewire-0.3` | Stable, documented, what WirePlumber uses; verbose and manual lifetime handling, so wrap it in RAII |
| Realtime node | `pw_filter` with `PW_FILTER_FLAG_RT_PROCESS`; `pw_stream` per port; SPA plugin loaded in the PipeWire process | `pw_filter` | One node with many ports gives sample-aligned mixing; an in-process SPA plugin is faster but a crash takes PipeWire down |
| DSP primitives | Hand-rolled biquads, dynamics, delay; Faust-generated C++; JUCE DSP | Hand-rolled, optional Faust later | Hand-rolled is small and fully controlled; Faust speeds up compressors and limiters but adds a build step; JUCE is GUI-oriented and its licensing complicates an open-source release |
| Plugin hosting | `lilv` (LV2); CLAP host headers; VST3 SDK | `lilv` at P1 | LV2 is native on Linux and the ecosystem (Calf, x42) is usable; CLAP is a good P2; VST3 is a poor fit |
| Loudness | `libebur128` | `libebur128` | MIT, well tested, runs on a non-RT worker |
| FFT for RTA | `pffft`; `kissfft`; FFTW | `pffft` | Small and fast; FFTW is overkill |
| Noise suppression | RNNoise; DeepFilterNet | RNNoise (P2) | BSD, 480-sample frames, CPU-light; quality lower than DeepFilterNet |
| API transport | WebSocket + JSON-RPC; gRPC; D-Bus; plain Unix socket + protobuf | WebSocket + JSON-RPC, with binary frames for meters | Works from Tauri, Qt, Electron, Python, and the browser with no codegen; no compile-time types, so publish a JSON Schema. gRPC gives types and streaming but needs grpc-web for browsers and is heavy. D-Bus fits desktop integration but is poor for 60 Hz meter streams |
| Network stack | Boost.Beast / Asio; Drogon; uWebSockets; libwebsockets | Standalone Asio + Beast | `boost-devel` is on Fedora; compile times are the cost; Drogon is faster to start but pulls a framework |
| JSON | `nlohmann/json`; Boost.JSON; simdjson + glaze | `nlohmann/json` | Ergonomic; slow only on huge payloads, which this API never sends |
| Config and rules | `toml++`; YAML; JSON | `toml++` for rules and config, JSON for scenes and runtime state | TOML is human-editable for rules; scenes are machine-written |
| Lock-free queues | `readerwriterqueue`; `boost::lockfree::spsc_queue`; hand-rolled ring | `readerwriterqueue` | Header-only, battle-tested; the meter ring is hand-rolled |
| Matching | `std::regex`; RE2; PCRE2 | Glob plus `std::regex` | Control thread only, so speed is irrelevant; avoid ReDoS by compiling rules once and limiting length |
| OSC | `liblo`; `tinyosc` | `liblo` (P1) | Packaged on Fedora; supports UDP and bundles |
| MIDI | PipeWire MIDI ports via `pw_filter`; `RtMidi`; ALSA seq | PipeWire MIDI ports | Stays in one API; some controllers only expose ALSA seq, which PipeWire bridges |
| Logging and tests | `spdlog`; Catch2 or doctest | `spdlog`, `Catch2` | Standard choices |
| Build | CMake + Ninja, C++20, `FetchContent` for header-only deps | as listed | GCC on Fedora supports the needed C++20 subset |
| Process management | systemd user unit with `sd_notify` | systemd | Starts after PipeWire and WirePlumber; restarts on crash |

## C++ modules

Twelve modules in four layers. Only `engine` runs on the realtime thread; everything else runs on the control thread or API threads.

| Module | Layer | Thread | Responsibility | Tier |
| --- | --- | --- | --- | --- |
| `pw::Core` | PipeWire | control | Connection, thread loop, registry listener, cached object model (nodes, ports, links, devices, clients, metadata), change signals | P0 |
| `pw::LinkManager` | PipeWire | control | Create and destroy links via the link factory; reconcile desired links against actual; retry on node churn | P0 |
| `pw::VirtualDevices` | PipeWire | control | Create virtual sinks and sources for strips with stable names; set `object.linger` or recreate on startup | P0 |
| `pw::RuleEngine` | PipeWire | control | Match new streams against rules, set `target.object` through metadata, debounce re-appearance, expose a dry-run `test` | P0 |
| `engine::MixerNode` | Engine | RT | The `pw_filter` node: ports, per-block callback, applies queued parameter changes, calls strips, buses, matrices | P0 |
| `engine::Dsp` | Engine | RT | Gain, pan law, smoothing, biquad EQ, HPF, gate, compressor, limiter, delay line, sidechain tap; preallocated, no heap | P0 gain/pan, P1 rest |
| `engine::FxHost` | Engine | RT + worker | LV2 slot hosting through `lilv`, atom and worker support, bypass and crossfade | P1 |
| `meter::MeterBus` | Engine | RT to API | Per-block peak and RMS written to a hand-rolled ring; a publisher thread decimates to 30 to 60 Hz; LUFS and FFT on a worker | P0 peak/RMS, P2 rest |
| `state::Store` | State | control | Single source of truth for strips, buses, sends, groups, rules, scenes; command objects with undo; versioned JSON; atomic autosave (temp file + rename) | P0 |
| `state::Scenes` | State | control | Snapshot, recall, diff, recall safes | P0 basic, P2 safes |
| `api::Server` | API | API threads | WebSocket and Unix-socket transport, JSON-RPC dispatch, subscriptions, schema validation, binary meter frames | P0 |
| `app::Supervisor` | App | main | Config, logging, signal handling, systemd notify, watchdog, fail-open fallback if the engine dies | P0 |

Two more optional modules sit beside `api::Server`: `api::Osc` (X32-subset UDP server, P1) and `control::Midi` (MIDI and hotkey mapping, P1).

**Key interfaces**

- `state::Store` exposes `apply(Command) -> Result` and `subscribe(Listener)`. Every API call, scene recall, OSC message and rule action goes through `apply`; nothing mutates state elsewhere.
- On each applied command the store publishes a `ParamDelta` to the engine through an SPSC queue, and a `ChangeEvent` to API subscribers. The engine never reads the store directly.
- Structural changes (adding a strip, changing channel count, inserting an LV2 slot) build a new immutable `EngineConfig` on the control thread and hand it to the RT thread by atomic pointer swap; the old one is freed on a worker thread after the RT thread acknowledges.
- `pw::RuleEngine` reads strips from the store and writes only through `pw::LinkManager` and metadata; it never touches engine state.

**Hole to plug early:** the swap path for structural changes is where RT bugs live. Write the `EngineConfig` handoff and its stress test (random reconfigure while a sine plays, check for glitches and xruns) before building DSP on top.

## API surface

Transport is JSON-RPC 2.0 over WebSocket, on a Unix socket by default and optional loopback TCP. Every state change is also pushed to all subscribers, so two GUIs, a phone and a hotkey daemon stay in sync. Meters travel as binary frames on the same socket.

**Resources**

| Resource | Methods | Notes |
| --- | --- | --- |
| `system` | `info`, `health`, `latency`, `shutdown` | Version, PipeWire and WirePlumber versions, xrun count, engine state |
| `graph` | `get`, `get_node`, `list_devices` | Read-only view of the raw PipeWire graph for debugging and device pickers |
| `streams` | `list`, `get`, `assign` | Live app streams with their properties and the strip each is assigned to |
| `strips` | `list`, `create`, `delete`, `set`, `get` | `set` takes any of gain, mute, pan, solo, name, color, icon, trim, polarity, source, delay |
| `strips.sends` | `set`, `get` | Level, pre/post tap, on/off per strip to bus pair |
| `buses` | `list`, `create`, `delete`, `set` | Mix buses; same parameter set as strips plus output assignment |
| `matrices` | `list`, `create`, `delete`, `set` | Bus-to-matrix sends and matrix outputs |
| `main` | `set`, `get` | Main bus level, mute, output assignment |
| `outputs` | `list`, `assign`, `set_delay` | Hardware output devices, bound to buses, matrices or main |
| `dsp` | `eq.set`, `dyn.set`, `hpf.set`, `bypass` | Per strip and per bus; parameter sets are typed and validated |
| `fx` | `list_plugins`, `load`, `unload`, `set_param`, `bypass` | LV2 slots (P1) |
| `groups` | `list`, `create`, `set`, `assign` | Mute groups and DCA groups (P1) |
| `rules` | `list`, `create`, `update`, `delete`, `test` | `test` dry-runs a rule against current streams and returns matches |
| `scenes` | `list`, `save`, `recall`, `delete`, `diff`, `set_safes` | `recall` accepts a scope: all, a list of strips, a list of parameter classes |
| `state` | `export`, `import`, `undo`, `redo` | Whole-state JSON; undo applies to commands from any client |
| `meters` | `subscribe`, `unsubscribe` | Targets, rate (10 to 60 Hz), what to include (peak, RMS, gain reduction) |
| `events` | `subscribe` | Topics below |

**Events pushed to subscribers**

- `stream.appeared`, `stream.disappeared`, `stream.matched` (which rule matched, which strip it joined)
- `device.added`, `device.removed`, `device.default_changed`
- `param.changed` (path, value, origin client), the single event that keeps every GUI in sync
- `scene.recalled`, `rules.changed`, `state.loaded`
- `engine.xrun`, `engine.state` (running, degraded, failed-open)

**Meter frame (binary, little-endian)**

| Field | Size | Meaning |
| --- | --- | --- |
| magic + version | 4 bytes | Reject frames from a mismatched server |
| sequence | 4 bytes | Lets the client detect dropped frames |
| count | 2 bytes | Number of targets in this frame |
| per target: id | 4 bytes | Strip, bus, matrix, or main ID |
| per target: peak L, R and RMS L, R | 4 x float16, dBFS | Clients apply their own ballistics and peak-hold |
| per target: gain reduction | float16, dB | Zero when no dynamics are active |

At 60 Hz with 40 targets that is about 34 KB/s (14 bytes per target), so no compression is needed.

**Design choices to settle before coding**

| Decision | Option A | Option B | Lean |
| --- | --- | --- | --- |
| Parameter addressing | Typed methods per resource (`strips.set`) | Generic path set (`/strip/3/gain`), X32-style | Typed methods for the API, paths internally so OSC maps trivially |
| Consistency model | Optimistic: server echoes `param.changed` to everyone | Request/response only | Optimistic echo; clients never assume their write landed |
| Auth on the Unix socket | File permissions only | Token handshake | File permissions for v1; token required before any TCP listener is allowed |
| Schema | Hand-written JSON Schema | Generated from C++ structs | Hand-written; it doubles as GUI documentation and a contract test |

Hole: exposing loopback TCP means any local process or browser page can drive your audio, including through DNS rebinding. Keep it off by default, check the `Origin` header, and require a token.

## Milestones, risks and open questions

Build order is chosen so the riskiest unknowns (WirePlumber coexistence and the RT engine) are tested first.

1. **Spike (1 to 2 weeks).** CLI that lists the graph, creates a virtual sink, steers a Discord stream with `target.object` metadata, and survives a reboot. Exit gate: Discord lands on the right strip after restart, three times in a row.
2. **Engine skeleton.** `pw_filter` node with N stereo inputs and M outputs, gain and mute only, `EngineConfig` swap with the stress test. Exit gate: no xruns under random reconfigure at 128 frames.
3. **State and API core.** `state::Store`, JSON-RPC server, events, autosave. Exit gate: two scripted clients see each other's changes.
4. **Rules and persistence (P0 complete).** Rule engine, strips, buses, sends, meters, basic scenes. Exit gate: you use it daily in place of Helvum.
5. **Console layer (P1).** EQ, dynamics, solo, groups, matrices, sidechain ducking, output delay, OSC.
6. **Plugins and polish (P1/P2).** LV2 hosting, MIDI mapping, LUFS, RTA, recording.

**Risks**

| Risk | Likelihood | Impact | Mitigation |
| --- | --- | --- | --- |
| WirePlumber re-links streams against our rules | High | High | Spike first; use metadata and rules it supports; reconcile on every `link.added` |
| Metadata keys or config format change between WirePlumber releases | Medium | Medium | Pin tested versions; isolate in `pw::RuleEngine`; integration test in CI |
| RT glitches from reconfiguration | Medium | High | Atomic config swap, stress test in step 2, no allocation in callback (enforced with a test allocator) |
| Added latency from extra graph hops | Medium | Medium | Single engine node; report latency in the API; document the budget |
| Clock drift between USB devices and the engine | Medium | Medium | Rely on PipeWire's adapter resampling; test with two unrelated devices |
| Scope creep into a GUI or plugin host | High | High | GUI stays out of scope; LV2 is P1 and gated on step 4 |
| Discord behavior changes (Electron or flatpak updates) | Medium | Low | Rules are user-editable data, not code |

**Open questions**

- Should strips be created implicitly for unmatched apps (a catch-all strip), or should unmatched apps stay on the default sink?
- Do you want hardware inputs mixed in the engine, or left as direct PipeWire links for lowest latency?
- Open source from the start? A license choice affects LV2 and RNNoise linking (both are compatible with GPL and MIT).
- Is X32 OSC compatibility worth the semantic contortions, or should the OSC namespace be our own?
