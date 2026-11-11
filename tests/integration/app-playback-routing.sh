#!/bin/sh
set -eu

binary=$1
input_probe_binary=$2
test_root=$(mktemp -d /tmp/shatter-pw-integration.XXXXXX)
runtime_dir="$test_root/runtime"
config_dir="$test_root/config"
home_dir="$test_root/home"
pipewire_pid=
wireplumber_pid=
engine_running=0
playback_pids=
input_probe_pid=

cleanup()
{
    if [ "$engine_running" -eq 1 ]; then
        XDG_RUNTIME_DIR="$runtime_dir" "$binary" stop >/dev/null 2>&1 || true
    fi
    for pid in $playback_pids; do
        kill "$pid" 2>/dev/null || true
    done
    if [ -n "$input_probe_pid" ]; then kill "$input_probe_pid" 2>/dev/null || true; fi
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
pw-cli create-node adapter '{ factory.name = support.null-audio-sink node.name = shatter-test-anchor node.description = "Shatter Test Anchor" media.class = Audio/Sink audio.position = [ FL FR ] object.linger = true }' >/dev/null
pw-cli create-node adapter '{ factory.name = support.null-audio-sink node.name = shatter-test-headphones node.description = "Shatter Test Headphones" device.serial = shatter-test-headphones-serial object.path = shatter-test-headphones-path media.class = Audio/Sink audio.position = [ FL FR ] object.linger = true }' >/dev/null
pw-cli create-node adapter '{ factory.name = support.null-audio-sink node.name = shatter-test-discord-mic node.description = "Discord Microphone Input Capture" application.name = Discord application.id = com.discordapp.Discord media.class = Stream/Input/Audio audio.position = [ FL FR ] object.linger = true }' >/dev/null
application_input_node_id=$(pw-cli ls Node | awk '/^[[:space:]]*id [0-9]+,/ { id=$2; sub(/,$/, "", id) } index($0, "node.name = \"shatter-test-discord-mic\"") { print id; exit }')
if [ -z "$application_input_node_id" ]; then
    echo "could not determine the mock application input node ID" >&2
    exit 1
fi
sleep 0.3
sink_id=$(wpctl status -n | awk '/shatter-test-speakers/ { for (i=1; i<=NF; ++i) if ($i ~ /^[0-9]+\.$/) { id=$i; sub(/\.$/, "", id); print id; exit } }')
anchor_sink_id=$(wpctl status -n | awk '/shatter-test-anchor/ { for (i=1; i<=NF; ++i) if ($i ~ /^[0-9]+\.$/) { id=$i; sub(/\.$/, "", id); print id; exit } }')
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
if [ -z "$anchor_sink_id" ]; then
    echo "isolated anchor sink was not created" >&2
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
        for frame in range(rate * 40):
            sample = random.randint(-8000, 8000)
            output.writeframesraw(struct.pack("<hh", sample, sample))
PY

pw-play --target default -P 'application.name=TestAppA application.id=org.shatter.test.appa' "$test_root/sine.wav" >/dev/null 2>&1 &
initial_playback_pid=$!
playback_pids="$playback_pids $initial_playback_pid"
sleep 0.3
"$binary" start
engine_running=1
health=$("$binary" health)
case "$health" in
    *"Engine health: "*"Xruns since engine start"*"I/O buffering estimate:"*"Shatter playback input ring occupancy"*) ;;
    *) echo "expected live engine health and latency details, got: $health" >&2; exit 1 ;;
esac

listed_device_id()
{
    device_name=$1
    "$binary" --list-out | awk -F'|' -v wanted="$device_name" 'index($0, wanted) { id=$2; gsub(/^[[:space:]]+|[[:space:]]+$/, "", id); print id; exit }'
}

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
if printf '%s\n' "$inputs" | grep -Eq 'PipeWire|sink-[0-9]+|source-[0-9]+'; then
    echo "input listing exposed a backend-specific selector: $inputs" >&2
    exit 1
