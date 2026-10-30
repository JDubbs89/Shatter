# systemd user service

Automatic startup is deferred. The initial prototype starts and stops the per-user engine through the `shatter` CLI. A systemd user unit can be added after startup, readiness, restart, and failure-fallback behavior has been prototyped.
