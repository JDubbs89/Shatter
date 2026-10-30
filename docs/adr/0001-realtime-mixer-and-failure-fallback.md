# Realtime mixer with failure fallback

The product needs real channel-to-bus and matrix mixing in its first useful release, so we will prototype a single custom realtime mixer node in the daemon rather than treating the project as graph organization alone. The daemon must fail open to ordinary PipeWire routing, allowing a brief interruption and loss of the custom mix during recovery; proving that fallback works is an early release gate before expanding the mixer or DSP. If the prototype cannot meet this failure behavior, revisit the engine topology before proceeding.


## Prototype topology clarification

The daemon uses one realtime mixer engine. For application playback, Shatter creates one PipeWire ingress sink per playback stream so the engine can retain stream identity and apply stream-level gain or mute before summing. Streams with the same application identity belong to one persistent app channel; the mixer applies app-channel gain or mute after their streams are summed. These ingress sinks are graph endpoints, not independent mixers.

The engine exposes named output channels as PipeWire playback streams. The built-in `speakers` output follows the system's default playback sink and preserves the startup route. Users can create additional output channels, route each to one physical sink, and assign app channels to those outputs. Input fan-out uses a separate preallocated ring per output so concurrent output callbacks do not consume one another's audio. Output creation, routing, and deletion run on the PipeWire control loop; realtime callbacks only mix the prepared buffers. Multi-sink routing and sends from stream and capture inputs remain follow-up work.

When Shatter exits, its stream ingress nodes are destroyed and WirePlumber restores ordinary routing. This keeps the fail-open behavior in the original decision while allowing per-stream controls.