fi
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
stream_a_id=$(printf '%s\n' "$inputs" | awk -F'|' '$3 ~ /TestAppA/ && $2 ~ /stream/ { if (match($6, /stream-[0-9]+/)) { print substr($6, RSTART, RLENGTH); exit } }')
selected_meter=$("$binary" meters "$stream_a_id")
case "$selected_meter" in
    *"stream | $stream_a_id "*) ;;
    *) echo "selected meter query did not return the requested stream: $selected_meter" >&2; exit 1 ;;
esac

"$binary" inc fader app "$app_a_id" +20
fader_status=$("$binary" fader status)
case "$fader_status" in
    *"Channel name"*"Fader position"*"TestAppA"*"+12.0 dB"*) ;;
    *) echo "fader increment did not clamp at +12 dB or status table is incomplete: $fader_status" >&2; exit 1 ;;
esac
"$binary" inc fader app "$app_a_id" -20
fader_status=$("$binary" fader status)
case "$fader_status" in
    *"TestAppA"*"-8.0 dB"*) ;;
    *) echo "fader decrement was not relative to the current level: $fader_status" >&2; exit 1 ;;
esac
"$binary" set fader app "$app_a_id" -inf
fader_status=$("$binary" fader status)
case "$fader_status" in
    *"TestAppA"*"-inf"*) ;;
    *) echo "-inf did not put the app fader at its minimum: $fader_status" >&2; exit 1 ;;
esac
"$binary" inc fader app "$app_a_id" +60

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
    index($0, channel " / stream") { row=$6; gsub(/^[[:space:]]+/, "", row); sub(/[[:space:]].*$/, "", row); print row; exit }
')
if [ -z "$stream_a_id" ]; then
    echo "$inputs" >&2
    echo "app playback stream ID was not listed" >&2
    exit 1
fi
"$binary" inc fader stream "$stream_a_id" +20
"$binary" inc fader stream "$stream_a_id" -20
fader_status=$("$binary" fader status)
case "$fader_status" in
    *"pw-play"*"-8.0 dB"*) ;;
    *) echo "playback-stream fader did not apply relative increments: $fader_status" >&2; exit 1 ;;
esac
"$binary" set fader stream "$stream_a_id" -inf
"$binary" inc fader stream "$stream_a_id" +60
"$binary" --set-gain stream "$stream_a_id" -20
"$binary" --set-mute stream "$stream_a_id" on
"$binary" --set-mute stream "$stream_a_id" off
"$binary" --set-mute app "$app_a_id" on
"$binary" --set-mute app "$app_a_id" off

# Numeric output names are ordinary Shatter names and do not select backend nodes.
"$binary" make out 57
"$binary" delete out 57
if "$binary" make out shatter.output.1 >/dev/null 2>&1; then
    echo "generated PipeWire node name was accepted as an output name" >&2
    exit 1
fi
if "$binary" make out shatter.speakers >/dev/null 2>&1; then
    echo "built-in PipeWire node name was accepted as an output name" >&2
    exit 1
fi
"$binary" make out "Headphone Monitor"
"$binary" inc fader output "Headphone Monitor" +20
fader_status=$("$binary" fader status)
case "$fader_status" in
    *"Headphone Monitor"*"+12.0 dB"*) ;;
    *) echo "output fader was missing or did not clamp at +12 dB: $fader_status" >&2; exit 1 ;;
esac
"$binary" inc fader output "Headphone Monitor" -20
fader_status=$("$binary" fader status)
case "$fader_status" in
    *"Headphone Monitor"*"-8.0 dB"*) ;;
    *) echo "output fader did not apply relative increments: $fader_status" >&2; exit 1 ;;
