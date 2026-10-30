#!/bin/sh
set -eu

binary=$1
test_root=$(mktemp -d /tmp/shatter-pw-integration.XXXXXX)
runtime_dir="$test_root/runtime"
config_dir="$test_root/config"
home_dir="$test_root/home"
pipewire_pid=
wireplumber_pid=
engine_running=0
playback_pids=

cleanup()
{
    if [ "$engine_running" -eq 1 ]; then
        XDG_RUNTIME_DIR="$runtime_dir" "$binary" stop >/dev/null 2>&1 || true
    fi
    for pid in $playback_pids; do
        kill "$pid" 2>/dev/null || true
    done
    if [ -n "$wireplumber_pid" ]; then kill "$wireplumber_pid" 2>/dev/null || true; fi
    if [ -n "$pipewire_pid" ]; then kill "$pipewire_pid" 2>/dev/null || true; fi
    if [ -n "$wireplumber_pid" ]; then wait "$wireplumber_pid" 2>/dev/null || true; fi
    if [ -n "$pipewire_pid" ]; then wait "$pipewire_pid" 2>/dev/null || true; fi
    python3 - "$test_root" <<'PY'
import shutil
import sys
shutil.rmtree(sys.argv[1], ignore_errors=True)
PY
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$runtime_dir" "$config_dir/pipewire.conf.d" "$home_dir/.config" "$home_dir/.local/state" "$home_dir/.cache"
chmod 700 "$runtime_dir"
cp /usr/share/pipewire/pipewire.conf "$config_dir/pipewire.conf"
cp /usr/share/pipewire/client.conf "$config_dir/client.conf"
export HOME="$home_dir"
export XDG_RUNTIME_DIR="$runtime_dir"
export XDG_CONFIG_HOME="$home_dir/.config"
export XDG_STATE_HOME="$home_dir/.local/state"
export XDG_CACHE_HOME="$home_dir/.cache"
export PIPEWIRE_CONFIG_DIR="$config_dir"

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
pw-cli create-node adapter '{ factory.name = support.null-audio-sink node.name = shatter-test-headphones node.description = "Shatter Test Headphones" media.class = Audio/Sink audio.position = [ FL FR ] object.linger = true }' >/dev/null
sleep 0.3
sink_id=$(wpctl status -n | awk '/shatter-test-speakers/ { for (i=1; i<=NF; ++i) if ($i ~ /^[0-9]+\.$/) { id=$i; sub(/\.$/, "", id); print id; exit } }')
headphones_sink_id=$(wpctl status -n | awk '/shatter-test-headphones/ { for (i=1; i<=NF; ++i) if ($i ~ /^[0-9]+\.$/) { id=$i; sub(/\.$/, "", id); print id; exit } }')
if [ -z "$sink_id" ]; then
    cat "$test_root/wireplumber.log" >&2
    echo "isolated test sink was not created" >&2
    exit 1
fi
if [ -z "$headphones_sink_id" ]; then
    echo "isolated headphone sink was not created" >&2
    exit 1
fi
wpctl set-default "$sink_id" >/dev/null

python3 - "$test_root/sine.wav" "$test_root/sine-other.wav" <<'PY'
import random
import struct
import sys
import wave

rate = 48000
for path, seed in ((sys.argv[1], 440), (sys.argv[2], 880)):
    random.seed(seed)
    with wave.open(path, "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(rate)
        for frame in range(rate * 20):
            sample = random.randint(-8000, 8000)
            output.writeframesraw(struct.pack("<hh", sample, sample))
PY

pw-play --target default -P 'application.name=TestAppA application.id=org.shatter.test.appa' "$test_root/sine.wav" >/dev/null 2>&1 &
initial_playback_pid=$!
playback_pids="$playback_pids $initial_playback_pid"
sleep 0.3
"$binary" start
engine_running=1

wait_for_apps()
{
    expected=$1
    for attempt in $(seq 1 80); do
        inputs=$("$binary" --list-in)
        count=$(printf '%s\n' "$inputs" | grep -Ec '/ stream' || true)
        if [ "$count" -ge "$expected" ]; then
            printf '%s\n' "$inputs"
            return 0
        fi
        sleep 0.1
    done
    echo "$inputs" >&2
    echo "PipeWire playback streams were not assigned to app channels" >&2
    return 1
}

inputs=$(wait_for_apps 1)
stream_default_sends=$(printf '%s\n' "$inputs" | awk -F'|' '/\/ stream/ { gsub(/^[[:space:]]+|[[:space:]]+$/, "", $7); print $7; exit }')
app_default_sends=$(printf '%s\n' "$inputs" | awk -F'|' '/\/ app[[:space:]]*\|/ { gsub(/^[[:space:]]+|[[:space:]]+$/, "", $7); print $7; exit }')
if [ "$stream_default_sends" != "off" ] || [ "$app_default_sends" != "speakers" ]; then
    echo "playback streams should inherit the app's default speaker route without a direct send: $inputs" >&2
    exit 1
fi
app_a_id=$(printf '%s\n' "$inputs" | awk -F'|' '$3 ~ /TestAppA/ && $2 ~ /app-/ { split($2, channel, " / "); gsub(/^[[:space:]]+|[[:space:]]+$/, "", channel[1]); print channel[1]; exit }')
if [ -z "$app_a_id" ]; then
    echo "$inputs" >&2
    echo "app channel ID was not listed" >&2
    exit 1
fi

if "$binary" --profile-stop >/dev/null 2>&1; then
    echo "expected profile stop without a running profile to fail" >&2
    exit 1
fi
"$binary" --profile-start
if "$binary" --profile-start >/dev/null 2>&1; then
    echo "expected a second profile start to fail while profiling is active" >&2
    exit 1
fi
sleep 0.25
profile=$($binary --profile-stop)
case "$profile" in
    *"Shatter runtime profile"*"Speaker output"*"Frames unavailable at mix time"*"Shatter input ring occupancy avg/max"*"PipeWire max device delay"*) ;;
    *) echo "profile summary is missing runtime metrics: $profile" >&2; exit 1 ;;
