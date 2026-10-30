#!/bin/sh
set -eu

binary=$1
runtime_dir=$(mktemp -d)
chmod 700 "$runtime_dir"
trap '"$binary" stop >/dev/null 2>&1 || true; rm -rf "$runtime_dir"' EXIT HUP INT TERM
export XDG_RUNTIME_DIR="$runtime_dir"

stopped=$($binary status)
case "$stopped" in
    *"is stopped"*) ;;
    *) echo "expected stopped status, got: $stopped" >&2; exit 1 ;;
esac

$binary start
running=$($binary status)
case "$running" in
    *"is running"*) ;;
    *) echo "expected running status, got: $running" >&2; exit 1 ;;
esac

inputs=$($binary --list-in)
case "$inputs" in
    *"Shatter inputs"*"No application playback streams have been discovered"*) ;;
    *) echo "expected an empty app channel list, got: $inputs" >&2; exit 1 ;;
esac

outputs=$($binary --list-out)
case "$outputs" in
    *"speakers"*"shatter.speakers"*) ;;
    *) echo "expected speakers output in list, got: $outputs" >&2; exit 1 ;;
esac

all=$($binary --list-all)
case "$all" in
    *"No application playback streams have been discovered"*"speakers"*) ;;
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

$binary start
$binary stop
stopped=$($binary status)
case "$stopped" in
    *"is stopped"*) ;;
    *) echo "expected stopped status after stop, got: $stopped" >&2; exit 1 ;;
esac
