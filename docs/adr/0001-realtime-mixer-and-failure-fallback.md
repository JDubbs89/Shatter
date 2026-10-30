# Realtime mixer with failure fallback

The product needs real channel-to-bus and matrix mixing in its first useful release, so we will prototype a single custom realtime mixer node in the daemon rather than treating the project as graph organization alone. The daemon must fail open to ordinary PipeWire routing, allowing a brief interruption and loss of the custom mix during recovery; proving that fallback works is an early release gate before expanding the mixer or DSP. If the prototype cannot meet this failure behavior, revisit the engine topology before proceeding.