esac
output_callbacks=$(printf '%s\n' "$profile" | awk -F'|' '/Speaker output/ { gsub(/[[:space:]]/, "", $3); print $3; exit }')
if [ -z "$output_callbacks" ] || [ "$output_callbacks" -le 0 ]; then
    echo "profile did not record speaker output callbacks: $profile" >&2
    exit 1
fi

record_monitor()
{
    destination=$1
    sample_count=${2:-48000}
    monitor_sink=${3:-shatter-test-speakers}
    pw-record --target 0 --rate 48000 --channels 2 --format f32 --raw --sample-count "$sample_count" "$destination" \
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
    pw-link "$monitor_sink:monitor_FL" pw-record:input_FL >/dev/null
    pw-link "$monitor_sink:monitor_FR" pw-record:input_FR >/dev/null
    wait "$record_pid" || [ -s "$destination" ]
}

audio_rms()
{
    python3 - "$1" <<'PY'
import math
import struct
import sys

raw = open(sys.argv[1], "rb").read()
samples = struct.unpack("<%df" % (len(raw) // 4), raw)
rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples)) if samples else 0.0
print(rms)
PY
}

assert_audio_rms_at_least()
{
    rms=$(audio_rms "$1")
    awk -v rms="$rms" -v threshold="$2" 'BEGIN { exit !(rms >= threshold) }' || {
        echo "$3: rms=$rms threshold=$2" >&2
        return 1
    }
}

sleep 1
record_monitor "$test_root/baseline.raw" 96000
"$binary" --set-gain app "$app_a_id" -20
sleep 0.25
record_monitor "$test_root/app-quiet.raw"
"$binary" --set-gain app "$app_a_id" 0

