#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT HUP INT TERM
mkdir -p "$test_dir/tools" "$test_dir/Program Files"
cat > "$test_dir/tools/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$SHATTER_TEST_SYSTEMCTL_LOG"
case "$*" in
    '--user is-enabled --quiet shatter.service') test -f "$SHATTER_TEST_ENABLED" ;;
    '--user enable shatter.service') touch "$SHATTER_TEST_ENABLED" ;;
    '--user disable --now shatter.service') rm -f "$SHATTER_TEST_ENABLED" ;;
esac
EOF
chmod +x "$test_dir/tools/systemctl"
binary="$test_dir/Program Files/shatter"
cp "$test_dir/tools/systemctl" "$binary"
export PATH="$test_dir/tools:$PATH"
export XDG_CONFIG_HOME="$test_dir/config"
export SHATTER_TEST_SYSTEMCTL_LOG="$test_dir/systemctl.log"
export SHATTER_TEST_ENABLED="$test_dir/enabled"
unit="$XDG_CONFIG_HOME/systemd/user/shatter.service"

sh "$project_dir/packaging/systemd/configure-startup.sh" enable "$binary"
test -f "$unit"
grep -F "ExecStart=\"$binary\" --engine" "$unit"
grep -F "ExecStop=\"$binary\" stop" "$unit"
grep -Fx -- '--user enable shatter.service' "$SHATTER_TEST_SYSTEMCTL_LOG"
cp "$unit" "$test_dir/original.service"
original_inode=$(ls -i "$unit" | awk '{print $1}')
: > "$SHATTER_TEST_SYSTEMCTL_LOG"
sh "$project_dir/packaging/systemd/configure-startup.sh" enable "$binary"
cmp "$unit" "$test_dir/original.service"
test "$(ls -i "$unit" | awk '{print $1}')" = "$original_inode"
if grep -E -- 'daemon-reload|--user enable shatter.service' "$SHATTER_TEST_SYSTEMCTL_LOG"; then
    echo 'Repeating startup configuration should leave an enabled, unchanged service alone.' >&2
    exit 1
fi
if grep -F -- '--now' "$SHATTER_TEST_SYSTEMCTL_LOG"; then
    echo 'Enabling startup should not start a second engine during installation.' >&2
    exit 1
fi
sh "$project_dir/packaging/systemd/configure-startup.sh" disable "$binary"
test ! -f "$unit"
grep -Fx -- '--user disable --now shatter.service' "$SHATTER_TEST_SYSTEMCTL_LOG"
sh "$project_dir/packaging/systemd/configure-startup.sh" disable "$binary"

# Exercise the install entry point without building or changing the real user session.
cat > "$test_dir/tools/cmake" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$test_dir/tools/cmake"
export SHATTER_INSTALL_PREFIX="$test_dir/Program Files"
export SHATTER_BUILD_DIR="$test_dir/build"
mkdir -p "$SHATTER_INSTALL_PREFIX/bin"
cp "$binary" "$SHATTER_INSTALL_PREFIX/bin/shatter"
sh "$project_dir/build-install.sh" --platform linux --startup enable
test -f "$unit"
sh "$project_dir/build-install.sh" --platform linux --startup skip
test -f "$unit"
sh "$project_dir/build-install.sh" --platform linux --startup disable
test ! -f "$unit"
if sh "$project_dir/build-install.sh" --platform windows --startup enable; then
    echo 'The shell installer accepted unsupported Windows startup configuration.' >&2
    exit 1
fi

if sh "$project_dir/packaging/systemd/configure-startup.sh" invalid "$binary"; then
    echo 'An invalid startup choice was accepted.' >&2
    exit 1
fi
echo 'Startup service install and removal checks passed.'
