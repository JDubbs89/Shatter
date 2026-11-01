# 01: Discover Windows playback sessions and endpoints

**What to build:** On Windows, Shatter starts and reports playback applications, their active sessions, available playback endpoints, and backend health through its existing commands.

**Blocked by:** None (can start immediately).

**Status:** ready-for-agent

- [ ] Windows selects a WASAPI backend and rejects WASAPI on non-Windows toolchains.
- [ ] Backend startup and shutdown manage Windows audio resources safely across repeated lifecycle operations.
- [ ] Playback applications and sessions can be listed with stable app selectors; playback endpoints can be listed with stable endpoint selectors and a clear default indicator.
- [ ] Health reports distinguish a working backend, no available endpoint, and a failed audio-service connection.
- [ ] Unit tests cover session-to-app selector matching and endpoint selection; Windows CI builds and runs them.
- [ ] Operations outside the supported playback scope return clear unsupported-operation errors.
