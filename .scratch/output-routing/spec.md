# Named output channels and sends

Shatter currently mixes every input into one PipeWire playback stream routed to one sink. Replace that single destination with named output channels that can each be routed to one or more physical PipeWire playback sinks or application input captures. Input channels can send to named Shatter outputs; input assignment does not target external sinks directly.

## CLI

- `shatter make out <name> [playback|capture]` creates a playback output (the default) or a capture-type output exposed as an `Audio/Source` virtual microphone.
- `shatter delete out <name-or-pipewire-node-id>` removes the output channel and its node.
- `shatter assign <output|app|capture|stream> <source-selector> <destination-selector>` creates a route. Every physical capture source is available as its own capture input channel in `shatter list in`; select it by device name or listed ID without a separate source-selection step.
- `shatter unassign <output|app|capture|stream> <source-selector> <destination-selector>` removes that route.
- `shatter set default show <name>` selects the named show to load when the engine starts.

Device names use PipeWire's user-facing node description when available, with the stable ID shown by `list in` or `list out` as a fallback for duplicate names. Output selectors include the Shatter name and PipeWire node ID. Names and IDs must resolve unambiguously.

## Routing semantics

- A playback output channel may route to multiple destinations, including speakers, headphones, and application input captures concurrently. A capture-type output exposes its mix as a virtual microphone source, which recording applications select in their own input settings.
- An input channel may send to multiple output channels; each send is independent.
- Each physical capture source has its own automatically created capture channel, fader, mute, meter, and independent sends.
- App channels represent the post-stream-sum signal. A direct playback-stream send inherits its parent app channel's gain and mute, then applies that stream's gain and mute. Capture is an independent input channel.
- Sends are additive: assigning both a playback stream and its parent app channel to the same output intentionally contributes both signals.
- A built-in `speakers` output preserves current startup behavior: app channels are sent to it by default and it follows the system's default playback sink. Playback streams reach speakers through their parent app channel; direct playback-stream sends are off by default. Each capture channel remains unsent by default. User-created outputs are additional destinations.
- Configurations persist output channels, sink bindings, and sends. PipeWire node IDs are runtime selectors; physical sink bindings use stable PipeWire device identity when available, and application input bindings use application ID or process identity with the capture node name when available. Routes remain visibly reconnecting while their exact saved endpoint is absent.
- Output creation, deletion, assignments, and config I/O run on the control thread. Realtime callbacks consume immutable/preallocated routing state and do not allocate, lock, or perform PipeWire graph operations.

## Implementation notes

The current engine has one `speaker_output_` stream and consumes input rings directly in its callback. Multiple output callbacks cannot independently consume those same rings without losing audio on other outputs, so fan-out and output mix scheduling need a shared, realtime-safe ownership model before adding per-output streams.