esac
"$binary" set fader output "Headphone Monitor" -inf
"$binary" inc fader output "Headphone Monitor" +60
anchor_device_id=$(listed_device_id "Shatter Test Anchor")
"$binary" assign out "Headphone Monitor" "$anchor_device_id"
headphones_device_id=$(listed_device_id "Shatter Test Headphones")
"$binary" assign out "Headphone Monitor" "$headphones_device_id"
"$binary" assign app "$app_a_id" "Headphone Monitor"
application_input_listing=$("$binary" --list-out)
application_input_id=$(printf '%s\n' "$application_input_listing" | awk -F'|' '/Discord Microphone Input Capture/ { id=$2; gsub(/^[[:space:]]+|[[:space:]]+$/, "", id); print id; exit }')
case "$application_input_listing" in
    *"Available application input captures"*"Discord / Discord Microphone Input Capture"*) ;;
    *) echo "application microphone input was not listed as an output destination: $application_input_listing" >&2; exit 1 ;;
esac
if [ -z "$application_input_id" ]; then
    echo "application microphone input did not receive an assignable ID: $application_input_listing" >&2
    exit 1
fi
"$binary" assign out "Headphone Monitor" "$application_input_id"
input_linked=0
for attempt in $(seq 1 50); do
    if pw-link -l | grep -A1 'shatter-test-discord-mic:input_FL' | grep -q 'shatter.output.1:output_FL'; then
        input_linked=1
        break
    fi
    sleep 0.1
done
if [ "$input_linked" -ne 1 ]; then
    pw-link -l >&2
    echo "output channel did not link to the application microphone input" >&2
    exit 1
fi
probe_result="$test_root/application-input-rms.txt"
"$input_probe_binary" shatter-test-audio-input "$probe_result" 2500 &
input_probe_pid=$!
probe_input_id=
for attempt in $(seq 1 50); do
    application_input_listing=$("$binary" --list-out)
    probe_input_id=$(printf '%s\n' "$application_input_listing" | awk -F'|' '/Shatter Integration Application Input/ { id=$2; gsub(/^[[:space:]]+|[[:space:]]+$/, "", id); print id; exit }')
    if [ -n "$probe_input_id" ]; then break; fi
    sleep 0.1
done
if [ -z "$probe_input_id" ]; then
    echo "PipeWire input probe did not appear in the application capture list: $application_input_listing" >&2
    exit 1
fi
"$binary" assign out "Headphone Monitor" "$probe_input_id"
probe_linked=0
for attempt in $(seq 1 50); do
    if pw-link -l | grep -A1 'shatter-test-audio-input:input_FL' | grep -q 'shatter.output.1:output_FL'; then
        probe_linked=1
        break
    fi
    sleep 0.1
done
if [ "$probe_linked" -ne 1 ]; then
    pw-link -l >&2
    echo "output channel did not link to the audio-measuring application input" >&2
    exit 1
fi
"$binary" save show application-input-identity-check
grep -F 'app-input:Shatter Integration Probe:shatter-test-audio-input' \
    "$XDG_CONFIG_HOME/shatter/application-input-identity-check.conf" >/dev/null || {
    echo "application input config omitted its PipeWire application identity" >&2
    cat "$XDG_CONFIG_HOME/shatter/application-input-identity-check.conf" >&2
    exit 1
}
sleep 0.5
"$binary" unassign out "Headphone Monitor" "$probe_input_id"
wait "$input_probe_pid"
input_probe_pid=
probe_rms=$(cat "$probe_result")
if ! awk -v rms="$probe_rms" 'BEGIN { exit !(rms + 0 >= 0.001) }'; then
    echo "application input received no routed audio samples: RMS=$probe_rms" >&2
    exit 1
fi
"$binary" save show application-input-route-check
application_input_config="$XDG_CONFIG_HOME/shatter/application-input-route-check.conf"
grep -F 'app-input:Discord:shatter-test-discord-mic' "$application_input_config" >/dev/null || {
    echo "saved output config omitted the stable application input identity" >&2
    cat "$application_input_config" >&2
    exit 1
}
"$binary" unassign out "Headphone Monitor" "$application_input_id"
if pw-link -l | grep -A1 'shatter-test-discord-mic:input_FL' | grep -q 'shatter.output.1:output_FL'; then
    echo "unassigning the application input left its output link in place" >&2
    exit 1
fi
"$binary" load show application-input-route-check
restored_input_link=0
for attempt in $(seq 1 50); do
    if pw-link -l | grep -A1 'shatter-test-discord-mic:input_FL' | grep -q 'shatter.output.1:output_FL'; then
        restored_input_link=1
        break
    fi
    sleep 0.1