stream_a_id=$(printf '%s\n' "$inputs" | awk -F'|' -v channel="$app_a_id" '
    index($0, channel " / stream") { row=$6; sub(/^.*\(/, "", row); sub(/\).*/, "", row); print row; exit }
')
if [ -z "$stream_a_id" ]; then
    echo "$inputs" >&2
    echo "app playback stream ID was not listed" >&2
    exit 1
fi
"$binary" --set-gain stream "$stream_a_id" -20
"$binary" --set-mute stream "$stream_a_id" on
"$binary" --set-mute stream "$stream_a_id" off
"$binary" --set-mute app "$app_a_id" on
"$binary" --set-mute app "$app_a_id" off

# A named output is an independently routable PipeWire playback node.
if "$binary" make out "$headphones_sink_id" >/dev/null 2>&1; then
    echo "numeric output name was accepted even though numeric selectors mean PipeWire node IDs" >&2
    exit 1
fi
if "$binary" make out shatter.output.1 >/dev/null 2>&1; then
    echo "generated PipeWire node name was accepted as an output name" >&2
    exit 1
fi
if "$binary" make out shatter.speakers >/dev/null 2>&1; then
    echo "built-in PipeWire node name was accepted as an output name" >&2
    exit 1
fi
"$binary" make out "Headphone Monitor"
"$binary" assign out "Headphone Monitor" "sink-$headphones_sink_id"
"$binary" assign app "$app_a_id" "Headphone Monitor"
"$binary" --set-gain stream "$stream_a_id" 0
sleep 0.3
output_listing=$("$binary" --list-out)
case "$output_listing" in
    *"Headphone Monitor"*"sink-$headphones_sink_id"*) ;;
    *) echo "named output sink link is missing: $output_listing" >&2; pw-link -l >&2; exit 1 ;;
esac
record_monitor "$test_root/named-output.raw" 96000 shatter-test-headphones
assert_audio_rms_at_least "$test_root/named-output.raw" 0.005 \
    "assigned app channel produced no audio at the named output sink"
"$binary" assign stream "$stream_a_id" speakers
"$binary" assign stream "$stream_a_id" "Headphone Monitor"
sleep 0.3
record_monitor "$test_root/app-and-stream-output.raw" 96000 shatter-test-headphones
app_and_stream_rms=$(audio_rms "$test_root/app-and-stream-output.raw")
app_only_rms=$(audio_rms "$test_root/named-output.raw")
awk -v combined="$app_and_stream_rms" -v app_only="$app_only_rms" \
    'BEGIN { exit !(combined >= app_only * 1.2) }' || {
    echo "direct stream send did not add signal to the app send: app_only=$app_only_rms combined=$app_and_stream_rms" >&2
    exit 1
}
"$binary" unassign app "$app_a_id" "Headphone Monitor"
"$binary" --set-gain app "$app_a_id" -20
sleep 0.3
record_monitor "$test_root/stream-only-output.raw" 96000 shatter-test-headphones
assert_audio_rms_at_least "$test_root/stream-only-output.raw" 0.005 \
    "stream send stopped when its independent app send was removed"
stream_only_rms=$(audio_rms "$test_root/stream-only-output.raw")
awk -v stream="$stream_only_rms" -v app="$app_only_rms" 'BEGIN { exit !(stream < app * 0.2) }' || {
    echo "direct stream send did not inherit parent app gain: app=$app_only_rms stream=$stream_only_rms" >&2
    exit 1
}
"$binary" --set-gain app "$app_a_id" 0
"$binary" assign app "$app_a_id" "Headphone Monitor"
"$binary" assign out "Headphone Monitor" "sink-$sink_id"
sleep 0.3
pw-link -l >&2
output_listing=$("$binary" --list-out)
case "$output_listing" in
    *"Headphone Monitor"*"sink-$headphones_sink_id"*"sink-$sink_id"*) ;;
    *) echo "output was not listed on both playback sinks: $output_listing" >&2; pw-link -l >&2; exit 1 ;;
esac
record_monitor "$test_root/multi-sink-speakers.raw" 96000 shatter-test-speakers
python3 - "$test_root/multi-sink-speakers.raw" <<'PY'
import math
import struct
import sys

raw = open(sys.argv[1], "rb").read()
samples = struct.unpack("<%df" % (len(raw) // 4), raw)
rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples)) if samples else 0.0
if rms < 0.005:
    raise SystemExit("named output did not deliver audio to its second sink: rms=%s" % rms)
