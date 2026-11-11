# Start Shatter when you sign in

Choose `--startup enable` when running `build-install.sh` to configure automatic startup for your user. You need a running systemd user session. Installation enables the service for your next sign-in; it does not start a second engine while you are installing.

```sh
sh build-install.sh --audio-backend pipewire --startup enable
```

To start it immediately, stop any engine you started manually, then start the service:

```sh
shatter stop
systemctl --user start shatter.service
systemctl --user status shatter.service
```

The service runs the installed executable and writes logs to the user journal. Read them with `journalctl --user -u shatter.service`. It does not restart automatically after a crash. Normal `shatter` commands work while the service is running.

Use `--startup disable` on a later installation to stop and remove the service. Omit the option, or use `--startup skip`, to preserve your existing startup settings.

Repeating `--startup enable` with the same installation keeps the existing unit and enabled state. If you change the executable location, it updates the unit to use the new path. Repeating `--startup disable` also succeeds after the service has been removed.

Linux binary archives also include `share/shatter/configure-startup.sh`. After extracting to `~/.local`, run:

```sh
sh "$HOME/.local/share/shatter/configure-startup.sh" enable "$HOME/.local/bin/shatter"
```

Use absolute paths if you extract elsewhere. Run the same helper with `disable` before removing or moving the installation. The generated unit is stored under `$XDG_CONFIG_HOME/systemd/user/shatter.service`, or `~/.config/systemd/user/shatter.service` when `XDG_CONFIG_HOME` is unset.