done
if [ "$restored_input_link" -ne 1 ]; then
    pw-link -l >&2
    echo "loading an output config did not restore its application input route" >&2
    exit 1
fi
pw-cli destroy "$application_input_node_id" >/dev/null
reconnecting_application_input=0
for attempt in $(seq 1 50); do
    output_listing=$("$binary" --list-out)
    case "$output_listing" in
        *"reconnecting"*) reconnecting_application_input=1; break ;;
    esac
    sleep 0.1
done
if [ "$reconnecting_application_input" -ne 1 ]; then
    echo "removed application input was not reported as reconnecting: $output_listing" >&2
    exit 1
fi
pw-cli create-node adapter '{ factory.name = support.null-audio-sink node.name = shatter-test-discord-mic node.description = "Discord Microphone Input Capture" application.name = Discord application.id = com.discordapp.Discord media.class = Stream/Input/Audio audio.position = [ FL FR ] object.linger = true }' >/dev/null
reconnected_application_input=0
for attempt in $(seq 1 50); do
    output_listing=$("$binary" --list-out)
    if printf '%s\n' "$output_listing" | grep -F "$application_input_id" | grep -q 'Discord Microphone Input Capture'; then
        if pw-link -l | grep -A1 'shatter-test-discord-mic:input_FL' | grep -q 'shatter.output.1:output_FL'; then
            reconnected_application_input=1
            break
        fi
    fi
    sleep 0.1
done
if [ "$reconnected_application_input" -ne 1 ]; then
    pw-link -l >&2
    echo "recreated application input did not restore its stable output route: $output_listing" >&2
    exit 1
fi
"$binary" unassign out "Headphone Monitor" "$application_input_id"
"$binary" --set-gain stream "$stream_a_id" 0
pw-cli destroy "$headphones_sink_id" >/dev/null
reconnecting=0
for attempt in $(seq 1 50); do
    output_listing=$("$binary" --list-out)
    case "$output_listing" in
        *"reconnecting"*) reconnecting=1; break ;;
    esac
    sleep 0.1
done
if [ "$reconnecting" -ne 1 ]; then
    echo "$output_listing" >&2
    echo "removed playback sink was not reported as reconnecting" >&2
    exit 1
fi
pw-cli create-node adapter '{ factory.name = support.null-audio-sink node.name = shatter-test-headphones node.description = "Shatter Test Headphones" device.serial = shatter-test-headphones-serial object.path = shatter-test-headphones-path media.class = Audio/Sink audio.position = [ FL FR ] object.linger = true }' >/dev/null
headphones_sink_id=
for attempt in $(seq 1 50); do
    headphones_sink_id=$(wpctl status -n | awk '/shatter-test-headphones/ { for (i=1; i<=NF; ++i) if ($i ~ /^[0-9]+\.$/) { id=$i; sub(/\.$/, "", id); print id; exit } }')
    if [ -n "$headphones_sink_id" ]; then
        output_listing=$("$binary" --list-out)
        case "$output_listing" in
            *"$headphones_device_id"*) break ;;
            *) headphones_sink_id= ;;
        esac
    fi
    sleep 0.1
done
if [ -z "$headphones_sink_id" ]; then
    echo "$output_listing" >&2
    echo "recreated playback sink did not restore its named output binding" >&2
    exit 1
fi
output_listing=$("$binary" --list-out)
case "$output_listing" in
    *"Headphone Monitor"*"$headphones_device_id"*) ;;
    *) echo "named output sink link is missing: $output_listing" >&2; pw-link -l >&2; exit 1 ;;
esac
reconnected_link=0
for attempt in $(seq 1 50); do
    if pw-link -l | grep -A1 'shatter-test-headphones:playback_FL' | grep -q 'shatter.output.1:output_FL' &&
       pw-link -l | grep -A1 'shatter-test-headphones:playback_FR' | grep -q 'shatter.output.1:output_FR'; then
        reconnected_link=1
        break
    fi
    sleep 0.1
