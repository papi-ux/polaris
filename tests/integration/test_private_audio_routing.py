#!/usr/bin/env python3
"""Exercise Linux repinning against device-free, temporary PipeWire servers.

Usage: python3 tests/integration/test_private_audio_routing.py \
    --binary build/native/tests/test_polaris_audio --evidence build/audio-routing
Requires PipeWire, pipewire-pulse, WirePlumber, pw-cat, pacat and pactl.
The evidence directory must be new. Logs may contain local process metadata.
"""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time


class Interruptions:
    def __init__(self):
        self.deferring = False
        self.pending = None

    def __call__(self, number, _frame):
        if self.deferring:
            self.pending = number
        else:
            raise RuntimeError(f"Private fixture interrupted by signal {number}")

    @contextmanager
    def defer(self):
        self.deferring = True
        try:
            yield
        finally:
            self.deferring = False
            if self.pending is not None:
                self(self.pending, None)


@contextmanager
def private_runtime(receipt, evidence):
    directory = tempfile.TemporaryDirectory(prefix="polaris-audio629-", dir="/tmp")
    root = Path(directory.name)
    interruptions = Interruptions()
    handlers = {number: signal.signal(number, interruptions)
                for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    try:
        yield directory.name, interruptions
    finally:
        try:
            directory.cleanup()
        except Exception as error:
            receipt["cleanup_errors"].append(f"runtime cleanup: {error}")
        receipt["runtime_removed"] = not root.exists()
        try:
            (evidence / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
        finally:
            for number, handler in handlers.items():
                signal.signal(number, handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--interrupt-after-ready", choices=("SIGTERM", "SIGHUP"),
                        help="Exercise cleanup after real producers start; intentionally exits nonzero")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    evidence = args.evidence.resolve()
    evidence.mkdir(mode=0o700, parents=True, exist_ok=False)
    executables = {name: shutil.which(name) for name in
                   ("pipewire", "pipewire-pulse", "wireplumber", "pw-cat", "pacat", "pactl")}
    if not all(executables.values()):
        raise RuntimeError("Missing private audio fixture dependency")
    children, logs = [], []
    receipt = {"routing_passed": False, "host_service_changes": False, "cleanup_errors": []}
    with private_runtime(receipt, evidence) as (tmp, interruptions):
        root = Path(tmp)
        os.chmod(root, 0o700)
        env = os.environ.copy()
        for key in tuple(env):
            if key.startswith(("PULSE_", "PIPEWIRE_", "POLARIS_SESSION_", "WIREPLUMBER_")):
                env.pop(key)
        config = root / "config"
        wp_config = config / "wireplumber/wireplumber.conf.d"
        wp_config.mkdir(parents=True)
        pw_config = root / "pwconf"
        pw_config.mkdir()
        for name in ("pipewire.conf", "pipewire-pulse.conf", "client.conf"):
            shutil.copyfile(Path("/usr/share/pipewire") / name, pw_config / name)
        (wp_config / "99-isolated.conf").write_text("""wireplumber.profiles = {
          polaris-test = {
            inherits = [ base, mixin.systemwide-session, mixin.stateless ]
            metadata.sm-settings = required
            policy.standard = required
            hardware.audio = disabled
            hardware.bluetooth = disabled
            hardware.video-capture = disabled
            support.dbus = disabled
          }
        }
        """)
        env.update(XDG_RUNTIME_DIR=tmp, PIPEWIRE_RUNTIME_DIR=tmp,
                   PULSE_RUNTIME_PATH=tmp + "/pulse", PULSE_SERVER="unix:" + tmp + "/pulse/native",
                   XDG_CONFIG_HOME=str(config), XDG_CONFIG_DIRS=str(root / "etc"),
                   XDG_STATE_HOME=str(root / "state"), XDG_CACHE_HOME=str(root / "cache"),
                   PIPEWIRE_CONFIG_DIR=str(pw_config),
                   DBUS_SESSION_BUS_ADDRESS="unix:path=" + tmp + "/no-bus",
                   DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + tmp + "/no-system-bus",
                   POLARIS_TEST_PRIVATE_AUDIO_ROUTING="1", POLARIS_STREAM_SINK="0")
        isolation = ("{ support.dbus=false module.jackdbus-detect=false module.portal=false "
                     "module.x11.bell=false module.raop=false }")

        def spawn(name, command, child_env, stdin=None):
            log = (evidence / (name + ".log")).open("w")
            logs.append(log)
            # Register the child before a Python signal handler may unwind.
            # The child keeps normal signal dispositions and masks.
            with interruptions.defer():
                process = subprocess.Popen(command, env=child_env, stdin=stdin, stdout=log,
                                           stderr=subprocess.STDOUT, start_new_session=True)
                children.append(process)
            return process

        def command(arguments):
            return subprocess.check_output(arguments, env=env, text=True,
                                           stderr=subprocess.PIPE, timeout=5)

        def pactl(*arguments):
            return command([executables["pactl"], *arguments])

        def wait(check, message):
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if check():
                    return
                time.sleep(.05)
            raise RuntimeError(message)

        def inputs():
            return json.loads(pactl("-f", "json", "list", "sink-inputs"))

        def snapshot():
            return {"streams": inputs(),
                    "clients": json.loads(pactl("-f", "json", "list", "clients")),
                    "default": pactl("get-default-sink").strip()}

        def route(name):
            process = spawn(name, [str(binary), "--gtest_filter=AudioProcessRouting.PrivatePipeWireFixture"], env)
            if process.wait(timeout=20) != 0:
                raise RuntimeError("Native routing bridge failed")

        marked = {"polaris-routing-session", "polaris-routing-legacy"}

        def assert_routes(snap, sinks, routed):
            assert snap["default"] == "polaris-routing-host", "Default sink changed"
            assert len(snap["streams"]) == 5, "Unexpected stream count"
            assert {stream["properties"]["application.name"] for stream in snap["streams"]} == {
                "polaris-routing-session", "polaris-routing-desktop", "polaris-routing-other",
                "polaris-routing-legacy", "polaris-routing-pulse-desktop"
            }, "Unexpected producers"
            for stream in snap["streams"]:
                name = stream["properties"]["application.name"]
                destination = "polaris-routing-session" if routed and name in marked else "polaris-routing-host"
                assert stream["sink"] == sinks[destination], f"Wrong destination for {name}"

        try:
            spawn("pipewire", [executables["pipewire"], "-c", "pipewire.conf", "-P", isolation], env)
            wait(lambda: (root / "pipewire-0").is_socket(), "Private PipeWire did not start")
            spawn("wireplumber", [executables["wireplumber"], "--profile=polaris-test"], env)
            spawn("pulse", [executables["pipewire-pulse"], "-c", "pipewire-pulse.conf", "-P", isolation], env)
            wait(lambda: (root / "pulse/native").is_socket(), "Private Pulse did not start")
            for sink in ("polaris-routing-host", "polaris-routing-session"):
                pactl("load-module", "module-null-sink", "sink_name=" + sink, "channels=2")
            pactl("set-default-sink", "polaris-routing-host")
            sinks = {sink["name"]: sink["index"] for sink in json.loads(pactl("-f", "json", "list", "sinks"))}
            assert set(sinks) == {"polaris-routing-host", "polaris-routing-session"}, "Unexpected sink"
            assert json.loads(pactl("-f", "json", "list", "cards")) == [], "Private server discovered hardware"
            zero = open("/dev/zero", "rb")
            logs.append(zero)
            native_session = None
            for name, marker in (("session", "polaris-routing-session"), ("desktop", None), ("other", "another-session")):
                app_env = env.copy()
                if marker:
                    app_env["POLARIS_SESSION_AUDIO_SINK"] = marker
                process = spawn(name, [executables["pw-cat"], "--playback", "--raw", "--format", "s16",
                                       "--rate", "48000", "--channels", "2", "--properties",
                                       "{ application.name=polaris-routing-" + name + " }", "-"], app_env, zero)
                if name == "session":
                    native_session = process
            pulse_env = dict(env, POLARIS_SESSION_AUDIO_SINK="polaris-routing-session")
            for name, app_env in (("legacy", pulse_env), ("pulse-desktop", env)):
                spawn(name, [executables["pacat"], "--playback", "--raw", "--rate=48000", "--channels=2",
                             "--client-name=polaris-routing-" + name], app_env, zero)
            wait(lambda: len(inputs()) == 5, "Private producers did not start")
            (evidence / "producer-metadata.json").write_text(json.dumps(snapshot(), indent=2) + "\n")
            before = snapshot()
            assert_routes(before, sinks, False)
            clients = {client["index"]: client["properties"] for client in before["clients"]}
            for stream in before["streams"]:
                name = stream["properties"]["application.name"]
                if name in {"polaris-routing-session", "polaris-routing-desktop", "polaris-routing-other"}:
                    assert "application.process.id" not in stream["properties"], "Fixture has an explicit stream PID"
                    client = clients[int(stream["client"])]
                    assert int(client["application.process.id"]) > 1
                if name == "polaris-routing-legacy":
                    client = clients[int(stream["client"])]
                    assert client["client.api"] == "pipewire-pulse"
                    assert client["application.process.id"] != client["pipewire.sec.pid"], "Fixture does not distinguish proxy PID"
                    assert stream["properties"]["application.process.id"] == client["application.process.id"]
            (evidence / "before.json").write_text(json.dumps(before, indent=2) + "\n")
            if args.interrupt_after_ready:
                os.kill(os.getpid(), getattr(signal, args.interrupt_after_ready))
            route("route-first")
            after = snapshot()
            assert_routes(after, sinks, True)
            (evidence / "after.json").write_text(json.dumps(after, indent=2) + "\n")
            # Force a session stream back to the private desktop sink and prove
            # a later control instance repins without changing other streams.
            for stream in after["streams"]:
                if stream["properties"]["application.name"] == "polaris-routing-session":
                    pactl("move-sink-input", str(stream["index"]), "polaris-routing-host")
            route("route-repin")
            assert_routes(snapshot(), sinks, True)
            # Retire a native client and create a replacement; per-pass client
            # ownership must follow the new server object, not an old cache.
            native_session.terminate()
            native_session.wait(timeout=3)
            wait(lambda: len(inputs()) == 4, "Retired stream did not disappear")
            spawn("session-reconnect", [executables["pw-cat"], "--playback", "--raw", "--format", "s16",
                  "--rate", "48000", "--channels", "2", "--properties",
                  "{ application.name=polaris-routing-session }", "-"], pulse_env, zero)
            wait(lambda: len(inputs()) == 5, "Replacement stream did not appear")
            route("route-reconnect")
            assert_routes(snapshot(), sinks, True)
            receipt["routing_passed"] = True
            receipt["native_missing_pid"] = True
            receipt["legacy_explicit_pid"] = True
            receipt["unmarked_and_other_session_unchanged"] = True
            receipt["default_sink_unchanged"] = True
            receipt["repin_and_reconnect"] = True
        finally:
            # A second signal must not abandon the remaining owned children.
            for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                signal.signal(number, signal.SIG_IGN)
            for process in reversed(children):
                try:
                    if process.poll() is None:
                        process.terminate()
                except Exception as error:
                    receipt["cleanup_errors"].append(f"terminate {process.pid}: {error}")
            for process in reversed(children):
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    try:
                        process.kill()
                        process.wait(timeout=3)
                    except Exception as error:
                        receipt["cleanup_errors"].append(f"kill/reap {process.pid}: {error}")
                except Exception as error:
                    receipt["cleanup_errors"].append(f"reap {process.pid}: {error}")
            for log in logs:
                try:
                    log.close()
                except Exception as error:
                    receipt["cleanup_errors"].append(f"close log: {error}")
            receipt["all_children_exited"] = all(process.poll() is not None for process in children)
    assert receipt["all_children_exited"] and receipt["runtime_removed"] and not receipt["cleanup_errors"], "Private fixture cleanup failed"
    print(json.dumps(receipt))


if __name__ == "__main__":
    main()
