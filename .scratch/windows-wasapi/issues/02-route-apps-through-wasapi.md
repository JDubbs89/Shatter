# 02: Route application playback through WASAPI

**What to build:** Users can assign an application's playback to a selected Windows endpoint through a supported Windows routing mechanism, with clearly defined behavior for active sessions, future sessions, and endpoint changes.

**Blocked by:** 01: Discover Windows playback sessions and endpoints.

**Status:** ready-for-human

- [ ] The chosen routing mechanism can be implemented using supported Windows APIs and its behavior for already-active sessions is documented.
- [ ] New playback sessions from an assigned app use the selected endpoint while the assignment is active.
- [ ] Endpoint removal and restoration preserve or clearly report the app's routing assignment.
- [ ] Unassigning an app restores the documented default behavior for active and future sessions.
- [ ] Invalid, missing, and ambiguous app or endpoint selectors produce actionable errors without saving a broken rule.
- [ ] A Windows integration test exercises app discovery, assignment, session creation, unassignment, and endpoint churn against virtual or loopback audio where available; unsupported hosts report a test skip clearly.

## Comments

### 2026-10-31: WASAPI routing limitation

Microsoft documents that each audio session is associated with one endpoint and a WASAPI client cannot change an existing stream's session assignment after creation. The current discovery backend does not own the application streams, so it cannot directly move an active app session. Choose and validate a supported routing design before implementation; direct active-session movement is not available through the discovery/session-control interfaces used by ticket 01.