done
if [ "$reconnected_link" -ne 1 ]; then
    echo "$output_listing" >&2
    pw-link -l >&2
    echo "named output did not reconnect to its playback sink" >&2
    exit 1
fi
record_monitor "$test_root/named-output.raw" 96000 shatter-test-headphones
assert_audio_rms_at_least "$test_root/named-output.raw" 0.005 \
    "assigned app channel produced no audio at the named output sink"
meters=$("$binary" meters)
if ! printf '%s\n' "$meters" | awk -F'|' '{ scope=$1; gsub(/^[[:space:]]+|[[:space:]]+$/, "", scope) } scope ~ /^(app|stream|output)$/ && $3 + 0 > -60 && $4 + 0 > -60 { found[scope]=1 } END { exit !(found["app"] && found["stream"] && found["output"]) }'; then
    echo "live meters did not report active app, stream, and output audio: $meters" >&2
    exit 1
fi
selected_meter=$("$binary" meters "$stream_a_id")
if ! printf '%s\n' "$selected_meter" | awk -F'|' '$1 ~ /^stream / && $3 + 0 > -60 && $4 + 0 > -60 { found=1 } END { exit !found }'; then
    echo "selected stream meter did not report the active signal: $selected_meter" >&2
    exit 1
fi
"$binary" set gain output "Headphone Monitor" -20
output_listing=$("$binary" list out)
case "$output_listing" in
    *"Headphone Monitor"*"-20.0 dB"*) ;;
    *) echo "output listing did not show the selected fader level: $output_listing" >&2; exit 1 ;;
esac
sleep 0.3
record_monitor "$test_root/output-attenuated.raw" 96000 shatter-test-headphones
attenuated_output_rms=$(audio_rms "$test_root/output-attenuated.raw")
awk -v baseline="$(audio_rms "$test_root/named-output.raw")" -v attenuated="$attenuated_output_rms" \
    'BEGIN { exit !(attenuated > baseline * 0.08 && attenuated < baseline * 0.12) }' || {
    echo "output fader did not attenuate its mixed signal by 20 dB: baseline=$(audio_rms "$test_root/named-output.raw") attenuated=$attenuated_output_rms" >&2
    exit 1
}
"$binary" set mute output "Headphone Monitor" on
output_listing=$("$binary" list out)
case "$output_listing" in
    *"Headphone Monitor"*"yes"*) ;;
    *) echo "output listing did not show the mute state: $output_listing" >&2; exit 1 ;;
esac
sleep 0.3
record_monitor "$test_root/output-muted.raw" 96000 shatter-test-headphones
if awk -v rms="$(audio_rms "$test_root/output-muted.raw")" 'BEGIN { exit !(rms > 0.001) }'; then
    echo "muted output channel remained audible" >&2
    exit 1
fi
"$binary" set mute output "Headphone Monitor" off
"$binary" set gain output "Headphone Monitor" 0
"$binary" unassign out "Headphone Monitor" "$anchor_device_id"
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
speakers_device_id=$(listed_device_id "Shatter Test Speakers")
"$binary" assign out "Headphone Monitor" "$speakers_device_id"
sleep 0.3
pw-link -l >&2
output_listing=$("$binary" --list-out)
case "$output_listing" in
    *"Headphone Monitor"*"$headphones_device_id"*"$speakers_device_id"*) ;;
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
# Keep the system-default sink as the output's primary target; headphones are an explicit secondary route.
"$binary" unassign out "Headphone Monitor" "$headphones_device_id"
headphones_device_id=$(listed_device_id "Shatter Test Headphones")
"$binary" assign out "Headphone Monitor" "$headphones_device_id"
"$binary" save show routes
config_file="$XDG_CONFIG_HOME/shatter/routes.conf"
grep -F 'O 1 "shatter.output.1" "Headphone Monitor"' "$config_file" >/dev/null || {
    echo "saved show omitted the output name or generated identity" >&2; cat "$config_file" >&2; exit 1;
}
grep -F 'D "shatter.output.1" "device:shatter-test-headphones-path" "shatter-test-headphones"' "$config_file" >/dev/null || {
    echo "saved show omitted the stable physical sink identity" >&2; cat "$config_file" >&2; exit 1;
}
"$binary" delete out "Headphone Monitor"
cp "$config_file" "$test_root/routes.conf"
sed 's/"device:shatter-test-headphones-path"/"device:missing-device"/' \
    "$test_root/routes.conf" >"$config_file"
