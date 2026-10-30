# Shatter configurations

Shatter stores user configurations in `$XDG_CONFIG_HOME/shatter/` (or
`~/.config/shatter/` when `XDG_CONFIG_HOME` is unset). Config names become
`<name>.conf`; the extension is optional when saving or loading.

```sh
shatter save show work
shatter load show work
```

The engine loads `default.conf` at startup and saves the current state there
when stopped. Configuration I/O and changes run on the engine control thread;
the real-time audio callback only reads the resulting atomic gain and mute
values.

Version 1 stores app playback channel identity, display name, gain, and mute
state, plus playback stream settings keyed by app identity and the stream's
original PipeWire name. This lets saved stream settings apply when the stream
returns with a different PipeWire node ID. The text format is versioned and
uses quoted fields so names can contain spaces.
