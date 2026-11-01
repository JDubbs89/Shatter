# Windows audio support through WASAPI

## Goal

Build Shatter on Windows with a native audio backend that discovers playback endpoints and application sessions, routes selected applications to endpoints, and keeps those assignments effective as sessions or devices change. The Windows release artifact must be produced only after the Windows test suite passes.

## Scope

- Use Windows audio APIs behind the existing backend-neutral audio interface.
- Support playback endpoint and application-session discovery, app-to-endpoint routing, backend lifecycle, and useful health reporting.
- Keep unsupported audio operations explicit in command responses.
- Run Windows-specific lifecycle, backend, and integration coverage in the release matrix.

## Not in scope

- Feature parity with the PipeWire mixer, including capture routing, custom mixing, sends, and effects.
- Persisting routing rules across Shatter restarts unless a separate issue requires it.

## Completion

The Windows configuration selects the WASAPI backend, builds and installs the executable, passes all registered Windows tests, and packages a Windows release artifact only after the platform matrix succeeds.
