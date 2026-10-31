#!/usr/bin/env python3
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import wave


def command(args, *, env=None, check=True):
    result = subprocess.run(args, text=True, capture_output=True, env=env)
    if check and result.returncode != 0:
        raise RuntimeError(f"command failed ({args[0]}): {result.stderr.strip() or result.stdout.strip()}")
    return result


def main(binary):
    if not shutil.which("pactl") or not shutil.which("paplay"):
        print("PulseAudio test tools are unavailable; skipping.")
        return 77
    info = command(["pactl", "info"], check=False)
    if info.returncode != 0:
        print("No PulseAudio-compatible server is available; skipping.")
        return 77

    server_line = next((line for line in info.stdout.splitlines() if line.startswith("Server String:")), "")
    server = server_line.partition(":")[2].strip()
    env = os.environ.copy()
    if server and "PULSE_SERVER" not in env:
        env["PULSE_SERVER"] = server if server.startswith(("unix:", "tcp:", "tcp4:", "tcp6:")) else f"unix:{server}"

    temp_dir = pathlib.Path(tempfile.mkdtemp(prefix="shatter-pulse-test-"))
    runtime_dir = temp_dir / "runtime"
    runtime_dir.mkdir(mode=0o700)
    env["XDG_RUNTIME_DIR"] = str(runtime_dir)
    env["XDG_CONFIG_HOME"] = str(temp_dir / "config")

    sink_name = f"shatter_test_{os.getpid()}"
    app_name = "ShatterTicket05Probe"
    module_id = None
    player = None
    try:
        loaded = command(["pactl", "load-module", "module-null-sink", f"sink_name={sink_name}",
                          f"sink_properties=device.description={app_name}Sink"], env=env, check=False)
        if loaded.returncode != 0:
            print("PulseAudio-compatible server does not provide module-null-sink; skipping.")
            return 77
        module_id = loaded.stdout.strip()

        audio_path = temp_dir / "probe.wav"
        sample_rate = 44100
        with wave.open(str(audio_path), "wb") as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(sample_rate)
            audio.writeframes(b"\0\0" * sample_rate * 30)

        player = subprocess.Popen(["paplay", "--client-name", app_name, str(audio_path)], env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        started = command([binary, "start"], env=env)
        if started.returncode != 0:
            raise RuntimeError(started.stderr.strip() or started.stdout.strip() or "Shatter failed to start")

        health = command([binary, "health"], env=env).stdout
        if ("Engine health: running" not in health or "Xruns since engine start" not in health or
                "I/O latency:" not in health or "pactl list sinks" not in health):
            raise RuntimeError(f"PulseAudio health report is incomplete:\n{health}")

        inputs = outputs = ""
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            inputs_result = command([binary, "list", "in"], env=env, check=False)
            outputs_result = command([binary, "list", "out"], env=env, check=False)
            inputs, outputs = inputs_result.stdout, outputs_result.stdout
            if app_name in inputs and sink_name in outputs:
                break
            time.sleep(0.1)
        if app_name not in inputs:
            raise RuntimeError(f"PulseAudio app is missing from Shatter inputs:\n{inputs}")
        if sink_name not in outputs:
            raise RuntimeError(f"test sink is missing from Shatter outputs:\n{outputs}")

        routed = command([binary, "assign", "app", app_name, sink_name], env=env)
        if routed.returncode != 0:
            raise RuntimeError(routed.stderr.strip() or routed.stdout.strip() or "Shatter routing command failed")

        sink_ids = command(["pactl", "list", "short", "sinks"], env=env).stdout.splitlines()
        target_id = next((line.split("\t", 2)[0] for line in sink_ids
                          if len(line.split("\t")) > 1 and line.split("\t")[1] == sink_name), None)
        if target_id is None:
            raise RuntimeError("PulseAudio test sink disappeared")

        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            details = command(["pactl", "list", "sink-inputs"], env=env).stdout
            current_sink = None
            matched_sink = None
            matched_app = False
            for line in details.splitlines():
                header = re.match(r"Sink Input #(\d+)", line)
                if header:
                    if matched_app:
                        matched_sink = current_sink
                    matched_app = False
                    current_sink = None
                elif line.strip().startswith("Sink:"):
                    current_sink = line.split(":", 1)[1].strip()
                elif f'application.name = "{app_name}"' in line:
                    matched_app = True
            if matched_app:
                matched_sink = current_sink
            if matched_sink == target_id:
                print("PulseAudio app playback routed to the selected sink.")
                return 0
            time.sleep(0.1)
        raise RuntimeError(f"PulseAudio did not move {app_name} to sink {sink_name}")
    finally:
        command([binary, "stop"], env=env, check=False)
        if player is not None:
            player.terminate()
            try:
                player.wait(timeout=2)
            except subprocess.TimeoutExpired:
                player.kill()
                player.wait()
        if module_id:
            command(["pactl", "unload-module", module_id], env=env, check=False)
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1]))
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)
