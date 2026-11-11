#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
platform=${SHATTER_PLATFORM:-auto}
audio_backend=${SHATTER_AUDIO_BACKEND:-auto}
startup=${SHATTER_STARTUP:-skip}

usage()
{
    cat <<EOF
Usage: $0 [--platform auto|linux|windows] [--audio-backend auto|pipewire|pulseaudio|wasapi|none] [--startup enable|disable|skip]

Defaults: auto selects the native platform and that platform's default backend.
Implemented audio backends: pipewire and pulseaudio (Linux), wasapi (Windows, discovery prototype), and none.
Startup: enable or disable the Linux systemd user service; skip leaves startup unchanged (default).
On Windows, choose startup in the MSI installer.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --platform)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            platform=$2
            shift 2
            ;;
        --platform=*) platform=${1#*=}; shift ;;
        --audio-backend|--backend)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            audio_backend=$2
            shift 2
            ;;
        --audio-backend=*|--backend=*) audio_backend=${1#*=}; shift ;;
        --startup)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            startup=$2
            shift 2
            ;;
        --startup=*) startup=${1#*=}; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

platform=$(printf '%s' "$platform" | tr '[:upper:]' '[:lower:]')
audio_backend=$(printf '%s' "$audio_backend" | tr '[:upper:]' '[:lower:]')
case "$platform" in auto|linux|windows) ;;
    *) echo "Unsupported platform '$platform'; choose auto, linux, or windows." >&2; exit 2 ;;
esac
case "$audio_backend" in auto|pipewire|pulseaudio|wasapi|none) ;;
    *) echo "Unsupported audio backend '$audio_backend'; choose auto, pipewire, pulseaudio, wasapi, or none." >&2; exit 2 ;;
esac

if [ "$audio_backend" = pulseaudio ]; then
    if [ "$platform" = windows ]; then
        echo "The PulseAudio backend is available only on Linux." >&2
        exit 2
    fi
fi
if [ "$audio_backend" = wasapi ] && [ "$platform" != windows ]; then
    echo "The WASAPI backend is available only on Windows." >&2
    exit 2
fi

export SHATTER_PLATFORM=$platform
export SHATTER_AUDIO_BACKEND=$audio_backend
export SHATTER_STARTUP=$startup

sh "$project_dir/install.sh"
