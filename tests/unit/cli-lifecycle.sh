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
    *"app-playback"*"shatter.app_playback"*) ;;
    *) echo "expected app playback input in list, got: $inputs" >&2; exit 1 ;;
esac

outputs=$($binary --list-out)
case "$outputs" in
    *"speakers"*"shatter.speakers"*) ;;
    *) echo "expected speakers output in list, got: $outputs" >&2; exit 1 ;;
esac

all=$($binary --list-all)
case "$all" in
    *"app-playback"*"speakers"*) ;;
    *) echo "expected both inputs and outputs in combined list, got: $all" >&2; exit 1 ;;
esac

if $binary --set-gain app-playback -6 >/dev/null 2>&1; then
    echo "expected gain change to report unavailable PipeWire audio" >&2
    exit 1
fi

$binary start
$binary stop
stopped=$($binary status)
case "$stopped" in
    *"is stopped"*) ;;
    *) echo "expected stopped status after stop, got: $stopped" >&2; exit 1 ;;
esac
