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

$binary start
$binary stop
stopped=$($binary status)
case "$stopped" in
    *"is stopped"*) ;;
    *) echo "expected stopped status after stop, got: $stopped" >&2; exit 1 ;;
esac