"$binary" load show routes
output_listing=$("$binary" list out)
case "$output_listing" in
    *"Headphone Monitor"*"reconnecting"*) ;;
    *) echo "loading a show did not retain its missing physical sink binding: $output_listing" >&2; exit 1 ;;
esac
mv "$test_root/routes.conf" "$config_file"
"$binary" load show routes
input_listing=$("$binary" list in)
case "$input_listing" in
    *"TestAppA"*"Headphone Monitor"*"TestAppA"*"Headphone Monitor"*) ;;
    *) echo "loading a show did not restore active app and stream sends: $input_listing" >&2; exit 1 ;;
esac
"$binary" --set-gain stream "$stream_a_id" -7
"$binary" --set-mute stream "$stream_a_id" on
"$binary" save show inactive-stream
grep -F 'S "app-name:TestAppA" "pw-play" "pw-play" -7 1 2 "shatter.speakers" "shatter.output.1"' \
    "$XDG_CONFIG_HOME/shatter/inactive-stream.conf" >/dev/null || {
    echo "saved inactive playback stream omitted its output send" >&2
    cat "$XDG_CONFIG_HOME/shatter/inactive-stream.conf" >&2
    exit 1
}
"$binary" --set-gain stream "$stream_a_id" 0
"$binary" --set-mute stream "$stream_a_id" off
headphones_sink_id=
for attempt in $(seq 1 50); do
    headphones_sink_id=$(wpctl status -n | awk '/shatter-test-headphones/ { for (i=1; i<=NF; ++i) if ($i ~ /^[0-9]+\.$/) { id=$i; sub(/\.$/, "", id); print id; exit } }')
    output_listing=$("$binary" list out)
    if [ -n "$headphones_sink_id" ] && printf '%s\n' "$output_listing" | awk -v sink="$headphones_device_id" \
        '/Headphone Monitor/ && index($0, sink) { found=1 } END { exit !found }'; then break; fi
    headphones_sink_id=
    sleep 0.1
done
if [ -z "$headphones_sink_id" ]; then
    echo "loaded show did not rebind its saved sink by stable identity: $output_listing" >&2
    exit 1
fi
reconnected_link=0
for attempt in $(seq 1 50); do
    if pw-link -l | grep -A1 'shatter-test-headphones:playback_FL' | grep -q 'shatter.output.1:output_FL' &&
       pw-link -l | grep -A1 'shatter-test-headphones:playback_FR' | grep -q 'shatter.output.1:output_FR'; then
        reconnected_link=1
        break
    fi
    sleep 0.1
done
if [ "$reconnected_link" -ne 1 ]; then
    pw-link -l >&2
    echo "loaded show did not restore its saved physical sink route" >&2
    exit 1
fi
"$binary" unassign out "Headphone Monitor" "$headphones_device_id"
sleep 0.3
output_row=$(printf '%s\n' "$($binary --list-out)" | awk '/Headphone Monitor/ { print; exit }')
case "$output_row" in
    *"$speakers_device_id"*) ;;
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
"$binary" unassign out "Headphone Monitor" "$speakers_device_id"
sleep 0.3
output_row=$(printf '%s\n' "$($binary --list-out)" | awk '/Headphone Monitor/ { print; exit }')
case "$output_row" in
    *"device-"*) echo "all routes were not removed: $output_row" >&2; exit 1 ;;
    *) ;;
esac
"$binary" unassign app "$app_a_id" "Headphone Monitor"
"$binary" unassign stream "$stream_a_id" speakers