PY
"$binary" unassign out "Headphone Monitor" "sink-$headphones_sink_id"
sleep 0.3
output_row=$(printf '%s\n' "$($binary --list-out)" | awk '/Headphone Monitor/ { print; exit }')
case "$output_row" in
    *"sink-$sink_id"*) ;;
    *) echo "unassigning one sink removed the remaining route: $output_row" >&2; exit 1 ;;
esac
record_monitor "$test_root/remaining-sink.raw" 96000 shatter-test-speakers
python3 - "$test_root/remaining-sink.raw" <<'PY'
import math
import struct
import sys

raw = open(sys.argv[1], "rb").read()
samples = struct.unpack("<%df" % (len(raw) // 4), raw)
rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples)) if samples else 0.0
if rms < 0.005:
    raise SystemExit("remaining sink stopped receiving output audio: rms=%s" % rms)
PY
"$binary" unassign out "Headphone Monitor" "sink-$sink_id"
sleep 0.3
output_row=$(printf '%s\n' "$($binary --list-out)" | awk '/Headphone Monitor/ { print; exit }')
case "$output_row" in
    *"sink-"*) echo "all routes were not removed: $output_row" >&2; exit 1 ;;
    *) ;;
esac
"$binary" unassign app "$app_a_id" "Headphone Monitor"
"$binary" unassign stream "$stream_a_id" "Headphone Monitor"
"$binary" delete out "Headphone Monitor"
"$binary" unassign stream "$stream_a_id" speakers
output_listing=$("$binary" --list-out)
case "$output_listing" in
    *"Headphone Monitor"*) echo "deleted output channel remained in the output list: $output_listing" >&2; exit 1 ;;
    *) ;;
esac

kill "$initial_playback_pid"
wait "$initial_playback_pid" 2>/dev/null || true
removed=0
for attempt in $(seq 1 50); do
    inputs=$("$binary" --list-in)
    if ! printf '%s\n' "$inputs" | grep -q "($stream_a_id)"; then removed=1; break; fi
    sleep 0.1
done
if [ "$removed" -ne 1 ]; then
    echo "$inputs" >&2
    echo "removed playback stream remained assigned to its channel" >&2
    exit 1
fi

pw-play --target default -P 'application.name=TestAppA application.id=org.shatter.test.appa' "$test_root/sine.wav" >/dev/null 2>&1 &
playback_pids="$playback_pids $!"
pw-play --target default -P 'application.name=TestAppA application.id=org.shatter.test.appa' "$test_root/sine-other.wav" >/dev/null 2>&1 &
playback_pids="$playback_pids $!"
pw-play --target default -P 'application.name=TestAppB application.id=org.shatter.test.appb' "$test_root/sine.wav" >/dev/null 2>&1 &
playback_pids="$playback_pids $!"
inputs=$(wait_for_apps 3)
app_b_id=$(printf '%s\n' "$inputs" | awk -F'|' '$3 ~ /TestAppB/ && $2 ~ /app-/ { split($2, channel, " / "); gsub(/^[[:space:]]+|[[:space:]]+$/, "", channel[1]); print channel[1]; exit }')
if [ -z "$app_b_id" ] || [ "$app_a_id" = "$app_b_id" ]; then
    echo "$inputs" >&2
    echo "separate applications did not receive separate channels" >&2
    exit 1
