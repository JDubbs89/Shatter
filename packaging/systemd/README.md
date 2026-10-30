# systemd user service

The design targets a per-user service started with the desktop session after PipeWire and WirePlumber are available. Add a unit file after the daemon's startup, readiness, restart, and failure-fallback behavior has been prototyped.
