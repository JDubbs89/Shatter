# 06: Route output channels to application input captures

**What to build:** Users can select application input capture endpoints, such as Discord's microphone input or a game's microphone capture, as output sinks. Routing a Shatter output channel to one of these endpoints sends that output's mixed audio into the application's input.

**Blocked by:** None (can start immediately).

**Status:** ready-for-human

- [x] Output and sink listings identify application input capture endpoints and distinguish them from physical playback sinks and Shatter's own capture input.
- [x] `shatter assign out <output-selector> <sink-selector>` and `unassign` can connect and disconnect an output channel to an application input capture endpoint using its listed selector.
- [x] The application receives the routed audio as microphone/input audio, while existing playback sink routing continues to work independently.
- [x] Endpoint discovery, disappearance, and reconnection update routes safely; unavailable endpoints are reported clearly and do not silently bind to a different application.
- [x] Configs can persist these routes using stable endpoint identity when available, and restore them when the matching capture endpoint returns.
- [x] Automated coverage verifies endpoint listing, audio delivery to an application input, independent route removal, and endpoint churn.
