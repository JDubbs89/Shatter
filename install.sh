#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
platform=${SHATTER_PLATFORM:-AUTO}
audio_backend=${SHATTER_AUDIO_BACKEND:-AUTO}
platform_lower=$(printf '%s' "$platform" | tr '[:upper:]' '[:lower:]')
backend_lower=$(printf '%s' "$audio_backend" | tr '[:upper:]' '[:lower:]')
if [ "$platform_lower" = auto ]; then
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) platform_lower=windows ;;
        *) platform_lower=linux ;;
    esac
fi
if [ "$backend_lower" = auto ]; then
    if [ "$platform_lower" = linux ]; then backend_lower=pipewire; else backend_lower=none; fi
fi
case "$platform_lower" in
    windows) default_prefix=${LOCALAPPDATA:-"$HOME/AppData/Local"}/Programs/Shatter ;;
    *) default_prefix="$HOME/.local" ;;
esac
install_prefix=${SHATTER_INSTALL_PREFIX:-$default_prefix}
build_dir=${SHATTER_BUILD_DIR:-"$project_dir/build"}
startup=${SHATTER_STARTUP:-skip}
case "$startup" in enable|disable|skip) ;;
    *) echo "Unsupported startup choice '$startup'; choose enable, disable, or skip." >&2; exit 2 ;;
esac
if [ "$startup" != skip ]; then
    if [ "$platform_lower" != linux ]; then
        echo "Choose automatic startup in the Windows MSI installer." >&2
        exit 2
    fi
    if ! command -v systemctl >/dev/null 2>&1 || ! systemctl --user show-environment >/dev/null 2>&1; then
        echo "Startup configuration requires a running systemd user session." >&2
        exit 1
    fi
fi

if [ "$(id -u)" -eq 0 ] && [ -z "${SHATTER_INSTALL_PREFIX:-}" ] && [ "$platform_lower" != windows ]; then
    echo "Run this script as your regular user to install in that user's ~/.local/bin; do not use sudo." >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ] && [ -e "$build_dir" ]; then
    if [ ! -w "$build_dir" ]; then
        echo "The build directory is not writable by this user: $build_dir" >&2
        echo "Remove that generated build directory or set SHATTER_BUILD_DIR to a writable path, then rerun this script." >&2
        exit 1
    fi

    for generated_file in CMakeCache.txt install_manifest.txt; do
        generated_path="$build_dir/$generated_file"
        if [ -e "$generated_path" ] && [ ! -w "$generated_path" ]; then
            if [ "$generated_file" = install_manifest.txt ] && [ -w "$build_dir" ]; then
                # CMake replaces this generated file during installation. Remove
                # an old root-owned manifest so the normal install can recreate it.
                rm -f -- "$generated_path"
                continue
            fi
            echo "A generated build file is not writable by this user: $generated_path" >&2
            echo "Remove that generated build directory or set SHATTER_BUILD_DIR to a writable path, then rerun this script." >&2
            exit 1
        fi
    done
fi

if [ "$backend_lower" = pipewire ] &&
   { ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists libpipewire-0.3; }; then
    echo "PipeWire development files are required. On Fedora, install them with: sudo dnf install pipewire-devel" >&2
    exit 1
fi
if [ "$backend_lower" = pulseaudio ] &&
   { ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists libpulse; }; then
    echo "PulseAudio development files are required. On Fedora, install them with: sudo dnf install pulseaudio-libs-devel" >&2
    exit 1
fi

cmake -S "$project_dir" -B "$build_dir" \
    -DBUILD_TESTING=OFF \
    -DSHATTER_PLATFORM="$platform" -DSHATTER_AUDIO_BACKEND="$audio_backend" \
    -DCMAKE_INSTALL_PREFIX="$install_prefix"
cmake --build "$build_dir" --target shatter --config Release
cmake --install "$build_dir" --config Release

if [ "$platform_lower" = windows ]; then installed_binary="$install_prefix/bin/shatter.exe"; else installed_binary="$install_prefix/bin/shatter"; fi
if [ "$startup" != skip ]; then
    sh "$project_dir/packaging/systemd/configure-startup.sh" "$startup" "$installed_binary"
fi
cat <<EOF
Installed shatter in $installed_binary
Ensure $install_prefix/bin is in your PATH, then run: shatter help
EOF
