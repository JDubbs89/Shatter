#!/usr/bin/env python3
"""One Linux binary detects server changes and restarts without a user command."""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time


def main(binary):
    root = pathlib.Path(tempfile.mkdtemp(prefix="shatter-backend-restart-"))
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    config = root / "pipewire"
    config.mkdir()
    for name in ("pipewire.conf", "pipewire-pulse.conf", "client.conf"):
        shutil.copyfile(pathlib.Path("/usr/share/pipewire") / name, config / name)
    hidden = (config / "pipewire.conf").read_text()
    hidden = re.sub(r"core.name\s*=\s*pipewire-0", "core.name = shatter-pulse-core", hidden)
    (config / "hidden.conf").write_text(hidden)
    # Keep test servers away from the desktop session bus as well as its sockets.
    env = os.environ.copy()
    env.update(DBUS_SESSION_BUS_ADDRESS=f"unix:path={runtime}/absent-bus",
               XDG_RUNTIME_DIR=str(runtime), PIPEWIRE_RUNTIME_DIR=str(runtime),
               PIPEWIRE_REMOTE="pipewire-0", PIPEWIRE_CONFIG_DIR=str(config),
               PULSE_SERVER=f"unix:{runtime}/pulse/native", XDG_CONFIG_HOME=str(root / "settings"),
               XDG_STATE_HOME=str(root / "state"), XDG_CACHE_HOME=str(root / "cache"))
    processes = []
    logs = []
    daemon = None

    def command(args, client_env=env, check=True):
        result = subprocess.run(args, env=client_env, text=True, capture_output=True, timeout=10)
        if check and result.returncode:
            raise RuntimeError(f"{args}: {result.stderr or result.stdout}")
        return result.stdout

    def launch(args, log_name, client_env=env):
        output = open(root / log_name, "w")
        logs.append(output)
        process = subprocess.Popen(args, env=client_env, stdout=output, stderr=subprocess.STDOUT)
        processes.append(process)
        return process

    def stop(process):
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)

    def wait_for(predicate, description, timeout=18):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            if daemon is not None and daemon.poll() is not None:
                raise RuntimeError(f"daemon exited while waiting for {description}")
            time.sleep(0.15)
        raise RuntimeError(f"timed out waiting for {description}")

    def status_is(kind, ready=False):
        value = command([binary, "status"], check=False)
        return f"Backend: {kind}\n" in value and (not ready or "audio backend is ready." in value)

    def control_identity():
        try:
            info = (runtime / "shatter/control.sock").stat()
            return info.st_ino, info.st_ctime_ns
        except FileNotFoundError:
            return None

    def start_primary(cycle):
        core = launch(["pipewire", "-c", "pipewire.conf"], f"primary-{cycle}.log")
        wait_for(lambda: "cookie:" in command(["pw-cli", "info", "0"], check=False), "primary core")
        policy = launch(["wireplumber", "--profile", "policy"], f"policy-{cycle}.log")
        command(["pw-cli", "create-node", "adapter", '{ factory.name = support.null-audio-sink '
                 'node.name = shatter-restart-speakers node.description = "Restart Speakers" '
                 'media.class = Audio/Sink audio.position = [ FL FR ] object.linger = true }'])
        return core, policy

    try:
        daemon = launch([binary, "--engine"], "daemon.log")
        wait_for(lambda: status_is("none"), "no-server startup")

        hidden_env = env.copy()
        hidden_env["PIPEWIRE_REMOTE"] = "shatter-pulse-core"
        launch(["pipewire", "-c", "hidden.conf"], "hidden.log", hidden_env)
        wait_for(lambda: "cookie:" in command(["pw-cli", "info", "0"], hidden_env, False), "hidden core")
        command(["pw-cli", "create-node", "adapter", '{ factory.name = support.null-audio-sink '
                 'node.name = pulse-restart-speakers media.class = Audio/Sink '
                 'audio.position = [ FL FR ] object.linger = true }'], hidden_env)
        pulse = launch(["pipewire-pulse"], "pulse.log", hidden_env)
        wait_for(lambda: "Server Name:" in command(["pactl", "info"], check=False), "PulseAudio protocol")
        wait_for(lambda: status_is("pulseaudio", True), "automatic no-server to PulseAudio restart")

        primary, policy = start_primary(1)
        wait_for(lambda: status_is("pipewire", True), "automatic PulseAudio to PipeWire restart")
        command([binary, "make", "out", "Restart Monitor"])
        command([binary, "set", "gain", "output", "Restart Monitor", "-7"])

        stop(policy)
        stop(primary)
        wait_for(lambda: status_is("pulseaudio", True), "automatic PipeWire to PulseAudio restart")
        saved = root / "settings/shatter/default.conf"
        if not saved.is_file():
            raise RuntimeError("automatic restart did not autosave the PipeWire show")

        primary, policy = start_primary(2)
        wait_for(lambda: status_is("pipewire", True), "automatic return to PipeWire")
        if "-7.0 dB" not in command([binary, "list", "out"]):
            raise RuntimeError("returning to PipeWire did not restore the autosaved output gain\n" + command([binary, "list", "out"]) + "\nSaved: " + saved.read_text())

        previous = control_identity()
        stop(policy)
        stop(primary)
        primary, policy = start_primary(3)
        wait_for(lambda: control_identity() not in (None, previous) and status_is("pipewire", True),
                 "same-backend server-instance restart")
        if "-7.0 dB" not in command([binary, "list", "out"]):
            raise RuntimeError("same-backend restart lost the saved gain")

        stop(policy)
        stop(primary)
        stop(pulse)
        wait_for(lambda: status_is("none"), "automatic transition to no-server mode")
        command([binary, "stop"])
        daemon.wait(timeout=8)
        time.sleep(2)
        if "is stopped" not in command([binary, "status"], check=False):
            raise RuntimeError("explicit stop was undone by the backend watcher")
        if "called from wrong context" in (root / "daemon.log").read_text():
            raise RuntimeError("native teardown ran outside its PipeWire loop context")
        print("Automatic backend selection, daemon restarts, show recovery, and explicit stop passed.")
    except Exception:
        for output in logs:
            output.flush()
        for path in sorted(root.glob("*.log")):
            print(f"{path.name}:\n{path.read_text()[-10000:]}", file=sys.stderr)
        raise
    finally:
        try:
            command([binary, "stop"], check=False)
        except (OSError, subprocess.TimeoutExpired):
            pass
        for process in reversed(processes):
            stop(process)
        for output in logs:
            output.close()
        shutil.rmtree(root)


if __name__ == "__main__":
    main(str(pathlib.Path(sys.argv[1]).resolve()))