"$binary" --set-gain stream "$stream_a_id" -7
"$binary" --set-mute stream "$stream_a_id" on
kill "$initial_playback_pid"
wait "$initial_playback_pid" 2>/dev/null || true
removed=0
for attempt in $(seq 1 50); do
    inputs=$("$binary" --list-in)
    if ! printf '%s\n' "$inputs" | grep -q "$stream_a_id"; then removed=1; break; fi
    sleep 0.1
done
if [ "$removed" -ne 1 ]; then
    echo "$inputs" >&2
    echo "removed playback stream remained assigned to its channel" >&2
    exit 1
fi

pw-play --target default -P 'application.name=TestAppB application.id=org.shatter.test.appb' "$test_root/sine.wav" >/dev/null 2>&1 &
unrelated_playback_pid=$!
playback_pids="$playback_pids $unrelated_playback_pid"
inputs=$(wait_for_apps 1)
unrelated_state=$(printf '%s\n' "$inputs" | awk -F'|' '$3 ~ /TestAppB/ && $2 ~ /\/ stream/ {
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", $4); gsub(/^[[:space:]]+|[[:space:]]+$/, "", $5);
    print $4 ":" $5; exit
}')
if [ "$unrelated_state" != "0.0 dB:no" ]; then
    echo "$inputs" >&2
    echo "reused playback slot inherited controls from a different app: $unrelated_state" >&2
    exit 1
fi
kill "$unrelated_playback_pid"
wait "$unrelated_playback_pid" 2>/dev/null || true
for attempt in $(seq 1 50); do
    inputs=$("$binary" --list-in)
    if ! printf '%s\n' "$inputs" | grep -q 'TestAppB.*/ stream'; then break; fi
    sleep 0.1
done

"$binary" load show inactive-stream
pw-play --target default -P 'application.name=TestAppA application.id=org.shatter.test.appa' "$test_root/sine.wav" >/dev/null 2>&1 &
recreated_playback_pid=$!
playback_pids="$playback_pids $recreated_playback_pid"
inputs=$(wait_for_apps 1)
recreated_stream=$(printf '%s\n' "$inputs" | awk -F'|' -v channel="$app_a_id" 'index($0, channel " / stream") { print; exit }')
recreated_state=$(printf '%s\n' "$recreated_stream" | awk -F'|' '{
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", $4); gsub(/^[[:space:]]+|[[:space:]]+$/, "", $5);
    print $4 ":" $5
}')
if [ "$recreated_state" != "-7.0 dB:yes" ]; then
    echo "recreated playback stream did not restore its controls: $recreated_stream" >&2
    exit 1
fi
recreated_sends=$(printf '%s\n' "$recreated_stream" | awk -F'|' '{ gsub(/^[[:space:]]+|[[:space:]]+$/, "", $7); print $7 }')
if [ "$recreated_sends" != "speakers, Headphone Monitor" ]; then
    echo "recreated playback stream did not restore its output send: $recreated_stream" >&2
    exit 1
fi
recreated_stream_id=$(printf '%s\n' "$recreated_stream" | awk -F'|' '{ row=$6; gsub(/^[[:space:]]+/, "", row); sub(/[[:space:]].*$/, "", row); print row }')
"$binary" --set-gain stream "$recreated_stream_id" 0
"$binary" --set-mute stream "$recreated_stream_id" off
"$binary" unassign stream "$recreated_stream_id" "Headphone Monitor"
"$binary" delete out "Headphone Monitor"
output_listing=$("$binary" --list-out)
case "$output_listing" in
    *"Headphone Monitor"*) echo "deleted output channel remained in the output list: $output_listing" >&2; exit 1 ;;
    *) ;;
esac
kill "$recreated_playback_pid"
wait "$recreated_playback_pid" 2>/dev/null || true
for attempt in $(seq 1 50); do
    inputs=$("$binary" --list-in)
    if ! printf '%s\n' "$inputs" | grep -q "$recreated_stream_id"; then break; fi
    sleep 0.1
done

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
    index($0, channel " / stream") { row=$6; gsub(/^[[:space:]]+/, "", row); sub(/[[:space:]].*$/, "", row); print row }
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
