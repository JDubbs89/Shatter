#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
install_prefix=${SHATTER_INSTALL_PREFIX:-"$HOME/.local"}
build_dir=${SHATTER_BUILD_DIR:-"$project_dir/build"}

cmake -S "$project_dir" -B "$build_dir" -DCMAKE_INSTALL_PREFIX="$install_prefix"
cmake --build "$build_dir" --target shatter
cmake --install "$build_dir"

cat <<EOF
Installed shatter in $install_prefix/bin/shatter
Ensure $install_prefix/bin is in your PATH, then run: shatter help
EOF
