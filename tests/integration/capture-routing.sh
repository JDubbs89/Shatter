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
"$binary" make out "Capture Monitor"
listed_device_id="Shatter Test Speakers"
"$binary" assign out "Capture Monitor" "$listed_device_id"
inputs=$($binary --list-in)
source_selector=$(printf '%s\n' "$inputs" | awk -F'|' '/capture \/ source-/ { id=$2; gsub(/^[[:space:]]+|[[:space:]]+$/, "", id); sub(/^capture \/ /, "", id); print id; exit }')
case "$inputs" in
    *"$source_selector"*) ;;
    *) echo "automatic capture channel is missing from input list: $inputs" >&2; exit 1 ;;
esac

"$binary" make out "Virtual Mic" capture
output_listing=$("$binary" --list-out)
case "$output_listing" in
    *"Available application input captures"*"No application input captures found"*) ;;
    *) echo "Shatter's own capture stream was exposed as an assignable application input: $output_listing" >&2; exit 1 ;;
esac
"$binary" set fader capture "$source_selector" -6
fader_status=$("$binary" fader status)
case "$fader_status" in
    *"Capture"*"-6.0 dB"*) ;;
    *) echo "capture fader did not apply a relative decrement: $fader_status" >&2; exit 1 ;;
esac
"$binary" inc fader capture "$source_selector" +6
sleep 0.3
inputs=$($binary --list-in)
case "$inputs" in
    *"| off "*) ;;
    *) echo "capture speaker send should default off: $inputs" >&2; exit 1 ;;
esac
if ! "$binary" --set-capture-send off >/dev/null; then
    echo "could not keep capture speaker send disabled" >&2
    exit 1
fi
"$binary" assign capture "$source_selector" "Virtual Mic"

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
"$binary" assign capture "$source_selector" speakers
sleep 0.25
record_monitor "$test_root/send-on.raw"
capture_meter=$("$binary" meters capture)
if ! printf '%s\n' "$capture_meter" | awk -F'|' '$1 ~ /^capture / && $3 + 0 > -60 && $4 + 0 > -60 { found=1 } END { exit !found }'; then
    echo "selected capture meter did not report the active signal: $capture_meter" >&2
    exit 1
fi
virtual_mic_node=$(printf '%s\n' "$output_listing" | awk -F'|' '/Virtual Mic/ { id=$3; gsub(/^[[:space:]]+|[[:space:]]+$/, "", id); print id; exit }')
pw-record --target 0 --rate 48000 --channels 2 --format f32 --raw --sample-count 48000 "$test_root/virtual-mic.raw" \
    >"$test_root/virtual-mic-record.log" 2>&1 &
record_pid=$!
ready=0
for attempt in $(seq 1 40); do
    if pw-link -i | grep -q 'pw-record:input_FL'; then ready=1; break; fi
    sleep 0.05
done
if [ "$ready" -ne 1 ]; then
    kill "$record_pid" 2>/dev/null || true
    echo "virtual microphone recorder ports did not appear" >&2
    exit 1
fi
if ! pw-link "$virtual_mic_node:capture_FL" pw-record:input_FL >/dev/null; then
    echo "could not link virtual microphone node '$virtual_mic_node'; output ports: $(pw-link -o)" >&2
    exit 1
fi
pw-link "$virtual_mic_node:capture_FR" pw-record:input_FR >/dev/null
wait "$record_pid" || [ -s "$test_root/virtual-mic.raw" ]
record_pid=
virtual_mic_meter=$("$binary" meters "Virtual Mic")
if ! printf '%s\n' "$virtual_mic_meter" | awk -F'|' '$1 ~ /^output / && $3 + 0 > -60 && $4 + 0 > -60 { found=1 } END { exit !found }'; then
    echo "virtual microphone output meter did not report the actively consumed capture signal: $virtual_mic_meter" >&2
    exit 1
fi
"$binary" set mute capture "$source_selector" on
inputs=$("$binary" list in)
case "$inputs" in
    *"capture"*"yes"*) ;;
    *) echo "input listing did not show capture mute state: $inputs" >&2; exit 1 ;;
esac
sleep 0.25
record_monitor "$test_root/capture-muted.raw"
"$binary" set mute capture "$source_selector" off
"$binary" assign capture "$source_selector" "Capture Monitor"
"$binary" save show capture-routes
config_file="$XDG_CONFIG_HOME/shatter/capture-routes.conf"
grep -F 'I "device-name:Shatter Integration Test Source"' "$config_file" >/dev/null || {
    echo "saved show omitted the automatic capture channel identity" >&2; cat "$config_file" >&2; exit 1;
}
"$binary" unassign capture "$source_selector" speakers
"$binary" unassign capture "$source_selector" "Capture Monitor"
"$binary" delete out "Capture Monitor"
"$binary" load show capture-routes
if "$binary" load show missing >/dev/null 2>&1; then
    echo "expected loading a missing show to fail" >&2
    exit 1
fi
status=$("$binary" status)
case "$status" in
    *"Current show: capture-routes"*"Default show (next start): default"*"Autosave show (on stop): default"*) ;;
    *) echo "expected loaded show with unchanged startup and autosave settings, got: $status" >&2; exit 1 ;;
esac
inputs=$("$binary" list in)
case "$inputs" in
    *"$source_selector"*"Capture Monitor"*) ;;
    *) echo "loaded show did not restore capture sends: $inputs" >&2; exit 1 ;;
