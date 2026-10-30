# Named output channels and sends

Shatter currently mixes every input into one PipeWire playback stream routed to one sink. Replace that single destination with named output channels that can each be routed to one or more physical PipeWire playback sinks. Input channels can send to named Shatter outputs; input assignment does not target physical sinks directly.

## CLI

- `shatter make out <name>` creates an output channel and a corresponding PipeWire playback node.
- `shatter delete out <name-or-pipewire-node-id>` removes the output channel and its node.
- `shatter assign out <output-selector> <sink-selector>` routes the output to a physical playback sink.
- `shatter unassign out <output-selector> <sink-selector>` removes that route.
- `shatter assign <app|capture|stream> <input-selector> <output-selector>` creates a send from an input channel to an output channel.
- `shatter unassign <app|capture|stream> <input-selector> <output-selector>` removes that send.

Sink selectors include PipeWire node IDs and Shatter-generated IDs shown by output listing. Output selectors include the Shatter name and PipeWire node ID. Names and IDs must resolve unambiguously.

## Routing semantics

- An output channel may route to multiple playback sinks, including speakers and headphones concurrently.
- An input channel may send to multiple output channels; each send is independent.
- App channels represent the post-stream-sum signal. Playback stream sends tap a stream before app-channel gain. Capture is an independent input channel.
- Sends are additive: assigning both a playback stream and its parent app channel to the same output intentionally contributes both signals.
- A built-in `speakers` output preserves current startup behavior: playback streams and app channels are sent to it by default and it follows the system's default playback sink. Capture remains unsent by default. User-created outputs are additional destinations.
- Configurations persist output channels, sink bindings, and sends. PipeWire node IDs are runtime selectors; persisted sink bindings use stable PipeWire device identity when available.
- Output creation, deletion, assignments, and config I/O run on the control thread. Realtime callbacks consume immutable/preallocated routing state and do not allocate, lock, or perform PipeWire graph operations.

## Implementation notes

The current engine has one `speaker_output_` stream and consumes input rings directly in its callback. Multiple output callbacks cannot independently consume those same rings without losing audio on other outputs, so fan-out and output mix scheduling need a shared, realtime-safe ownership model before adding per-output streams.
