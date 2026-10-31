# PipeWire Mixer

A single-user Linux desktop application for persistently organizing and mixing audio from applications and physical devices. The project models audio as mixer channels, mixes, and destinations so users can route playback and capture independently.

## Audio sources and destinations

**Input channel**:
A mixer source that contributes audio to one or more mixes. It can represent application playback, a physical input such as a microphone, or a virtual source.
_Avoid_: Strip (unless quoting a specific external mixer’s terminology)

**Playback channel**:
An input channel for one application, receiving the application's matching playback streams. Each playback stream can be adjusted before the streams are combined; the combined channel has its own gain, mute, metering, and sends.
_Avoid_: App output (ambiguous with PipeWire graph output)

**Playback stream**:
An individual audio stream an application publishes to PipeWire; one application may have several playback streams.
_Avoid_: Playback channel (a stream is one feed into the application channel)

**Capture channel**:
An automatically created input channel receiving audio from one physical capture device, such as a microphone. Each available capture device has its own gain, mute, meter, output sends, and stable selector.
_Avoid_: Microphone destination

**Output channel**:
A named mixer destination that collects sends from input channels. Its type determines whether it plays to playback sinks or exposes its mix as a virtual capture source for applications.
_Avoid_: Output sink (that is a PipeWire endpoint), output bus

**Output channel type**:
The role of an output channel: playback, which delivers audio to playback sinks, or capture, which exposes audio as a virtual microphone source applications can select.
_Avoid_: Classification (without saying what is classified)

**Mix bus**:
A named mix that collects sends from input channels and can feed one or more matrices.
_Avoid_: Output bus (ambiguous with physical or virtual destination)

**Matrix**:
A destination-oriented mix that combines and adjusts feeds from mix buses before they reach a destination.
_Avoid_: Bus (when distinguishing its role from a mix bus)

**Destination**:
A physical playback device or virtual app-facing playback/capture endpoint that receives a configured mix.
_Avoid_: Output (ambiguous with a PipeWire graph output)

**Capture destination**:
A virtual app-facing audio source assigned to an application’s capture stream; its configured mix determines what the application receives.
_Avoid_: Microphone (a microphone is a physical-source input channel)

## Routing and identity

**Playback rule**:
A direction-specific rule that assigns matching application playback streams to a persistent playback channel.
_Avoid_: Capture rule

**Capture rule**:
A direction-specific rule that assigns a matching application capture stream to a virtual capture destination.
_Avoid_: Playback rule

**Device binding**:
A destination’s choice to follow the system default device or remain pinned to a specific physical device.
_Avoid_: Device name (names may change and are not necessarily stable identity)

**Device selector**:
A command-facing reference to a device, resolved from its user-facing name or stable listed ID; duplicate names require the ID.
_Avoid_: Device ID as the only way to address a device

**Send**:
A channel’s routed contribution to an output channel or mix bus, with its own level; direct sends are post-fader.
_Avoid_: Link (a PipeWire graph connection is not necessarily a mixer send)