esac
case "$inputs" in
    *"$source_selector"*) ;;
    *) echo "loaded show did not preserve the automatically discovered capture channel: $inputs" >&2; exit 1 ;;
esac
outputs=$("$binary" list out)
case "$outputs" in
    *"Capture Monitor"*"$listed_device_id"*) ;;
    *) echo "loaded show did not restore the named output and sink binding: $outputs" >&2; exit 1 ;;
esac
sleep 0.25
record_monitor "$test_root/two-sends.raw"
"$binary" unassign capture "$source_selector" speakers
sleep 0.25
record_monitor "$test_root/named-send-only.raw"
"$binary" unassign capture "$source_selector" "Capture Monitor"
sleep 0.25
record_monitor "$test_root/all-sends-off.raw"
"$binary" assign capture "$source_selector" speakers
"$binary" --set-capture-gain -20
sleep 0.25
record_monitor "$test_root/attenuated.raw"

# Keep the playback output active while the capture source disconnects and is
# rediscovered so teardown overlaps real output callbacks.
pw-record --target 0 --rate 48000 --channels 2 --format f32 --raw --sample-count 240000 "$test_root/churn.raw" \
    >"$test_root/churn-record.log" 2>&1 &
record_pid=$!
ready=0
for attempt in $(seq 1 40); do
    if pw-link -i | grep -q 'pw-record:input_FL'; then ready=1; break; fi
    sleep 0.05
done
if [ "$ready" -ne 1 ]; then
    echo "capture churn recorder ports did not appear" >&2
    exit 1
fi
pw-link shatter-test-speakers:monitor_FL pw-record:input_FL >/dev/null
pw-link shatter-test-speakers:monitor_FR pw-record:input_FR >/dev/null

for cycle in 1 2; do
    kill "$source_pid"
    wait "$source_pid" 2>/dev/null || true
    source_pid=
    retained=0
    for attempt in $(seq 1 50); do
        inputs=$("$binary" list in)
        if printf '%s\n' "$inputs" | grep -Fq "$source_selector"; then retained=1; break; fi
        sleep 0.1
    done
    if [ "$retained" -ne 1 ]; then
        echo "capture channel did not retain its stable selector after source removal (cycle $cycle): $inputs" >&2
        exit 1
    fi

    "$test_source" >"$test_root/source-$cycle.log" 2>&1 &
    source_pid=$!
    restored=0
    for attempt in $(seq 1 50); do
        inputs=$("$binary" list in)
        if printf '%s\n' "$inputs" | grep -Fq "$source_selector"; then
            meter=$("$binary" meters capture)
            if printf '%s\n' "$meter" | awk -F'|' '$1 ~ /^capture / && $3 + 0 > -60 && $4 + 0 > -60 { found=1 } END { exit !found }'; then
                restored=1
                break
            fi
        fi
        sleep 0.1
    done
    if [ "$restored" -ne 1 ]; then
        echo "capture channel did not resume signal with its stable selector (cycle $cycle): $inputs; meter: $meter" >&2
        cat "$test_root/source-$cycle.log" >&2
        exit 1
    fi
done
wait "$record_pid" || [ -s "$test_root/churn.raw" ]
record_pid=

python3 - "$test_root/send-off.raw" "$test_root/send-on.raw" "$test_root/capture-muted.raw" "$test_root/two-sends.raw" \
    "$test_root/named-send-only.raw" "$test_root/all-sends-off.raw" "$test_root/attenuated.raw" \
    "$test_root/virtual-mic.raw" "$test_root/churn.raw" <<'PY'
import math
import struct
import sys

def rms(path):
    raw = open(path, "rb").read()
    samples = struct.unpack("<%df" % (len(raw) // 4), raw)
    return math.sqrt(sum(sample * sample for sample in samples) / len(samples)) if samples else 0.0

off, on, muted, two_sends, named_only, all_off, attenuated, virtual_mic, churn = [rms(path) for path in sys.argv[1:]]
if off > 0.001:
    raise SystemExit("capture speaker send was audible while disabled: rms=%s" % off)
if on < 0.05:
    raise SystemExit("enabling capture speaker send produced no audible signal: rms=%s" % on)
if muted > 0.001:
    raise SystemExit("muted capture channel remained audible: rms=%s" % muted)
if two_sends < on * 1.2:
    raise SystemExit("capture send to a second output did not add signal: one=%s two=%s" % (on, two_sends))
if named_only < 0.05:
    raise SystemExit("capture send stopped after independently unassigning speakers: rms=%s" % named_only)
if all_off > 0.001:
    raise SystemExit("capture remained audible after unassigning all outputs: rms=%s" % all_off)
if not on * 0.08 < attenuated < on * 0.12:
    raise SystemExit("capture gain did not attenuate the microphone signal by 20 dB: on=%s attenuated=%s" %
                     (on, attenuated))
if virtual_mic < 0.05:
    raise SystemExit("virtual microphone consumer received no capture signal: rms=%s" % virtual_mic)
if churn < 0.005:
    raise SystemExit("capture send did not resume after source rediscovery: rms=%s" % churn)
PY

"$binary" stop
engine_running=0
echo "isolated capture source routing, default-off speaker send, fader, and gain control passed"
