#!/bin/sh
set -eu

binary=$1
test_source=$2
test_root=$(mktemp -d /tmp/shatter-capture-integration.XXXXXX)
runtime_dir="$test_root/runtime"
config_dir="$test_root/config"
home_dir="$test_root/home"
pipewire_pid=
wireplumber_pid=
source_pid=
engine_running=0
record_pid=

cleanup()
{
    if [ "$engine_running" -eq 1 ]; then
        XDG_RUNTIME_DIR="$runtime_dir" "$binary" stop >/dev/null 2>&1 || true
    fi
    if [ -n "$record_pid" ]; then kill "$record_pid" 2>/dev/null || true; fi
    if [ -n "$source_pid" ]; then kill "$source_pid" 2>/dev/null || true; fi
    if [ -n "$wireplumber_pid" ]; then kill "$wireplumber_pid" 2>/dev/null || true; fi
    if [ -n "$pipewire_pid" ]; then kill "$pipewire_pid" 2>/dev/null || true; fi
    if [ -n "$record_pid" ]; then wait "$record_pid" 2>/dev/null || true; fi
    if [ -n "$source_pid" ]; then wait "$source_pid" 2>/dev/null || true; fi
    if [ -n "$wireplumber_pid" ]; then wait "$wireplumber_pid" 2>/dev/null || true; fi
    if [ -n "$pipewire_pid" ]; then wait "$pipewire_pid" 2>/dev/null || true; fi
    python3 - "$test_root" <<'PY'
import shutil
import sys
shutil.rmtree(sys.argv[1], ignore_errors=True)
PY
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$runtime_dir" "$config_dir" "$home_dir/.config" "$home_dir/.local/state" "$home_dir/.cache"
chmod 700 "$runtime_dir"
cp /usr/share/pipewire/pipewire.conf "$config_dir/pipewire.conf"
cp /usr/share/pipewire/client.conf "$config_dir/client.conf"
export HOME="$home_dir" XDG_RUNTIME_DIR="$runtime_dir" XDG_CONFIG_HOME="$home_dir/.config"
export XDG_STATE_HOME="$home_dir/.local/state" XDG_CACHE_HOME="$home_dir/.cache" PIPEWIRE_CONFIG_DIR="$config_dir"

pipewire --config=pipewire.conf >"$test_root/pipewire.log" 2>&1 &
pipewire_pid=$!
ready=0
for attempt in $(seq 1 50); do
    if pw-cli info 0 >/dev/null 2>&1; then ready=1; break; fi
    sleep 0.1
done
if [ "$ready" -ne 1 ]; then
    cat "$test_root/pipewire.log" >&2
    echo "isolated PipeWire did not start" >&2
    exit 1
fi

wireplumber --profile policy >"$test_root/wireplumber.log" 2>&1 &
wireplumber_pid=$!
sleep 0.5
pw-cli create-node adapter '{ factory.name = support.null-audio-sink node.name = shatter-test-speakers node.description = "Shatter Test Speakers" media.class = Audio/Sink audio.position = [ FL FR ] object.linger = true }' >/dev/null
sleep 0.3
sink_id=$(wpctl status -n | awk '/shatter-test-speakers/ { for (i=1; i<=NF; ++i) if ($i ~ /^[0-9]+\.$/) { id=$i; sub(/\.$/, "", id); print id; exit } }')
if [ -z "$sink_id" ]; then
    cat "$test_root/wireplumber.log" >&2
    echo "isolated test speaker sink was not created" >&2
    exit 1
fi
wpctl set-default "$sink_id" >/dev/null

"$test_source" >"$test_root/source.log" 2>&1 &
source_pid=$!
source_id=
for attempt in $(seq 1 50); do
    source_id=$(pw-cli ls Node | awk '/id [0-9]+,/ { id=$2; sub(/,/, "", id) } /node.name = "shatter-test-mic-source"/ { print id; exit }')
    if [ -n "$source_id" ]; then break; fi
    sleep 0.1
done
if [ -z "$source_id" ]; then
    cat "$test_root/source.log" >&2
    echo "synthetic capture source did not appear in PipeWire" >&2
    exit 1
fi

"$binary" start
engine_running=1
inputs=$($binary --list-in)
case "$inputs" in
    *"Capture source"*"shatter-test-mic-source ($source_id)"*) ;;
    *) echo "selected source is missing from input list: $inputs" >&2; exit 1 ;;
esac

"$binary" --set-capture-source "$source_id"
sleep 0.3
inputs=$($binary --list-in)
case "$inputs" in
    *"speaker send off"*) ;;
    *) echo "capture speaker send should default off: $inputs" >&2; exit 1 ;;
esac
if ! "$binary" --set-capture-send off >/dev/null; then
    echo "could not keep capture speaker send disabled" >&2
    exit 1
fi

record_monitor()
{
    destination=$1
    pw-record --target 0 --rate 48000 --channels 2 --format f32 --raw --sample-count 48000 "$destination" \
        >"$test_root/record.log" 2>&1 &
    record_pid=$!
    ready=0
    for attempt in $(seq 1 40); do
        if pw-link -i | grep -q 'pw-record:input_FL'; then ready=1; break; fi
        sleep 0.05
    done
    if [ "$ready" -ne 1 ]; then
        kill "$record_pid" 2>/dev/null || true
        echo "PipeWire recorder ports did not appear" >&2
        return 1
    fi
    pw-link shatter-test-speakers:monitor_FL pw-record:input_FL >/dev/null
    pw-link shatter-test-speakers:monitor_FR pw-record:input_FR >/dev/null
    wait "$record_pid" || [ -s "$destination" ]
    record_pid=
}

record_monitor "$test_root/send-off.raw"
"$binary" --set-capture-send on
sleep 0.25
record_monitor "$test_root/send-on.raw"
"$binary" --set-capture-gain -20
sleep 0.25
record_monitor "$test_root/attenuated.raw"

python3 - "$test_root/send-off.raw" "$test_root/send-on.raw" "$test_root/attenuated.raw" <<'PY'
import math
import struct
import sys

def rms(path):
    raw = open(path, "rb").read()
    samples = struct.unpack("<%df" % (len(raw) // 4), raw)
    return math.sqrt(sum(sample * sample for sample in samples) / len(samples)) if samples else 0.0

off, on, attenuated = [rms(path) for path in sys.argv[1:]]
if off > 0.001:
    raise SystemExit("capture speaker send was audible while disabled: rms=%s" % off)
if on < 0.05:
    raise SystemExit("enabling capture speaker send produced no audible signal: rms=%s" % on)
if not on * 0.08 < attenuated < on * 0.12:
    raise SystemExit("capture gain did not attenuate the microphone signal by 20 dB: on=%s attenuated=%s" %
                     (on, attenuated))
PY

"$binary" stop
engine_running=0
echo "isolated capture source routing, default-off speaker send, and gain control passed"
