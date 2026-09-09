#!/usr/bin/env python3
"""Run opt-in NVIDIA capture acceptance in an owned, private labwc session.

Requires a CUDA-enabled native video test binary, labwc, wlr-randr and vkcube.
Starts no streaming server or input authority. Logs and private configuration
are retained; this is capture/encoding evidence, not game-streaming acceptance.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time


def alive(pin):
    try:
        signal.pidfd_send_signal(pin, 0)
        return True
    except ProcessLookupError:
        return False


def children_of(pid):
    children = set()
    for task in Path(f"/proc/{pid}/task").iterdir():
        try:
            children.update(int(value) for value in (task / "children").read_text().split())
        except OSError:
            continue
    return children


def observe_children(pins, self_pin):
    pending = [os.getpid(), *pins]
    seen = set()
    while pending:
        pid = pending.pop()
        parent_pin = self_pin if pid == os.getpid() else pins[pid]
        if pid in seen or not alive(parent_pin):
            continue
        seen.add(pid)
        try:
            children = children_of(pid)
        except OSError:
            continue
        for value in children:
            child = int(value)
            if child not in pins:
                try:
                    pin = os.pidfd_open(child)
                except OSError:
                    continue
                try:
                    status = Path(f"/proc/{child}/status").read_text().splitlines()
                    parent = next(int(line.split()[1]) for line in status if line.startswith("PPid:"))
                    # Both pinned processes must still be alive around the
                    # parentage observation; PID reuse cannot authorize a kill.
                    if parent != pid or not alive(pin) or not alive(parent_pin):
                        os.close(pin)
                        continue
                except (OSError, StopIteration):
                    os.close(pin)
                    continue
                pins[child] = pin
            pending.append(child)


def reap_adopted():
    while True:
        try:
            if os.waitpid(-1, os.WNOHANG)[0] == 0:
                return
        except ChildProcessError:
            return


def terminate_signal(number, _frame):
    raise SystemExit(128 + number)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--render-device", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    binary = args.test_binary.resolve(strict=True)
    render = args.render_device.resolve(strict=True)
    if not render.is_char_device() or not render.name.startswith("renderD"):
        parser.error("render-device must resolve to a DRM render character device")
    # Process-local adoption catches short-lived/reparented descendants even
    # when they disappear between observations. No host service is changed.
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), "Cannot establish child subreaper")
    for number in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, terminate_signal)
    args.evidence.mkdir(mode=0o700, parents=True, exist_ok=False)
    root = Path(tempfile.mkdtemp(prefix="polaris-video-proof-"))
    runtime, home = root / "runtime", root / "home"
    runtime.mkdir(mode=0o700)
    home.mkdir(mode=0o700)
    receipt = {"restored": False, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
               "harness_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               "render_device": str(render), "private_directory": str(root)}
    environment = {**os.environ, "POLARIS_TEST_PRIVATE_VIDEO": "1", "POLARIS_TEST_RENDER_DEVICE": str(render),
                   "XDG_RUNTIME_DIR": str(runtime), "HOME": str(home), "XDG_CONFIG_HOME": str(home / "config"),
                   "XDG_CACHE_HOME": str(home / "cache"), "XDG_DATA_HOME": str(home / "data")}
    for name in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "LD_PRELOAD", "LD_LIBRARY_PATH"):
        environment.pop(name, None)
    pins = {}
    process = None
    self_pin = os.pidfd_open(os.getpid())
    try:
        with (args.evidence / "tests.log").open("w") as log:
            process = subprocess.Popen([str(binary), "--gtest_filter=VideoHardwareLifecycleTests.*"],
                                       env=environment, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            pins[process.pid] = os.pidfd_open(process.pid)
            deadline = time.monotonic() + 180
            while process.poll() is None and time.monotonic() < deadline:
                observe_children(pins, self_pin)
                time.sleep(0.1)
            receipt["exit_code"] = process.poll()
            receipt["timeout"] = process.poll() is None
    finally:
        # Complete this bounded cleanup even if a second termination arrives.
        signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM, signal.SIGHUP})
        survivors = set()
        errors = []
        if process is not None and process.poll() is None:
            # Popen still owns this unreaped direct child even if pidfd_open
            # failed. Its PID cannot be reused before it is reaped.
            survivors.add(process.pid)
            try:
                process.kill()
                process.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired) as error:
                errors.append(str(error))
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                observe_children(pins, self_pin)
                for pid, pin in pins.items():
                    if alive(pin):
                        survivors.add(pid)
                        try:
                            signal.pidfd_send_signal(pin, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                reap_adopted()
                if not children_of(os.getpid()):
                    break
            except OSError as error:
                errors.append(str(error))
            time.sleep(0.05)
        remaining = sorted(children_of(os.getpid()))
        for pin in pins.values():
            os.close(pin)
        os.close(self_pin)
        receipt["forced_cleanup_pids"] = sorted(survivors)
        receipt["remaining_children"] = remaining
        receipt["cleanup_errors"] = errors
        receipt["runtime_entries"] = [entry.name for entry in runtime.iterdir()]
        receipt["restored"] = not survivors and not remaining and not errors and not receipt["runtime_entries"]
        (args.evidence / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    log = (args.evidence / "tests.log").read_text()
    results = [line for line in log.splitlines() if line.startswith("VIDEO_LIFECYCLE_RESULT ")]
    (args.evidence / "measurements.txt").write_text("\n".join(results) + "\n")
    if receipt.get("exit_code") != 0 or not receipt["restored"] or len(results) != 9:
        raise SystemExit("Physical video acceptance failed; inspect retained evidence")
    print("Nine private capture cycles passed; owned processes and runtime resources are gone.")


if __name__ == "__main__":
    main()
