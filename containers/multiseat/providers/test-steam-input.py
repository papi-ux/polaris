#!/usr/bin/env python3
"""Run the real preload library under both ABIs without any input device."""
import os
import pathlib
import re
import socket
import struct
import subprocess
import sys
import tempfile

if len(sys.argv) < 3 or len(sys.argv) % 2 != 1:
    raise SystemExit("usage: test-steam-input.py CLIENT LIBRARY [CLIENT LIBRARY ...]")
for client, library in zip(sys.argv[1::2], sys.argv[2::2]):
    versions = subprocess.check_output(["readelf", "--version-info", library], text=True)
    required = {tuple(map(int, version.split("."))) for version in re.findall(r"GLIBC_([0-9.]+)", versions)}
    assert required and max(required) <= (2, 15), ("requires newer Steam runtime libc", required)
    dynamic = subprocess.check_output(["readelf", "-d", library], text=True)
    assert "[libdl.so.2]" in dynamic and "[libpthread.so.0]" in dynamic, "missing older runtime dependencies"
    for scenario in ("name-valid", "name-generation", "name-id"):
        env = dict(os.environ, LD_PRELOAD=str(pathlib.Path(library).resolve()) + ":" +
                   str(pathlib.Path(client + ".evdev.so").resolve()),
                   POLARIS_STEAM_INPUT_SOCKET="/unused-test-socket",
                   POLARIS_STEAM_INPUT_NAME="Polaris multiseat 0123456789abcdef0123456789abcdef steam-gamepad-0")
        subprocess.run([client, scenario], env=env, check=True, timeout=10)
        print(pathlib.Path(client).name, scenario, "passed", flush=True)
    for scenario in ("modern", "legacy", "destroy"):
        with tempfile.TemporaryDirectory(prefix="polaris-steam-") as directory:
            path = str(pathlib.Path(directory) / "input.sock")
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as listener:
                listener.bind(path)
                listener.listen(1)
                listener.settimeout(10)
                env = dict(os.environ, LD_PRELOAD=str(pathlib.Path(library).resolve()),
                           POLARIS_STEAM_INPUT_SOCKET=path,
                           POLARIS_STEAM_INPUT_SYSNAME="input123")
                process = subprocess.Popen([client, scenario], env=env,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    peer, _ = listener.accept()
                    with peer:
                        peer.settimeout(10)
                        first = peer.recv(4096)
                        second = peer.recv(4096)
                        eof = peer.recv(4096)
                    expected = struct.pack("<4sIHBBhhhhbbBB", b"PSI1", 1, 1, 255, 128,
                                           32767, -32768, 0, -1, 1, -1, 0, 0)
                    neutral = struct.pack("<4sIHBBhhhhbbBB", b"PSI1", 2, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 0)
                    assert first == expected, (scenario, "translated report differs", first.hex())
                    assert second == neutral, (scenario, "neutral report differs", second.hex())
                    assert not eof, (scenario, "extra packet or leaked descriptor")
                    stdout, stderr = process.communicate(timeout=10)
                    assert process.returncode == 0, (process.returncode, stdout, stderr)
                    assert not stderr, stderr
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.communicate()
            print(pathlib.Path(client).name, scenario, "passed", flush=True)
