#!/bin/sh
set -eu

startup=${1:-}
binary=${2:-}
case "$startup" in enable|disable) ;;
    *) echo "Usage: $0 enable|disable /absolute/path/to/shatter" >&2; exit 2 ;;
esac
case "$binary" in
    /*) ;;
    *) echo "The Shatter executable path must be absolute." >&2; exit 2 ;;
esac
systemctl --user show-environment >/dev/null
unit_dir=${XDG_CONFIG_HOME:-"$HOME/.config"}/systemd/user
unit_path="$unit_dir/shatter.service"

if [ "$startup" = disable ]; then
    if [ -e "$unit_path" ]; then
        systemctl --user disable --now shatter.service
        rm -f -- "$unit_path"
        systemctl --user daemon-reload
    fi
    echo "Shatter automatic startup disabled."
    exit 0
fi

[ -x "$binary" ] || { echo "Shatter executable not found: $binary" >&2; exit 1; }
# Escape systemd specifiers and quoted command arguments for custom prefixes.
escaped_binary=$(printf '%s' "$binary" | sed 's/\\/\\\\/g; s/"/\\"/g; s/%/%%/g; s/\$/$$/g')
mkdir -p -- "$unit_dir"
temporary_unit=$(mktemp "$unit_dir/.shatter.service.XXXXXX")
trap 'rm -f -- "$temporary_unit"' EXIT HUP INT TERM
cat > "$temporary_unit" <<EOF
[Unit]
Description=Shatter desktop audio mixer
After=pipewire.service pulseaudio.service

[Service]
Type=simple
ExecStart="$escaped_binary" --engine
ExecStop="$escaped_binary" stop
Restart=no

[Install]
WantedBy=default.target
EOF
if ! cmp -s "$temporary_unit" "$unit_path"; then
    chmod 644 "$temporary_unit"
    mv -f -- "$temporary_unit" "$unit_path"
    systemctl --user daemon-reload
fi
if ! systemctl --user is-enabled --quiet shatter.service; then
    systemctl --user enable shatter.service
fi
echo "Shatter automatic startup enabled for your next sign-in."