fi
stream_a_ids=$(printf '%s\n' "$inputs" | awk -F'|' -v channel="$app_a_id" '
    index($0, channel " / stream") { row=$6; sub(/^.*\(/, "", row); sub(/\).*/, "", row); print row }
')
set -- $stream_a_ids
if [ "$#" -ne 2 ]; then
    echo "$inputs" >&2
    echo "expected two playback streams assigned to TestAppA" >&2
    exit 1
fi
stream_a_first=$1
stream_a_second=$2
"$binary" --set-mute app "$app_b_id" on
"$binary" --set-gain stream "$stream_a_first" 0
"$binary" --set-gain stream "$stream_a_second" 0
sleep 0.25
record_monitor "$test_root/paired-baseline.raw"
"$binary" --set-gain stream "$stream_a_second" -20
sleep 0.25
record_monitor "$test_root/paired-attenuated.raw"
"$binary" --set-gain stream "$stream_a_second" 0
sleep 0.25
"$binary" --set-mute stream "$stream_a_second" on
sleep 0.25
record_monitor "$test_root/one-stream-muted.raw"
"$binary" --set-mute stream "$stream_a_second" off
"$binary" --set-mute app "$app_b_id" off

link_count=$(pw-link -l | grep -c 'shatter.stream.' || true)
if [ "$link_count" -lt 6 ]; then
    pw-link -l >&2
    echo "expected all three stereo app streams to link to their Shatter ingress endpoints" >&2
    exit 1
fi

"$binary" stop
engine_running=0
restored=0
for attempt in $(seq 1 50); do
    link_count=$(pw-link -l | grep -c 'shatter-test-speakers:playback_' || true)
    if [ "$link_count" -ge 6 ]; then restored=1; break; fi
    sleep 0.1
done
if [ "$restored" -ne 1 ]; then
    pw-link -l >&2
    echo "application streams did not return to the ordinary PipeWire sink after Shatter stopped" >&2
    exit 1
fi

python3 - "$test_root/baseline.raw" "$test_root/app-quiet.raw" "$test_root/paired-baseline.raw" "$test_root/paired-attenuated.raw" "$test_root/one-stream-muted.raw" <<'PY'
import math
import struct
import sys

def rms(path):
    data = open(path, "rb").read()
    samples = struct.unpack("<%df" % (len(data) // 4), data)
    return math.sqrt(sum(sample * sample for sample in samples) / len(samples)) if samples else 0.0

baseline_data = open(sys.argv[1], "rb").read()
baseline_samples = struct.unpack("<%df" % (len(baseline_data) // 4), baseline_data)
window_frames = 480
window_rms = [
    math.sqrt(sum(sample * sample for sample in baseline_samples[start * 2:(start + window_frames) * 2]) /
              (window_frames * 2))
    for start in range(0, len(baseline_samples) // 2 - window_frames + 1, window_frames)
]
steady_windows = window_rms[10:-10]
median_window = sorted(steady_windows)[len(steady_windows) // 2]
quiet_windows = [value < median_window * 0.2 for value in steady_windows]
longest_quiet_run = current_quiet_run = 0
quiet_runs = []
run_start = None
for index, is_quiet in enumerate(quiet_windows):
    if is_quiet:
        if run_start is None:
            run_start = index
        current_quiet_run += 1
        longest_quiet_run = max(longest_quiet_run, current_quiet_run)
    else:
        if current_quiet_run:
            quiet_runs.append((run_start * 10 + 100, current_quiet_run * 10))
        current_quiet_run = 0
        run_start = None
if current_quiet_run:
    quiet_runs.append((run_start * 10 + 100, current_quiet_run * 10))
if median_window < 0.01 or longest_quiet_run >= 5:
    raise SystemExit("speaker output contains sustained audio gaps: median_window_rms=%s gaps_ms=%s" %
                     (median_window, quiet_runs))
baseline = rms(sys.argv[1])
quiet = rms(sys.argv[2])
paired = rms(sys.argv[3])
attenuated = rms(sys.argv[4])
one_muted = rms(sys.argv[5])
if baseline < 0.01 or quiet > baseline * 0.7:
    raise SystemExit("app channel gain did not produce the expected output change: baseline=%s quiet=%s" % (baseline, quiet))
if paired < 0.01 or not paired * 0.4 < attenuated < paired * 0.9:
    raise SystemExit("stream gain did not affect only its stream: paired=%s attenuated=%s" % (paired, attenuated))
if not paired * 0.4 < one_muted < paired * 0.9:
    raise SystemExit("stream mute did not mute only its stream: paired=%s one-muted=%s" % (paired, one_muted))
PY

echo "isolated app playback routing, grouping, controls, and fallback passed"
