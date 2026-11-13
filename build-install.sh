#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
platform=${SHATTER_PLATFORM:-auto}
startup=${SHATTER_STARTUP:-skip}

usage()
{
    cat <<EOF
Usage: $0 [--platform auto|linux|windows] [--startup enable|disable|skip]

Defaults: auto selects the native platform. All platform audio adapters are included.
The daemon detects the running audio server automatically.
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
case "$platform" in auto|linux|windows) ;;
    *) echo "Unsupported platform '$platform'; choose auto, linux, or windows." >&2; exit 2 ;;
esac
export SHATTER_PLATFORM=$platform
export SHATTER_STARTUP=$startup

sh "$project_dir/install.sh"
