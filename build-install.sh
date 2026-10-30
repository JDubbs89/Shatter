#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${SHATTER_BUILD_DIR:-"$project_dir/build"}

cmake -S "$project_dir" -B "$build_dir"
cmake --build "$build_dir"
sh "$project_dir/install.sh"
