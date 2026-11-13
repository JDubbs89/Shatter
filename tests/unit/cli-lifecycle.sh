#!/bin/sh
set -eu

binary=$1
runtime_dir=$(mktemp -d)
chmod 700 "$runtime_dir"
pipewire_pid=
cleanup() {
    "$binary" stop >/dev/null 2>&1 || true
    if [ -n "$pipewire_pid" ]; then kill "$pipewire_pid" 2>/dev/null || true; wait "$pipewire_pid" 2>/dev/null || true; fi
    rm -rf "$runtime_dir"
}
trap cleanup EXIT HUP INT TERM
export XDG_RUNTIME_DIR="$runtime_dir"
export PIPEWIRE_RUNTIME_DIR="$runtime_dir"
export PIPEWIRE_REMOTE=pipewire-0
export PULSE_SERVER="unix:$runtime_dir/pulse/native"
export XDG_CONFIG_HOME="$runtime_dir/config"

pipewire >"$runtime_dir/pipewire.log" 2>&1 &
pipewire_pid=$!
ready=0
for attempt in $(seq 1 50); do
    if pw-cli info 0 >/dev/null 2>&1; then ready=1; break; fi
    sleep 0.1
done
if [ "$ready" -ne 1 ]; then cat "$runtime_dir/pipewire.log" >&2; exit 1; fi

stopped=$($binary status)
case "$stopped" in
    *"is stopped"*) ;;
    *) echo "expected stopped status, got: $stopped" >&2; exit 1 ;;
esac

$binary start
running=$($binary status)
case "$running" in
    *"is running"*"Current show: default"*"Default show (next start): default"*"Autosave show (on stop): default"*) ;;
    *) echo "expected running status, got: $running" >&2; exit 1 ;;
esac

health=$($binary health)
case "$health" in
    *"Engine health: "*"Xruns since engine start"*"I/O buffering estimate"*) ;;
    *) echo "expected engine health and latency details, got: $health" >&2; exit 1 ;;
esac

if $binary --engine >/dev/null 2>&1; then
    echo "expected duplicate engine process to fail" >&2
    exit 1
fi
running=$($binary status)
case "$running" in
    *"is running"*) ;;
    *) echo "duplicate start disrupted the active engine: $running" >&2; exit 1 ;;
esac

inputs=$($binary --list-in)
case "$inputs" in
    *"Shatter inputs"*"No app streams discovered"*) ;;
    *) echo "expected an empty app channel list, got: $inputs" >&2; exit 1 ;;
esac

outputs=$($binary --list-out)
case "$outputs" in
    *"speakers"*"shatter.speakers"*) ;;
    *) echo "expected speakers output in list, got: $outputs" >&2; exit 1 ;;
esac

all=$($binary --list-all)
case "$all" in
    *"No app streams discovered"*"speakers"*) ;;
    *) echo "expected both inputs and outputs in combined list, got: $all" >&2; exit 1 ;;
esac

if $binary --set-gain app app-deadbeef 0 >/dev/null 2>&1; then
    echo "expected gain change for an unknown app channel to fail" >&2
    exit 1
fi

if $binary --set-gain stream 42 -6 >/dev/null 2>&1; then
    echo "expected gain change for an unknown playback stream to fail" >&2
    exit 1
fi

if $binary --set-mute app app-deadbeef on >/dev/null 2>&1; then
    echo "expected mute change for an unknown app channel to fail" >&2
    exit 1
fi

$binary save show studio
running=$($binary status)
case "$running" in
    *"Current show: default"*) ;;
    *) echo "saving a copy changed the current show: $running" >&2; exit 1 ;;
esac
if $binary load show missing >/dev/null 2>&1; then
    echo "expected loading a missing show to fail" >&2
    exit 1
fi
$binary set default show studio
running=$($binary status engine)
case "$running" in
    *"Current show: default"*"Default show (next start): studio"*"Autosave show (on stop): default"*) ;;
    *) echo "expected current and default shows with the original autosave target, got: $running" >&2; exit 1 ;;
esac
if [ "$(cat "$XDG_CONFIG_HOME/shatter/default-show")" != "studio" ]; then
    echo "default show selection was not persisted" >&2
    exit 1
fi
if $binary set default show missing >/dev/null 2>&1; then
    echo "expected selecting a missing show to fail" >&2
    exit 1
fi

$binary stop
stopped=$($binary status)
case "$stopped" in
    *"is stopped"*) ;;
    *) echo "expected stopped status after stop, got: $stopped" >&2; exit 1 ;;
esac

$binary start
$binary stop
if [ ! -f "$XDG_CONFIG_HOME/shatter/studio.conf" ]; then
    echo "engine did not save the selected default show at stop" >&2
    exit 1
fi
