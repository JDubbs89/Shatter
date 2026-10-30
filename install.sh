#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
install_prefix=${SHATTER_INSTALL_PREFIX:-"$HOME/.local"}
build_dir=${SHATTER_BUILD_DIR:-"$project_dir/build"}

if [ "$(id -u)" -eq 0 ] && [ -z "${SHATTER_INSTALL_PREFIX:-}" ]; then
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

if ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists libpipewire-0.3; then
    echo "PipeWire development files are required. On Fedora, install them with: sudo dnf install pipewire-devel" >&2
    exit 1
fi

cmake -S "$project_dir" -B "$build_dir" -DCMAKE_INSTALL_PREFIX="$install_prefix"
cmake --build "$build_dir" --target shatter
cmake --install "$build_dir"

cat <<EOF
Installed shatter in $install_prefix/bin/shatter
Ensure $install_prefix/bin is in your PATH, then run: shatter help
EOF
