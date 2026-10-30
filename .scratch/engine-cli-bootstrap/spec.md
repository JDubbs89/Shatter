# Engine CLI bootstrap

Prototype the local Shatter command and background engine lifecycle before adding audio processing. The command starts, reports status for, and stops a per-user engine process. It communicates with that process over a private local Unix-domain socket. A repository install script builds and installs the `shatter` command into the user's local bin directory.

The first ticket establishes this lifecycle only. Audio endpoints and processing are covered by later tickets. The initial prototype is manually started through the CLI; automatic session startup is deferred.
