#!/bin/sh
set -eu

binary=$1
runtime_dir=$(mktemp -d)
chmod 700 "$runtime_dir"
trap '"$binary" stop >/dev/null 2>&1 || true; rm -rf "$runtime_dir"' EXIT HUP INT TERM
export XDG_RUNTIME_DIR="$runtime_dir"
export XDG_CONFIG_HOME="$runtime_dir/config"

"$binary" start
status=$("$binary" status)
case "$status" in
    *"is running; audio-unavailable:"*"no audio backend is configured"*) ;;
    *) echo "expected clear no-audio status, got: $status" >&2; exit 1 ;;
esac

case "$status" in
    *"Current show: none"*"Default show (next start): default"*"Autosave show (on stop): disabled"*"Startup show error:"*) ;;
    *) echo "expected global settings with no loaded show, got: $status" >&2; exit 1 ;;
esac

health=$($binary health)
case "$health" in
    *"Engine health: failed-open"*"no audio backend is configured"*"I/O latency: unavailable"*) ;;
    *) echo "expected failed-open health details, got: $health" >&2; exit 1 ;;
esac

inputs=$("$binary" --list-in)
case "$inputs" in
    *"no audio backend is configured"*) ;;
    *) echo "expected clear no-audio input listing, got: $inputs" >&2; exit 1 ;;
esac

"$binary" stop
status=$("$binary" status)
case "$status" in
    *"is stopped"*) ;;
    *) echo "expected stopped status, got: $status" >&2; exit 1 ;;
esac
