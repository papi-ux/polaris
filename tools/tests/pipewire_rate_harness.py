#!/usr/bin/env python3
"""Measure the native capture consumer against isolated synthetic PipeWire video.

Runs no host audio daemon, portal request, display change, or game. Requires the
native platform test binary, PipeWire CLI, and GStreamer videotestsrc/pipewiresink.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

CONFIG = """
context.properties = { core.daemon = true core.name = pipewire-0 }
context.spa-libs = { support.* = support/libspa-support }
context.modules = [
 { name = libpipewire-module-protocol-native }
 { name = libpipewire-module-client-node }
 { name = libpipewire-module-access }
 { name = libpipewire-module-spa-node-factory }
 { name = libpipewire-module-adapter }
 { name = libpipewire-module-link-factory }
 { name = libpipewire-module-metadata }
]
context.objects = [
 { factory = spa-node-factory args = {
   factory.name = support.node.driver node.name = Dummy-Driver
   node.group = pipewire.dummy priority.driver = 20000
 } }
]
"""


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    binary = args.test_binary.resolve(strict=True)
    args.evidence.mkdir(parents=True, exist_ok=False)
    results = []
    with tempfile.TemporaryDirectory(prefix="polaris-pw-rate-") as directory:
        root = Path(directory)
        (root / "isolated.conf").write_text(CONFIG)
        environment = {**os.environ, "XDG_RUNTIME_DIR": directory, "PIPEWIRE_RUNTIME_DIR": directory,
                       "PIPEWIRE_REMOTE": "pipewire-0", "PIPEWIRE_CONFIG_DIR": directory}
        with (args.evidence / "daemon.log").open("w") as log:
            daemon = subprocess.Popen(["pipewire", "-c", "isolated.conf"], env=environment, stdout=log, stderr=subprocess.STDOUT)
            try:
                for _ in range(100):
                    if (root / "pipewire-0").is_socket():
                        break
                    if daemon.poll() is not None:
                        raise RuntimeError("Isolated PipeWire daemon exited")
                    time.sleep(0.05)
                else:
                    raise RuntimeError("Isolated PipeWire socket did not appear")
                # Clients must use their regular client configuration while
                # connecting only to the newly created private runtime socket.
                clients = dict(environment)
                clients.pop("PIPEWIRE_CONFIG_DIR")
                for label, producer_rate, requested in [
                    ("60-from-60", "60/1", (60, 1)),
                    ("60-from-120", "120/1", (60, 1)),
                    ("ntsc", "60000/1001", (60000, 1001)),
                ]:
                    with (args.evidence / (label + "-producer.log")).open("w") as producer_log:
                        producer = subprocess.Popen([
                            "gst-launch-1.0", "-q", "videotestsrc", "is-live=true", "!",
                            f"video/x-raw,format=BGRx,width=64,height=64,framerate={producer_rate}", "!",
                            "pipewiresink", "mode=provide", "stream-properties=props,node.name=polaris-rate-producer,media.class=Video/Source",
                        ], env=clients, stdout=producer_log, stderr=subprocess.STDOUT)
                        try:
                            node = None
                            for _ in range(100):
                                graph = json.loads(subprocess.check_output(["pw-dump"], env=clients, timeout=2))
                                nodes = [entry for entry in graph if entry.get("type") == "PipeWire:Interface:Node" and
                                         entry.get("info", {}).get("props", {}).get("node.name") == "polaris-rate-producer"]
                                if len(nodes) == 1:
                                    node = nodes[0]["id"]
                                    break
                                if producer.poll() is not None:
                                    raise RuntimeError("Synthetic producer exited")
                                time.sleep(0.05)
                            if node is None:
                                raise RuntimeError("Synthetic producer node did not appear")
                            case = {"node": node, "numerator": requested[0], "denominator": requested[1]}
                            test_environment = {**clients, "POLARIS_TEST_PIPEWIRE_CASE": json.dumps(case)}
                            capture_log_path = args.evidence / (label + "-capture.log")
                            with capture_log_path.open("w") as capture_log:
                                consumer = subprocess.Popen([
                                    str(binary), "--gtest_filter=PipeWireLiveProducerTests.*"
                                ], env=test_environment, stdout=capture_log, stderr=subprocess.STDOUT)
                                try:
                                    # Link only the two named test nodes in this
                                    # private graph; no desktop session manager
                                    # or host device monitor is started.
                                    for _ in range(80):
                                        graph = json.loads(subprocess.check_output(["pw-dump"], env=clients, timeout=2))
                                        consumers = [entry["id"] for entry in graph if entry.get("type") == "PipeWire:Interface:Node" and
                                                     (entry.get("info", {}).get("props", {}).get("media.name") == "polaris-portal-capture" or
                                                      str(entry.get("info", {}).get("props", {}).get("application.process.id")) == str(consumer.pid))]
                                        outputs, inputs = [], []
                                        for entry in graph:
                                            if entry.get("type") != "PipeWire:Interface:Port":
                                                continue
                                            props = entry.get("info", {}).get("props", {})
                                            if props.get("node.id") == node and props.get("port.direction") == "out":
                                                outputs.append(entry["id"])
                                            if props.get("node.id") in consumers and props.get("port.direction") == "in":
                                                inputs.append(entry["id"])
                                        if len(outputs) == len(inputs) == 1:
                                            subprocess.run(["pw-link", "-L", str(outputs[0]), str(inputs[0])], env=clients, check=True, timeout=2)
                                            break
                                        if consumer.poll() is not None:
                                            raise RuntimeError("Capture exited before its test link was ready")
                                        time.sleep(0.05)
                                    else:
                                        (args.evidence / (label + "-graph.json")).write_text(json.dumps(graph, indent=2))
                                        raise RuntimeError("Test-only capture ports did not appear")
                                    if consumer.wait(timeout=25) != 0:
                                        raise RuntimeError("Native capture test failed; inspect its retained log")
                                finally:
                                    stop(consumer)
                            measured = [line.removeprefix("PIPEWIRE_RATE_RESULT ") for line in capture_log_path.read_text().splitlines()
                                        if line.startswith("PIPEWIRE_RATE_RESULT ")]
                            if len(measured) != 1:
                                raise RuntimeError("Missing measured capture result")
                            results.append({"case": label, "producer_rate": producer_rate, **json.loads(measured[0])})
                        finally:
                            stop(producer)
            finally:
                stop(daemon)
    (args.evidence / "results.json").write_text(json.dumps({"results": results, "temporary_runtime_removed": True}, indent=2) + "\n")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
