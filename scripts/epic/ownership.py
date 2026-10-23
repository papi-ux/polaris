"""Non-secret process attribution for the official Epic Linux Preview.

The activation URL is not a process handle. Wine reparents the title and its
prefix server, so neither the activation process nor its process group can be
used as the target of End. This module selects a coherent process candidate;
it neither sends signals nor proves which concurrent request caused a launch.
The runtime controller must establish activation ownership separately, then
retain and revalidate pidfds before implementing End.

Only the observed Windows/Proton route is described here. Discovery, native
titles, launcher startup and stream-mode admission belong to their callers.
"""

from dataclasses import dataclass
from pathlib import PurePosixPath
import re


class AttributionError(ValueError):
    """The activation cannot safely claim this title."""


def _absolute(path):
    value = PurePosixPath(path)
    if not value.is_absolute() or ".." in value.parts:
        raise AttributionError("Expected a resolved absolute path")
    return value


@dataclass(frozen=True)
class Title:
    namespace: str
    item_id: str
    artifact_id: str
    install_dir: PurePosixPath
    launch_executable: str
    data_root: PurePosixPath

    def __post_init__(self):
        for value in (self.namespace, self.item_id, self.artifact_id):
            if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,256}", value):
                raise AttributionError("Invalid Epic title identity")
        object.__setattr__(self, "install_dir", _absolute(self.install_dir))
        object.__setattr__(self, "data_root", _absolute(self.data_root))
        relative = PurePosixPath(self.launch_executable)
        if (not self.launch_executable or relative.is_absolute()
                or ".." in relative.parts or "\\" in self.launch_executable
                or "\x00" in self.launch_executable or relative == PurePosixPath(".")):
            raise AttributionError("Expected an installation-relative executable")

    @property
    def uri(self):
        return ("com.epicgames.launcher://apps/"
                + "%3A".join((self.namespace, self.item_id, self.artifact_id))
                + "?action=launch&silent=true")

    @property
    def image(self):
        return self.install_dir / self.launch_executable

    @property
    def prefix(self):
        return self.data_root / "compat/compatdata" / self.artifact_id / "pfx"

    @property
    def runtime_base(self):
        return self.data_root / "compat/runtime"


@dataclass(frozen=True)
class Image:
    path: PurePosixPath
    device_major: int
    device_minor: int
    inode: int

    def __post_init__(self):
        object.__setattr__(self, "path", _absolute(self.path))
        if self.device_major < 0 or self.device_minor < 0 or self.inode <= 0:
            raise AttributionError("Invalid mapped-image identity")


@dataclass(frozen=True)
class Prefix:
    device: int
    inode: int

    def server_dir(self, uid):
        if self.device < 0 or self.inode <= 0 or uid < 0:
            raise AttributionError("Invalid Wine prefix identity")
        return PurePosixPath(f"/tmp/.wine-{uid}/server-{self.device:x}-{self.inode:x}")


@dataclass(frozen=True)
class Process:
    pid: int
    start_ticks: int
    parent_pid: int
    uid: int
    executable: PurePosixPath
    cwd: PurePosixPath
    cgroup: str
    images: tuple = ()

    def __post_init__(self):
        if self.pid <= 0 or self.start_ticks <= 0 or self.parent_pid < 0 or self.uid < 0:
            raise AttributionError("Invalid process identity")
        object.__setattr__(self, "executable", _absolute(self.executable))
        object.__setattr__(self, "cwd", _absolute(self.cwd))
        if not self.cgroup.startswith("/") or ".." in PurePosixPath(self.cgroup).parts:
            raise AttributionError("Missing process cgroup identity")

    @property
    def identity(self):
        return self.pid, self.start_ticks


@dataclass(frozen=True)
class Snapshot:
    boot_id: str
    captured_ticks: int
    processes: tuple
    complete: bool = True

    def __post_init__(self):
        if not self.boot_id or self.captured_ticks <= 0:
            raise AttributionError("Missing process snapshot clock")
        if len({p.pid for p in self.processes}) != len(self.processes):
            raise AttributionError("Ambiguous process snapshot")
        if any(p.start_ticks > self.captured_ticks for p in self.processes):
            raise AttributionError("Process starts after its snapshot")


@dataclass(frozen=True)
class LaunchCandidate:
    boot_id: str
    title: Title
    image: Image
    prefix: Prefix
    launcher: Process
    wrapper: Process
    wine_server: Process
    game: Process


def _one(processes, description):
    if len(processes) != 1:
        raise AttributionError(f"Expected one {description}; found {len(processes)}")
    return processes[0]


def _runtime(title, process, binary):
    try:
        parts = process.executable.relative_to(title.runtime_base).parts
    except ValueError:
        return None
    if len(parts) != 4 or parts[1:] != ("files", "bin", binary):
        return None
    return title.runtime_base / parts[0]


def _inside(path, parent):
    return path == parent or parent in path.parents


def _under_cgroup(process, cgroup):
    return process.cgroup == cgroup or process.cgroup.startswith(cgroup.rstrip("/") + "/")


def match_activation(title, image, prefix, before, after, launcher_identity,
                     request_start_ticks, polaris_cgroup):
    """Match an activation, refusing existing, incomplete or ambiguous evidence.

    A match alone never authorizes End: another launcher client or the owner's
    UI could request this same title concurrently. The runtime controller must
    refuse ownership when it cannot distinguish that case.

    Image device numbers come from the process's mount identity, not blindly
    from stat().st_dev: Btrfs uses different devices for its mapped superblock
    and a subvolume. The kernel reader must verify the path/inode, process-root
    view and map_files link when it constructs Image records.
    """
    if not before.complete or not after.complete:
        raise AttributionError("Incomplete process observation")
    if before.boot_id != after.boot_id:
        raise AttributionError("Host restarted during activation")
    if not before.captured_ticks <= request_start_ticks <= after.captured_ticks:
        raise AttributionError("Activation is outside the observation interval")
    if image.path != title.image:
        raise AttributionError("Mapped image belongs to another title")
    if not polaris_cgroup.startswith("/") or polaris_cgroup == "/":
        raise AttributionError("Missing Polaris service cgroup")
    old_launcher = _one([p for p in before.processes if p.identity == launcher_identity], "existing launcher")
    launcher = _one([p for p in after.processes if p == old_launcher], "unchanged launcher")
    if launcher.executable.name != "EpicGamesLauncher":
        raise AttributionError("Unexpected launcher executable")
    if _under_cgroup(launcher, polaris_cgroup):
        raise AttributionError("Epic must outlive the Polaris service")
    server_dir = prefix.server_dir(launcher.uid)

    for process in before.processes:
        if process.uid != launcher.uid:
            continue
        runtime = (_runtime(title, process, "wineserver")
                   or _runtime(title, process, "wine64-preloader"))
        if (image in process.images
                or (runtime and (process.cwd == server_dir
                                 or _inside(process.cwd, title.prefix)
                                 or process.cwd == title.install_dir))
                or (process.parent_pid == launcher.pid and process.cwd == title.install_dir)):
            raise AttributionError("The title or its runtime was already running")

    previous = {p.identity for p in before.processes}
    fresh = [p for p in after.processes
             if p.uid == launcher.uid and p.identity not in previous
             and request_start_ticks <= p.start_ticks <= after.captured_ticks]
    wrapper = _one([p for p in fresh
                    if p.parent_pid == launcher.pid and p.cwd == title.install_dir
                    and re.fullmatch(r"python3(?:\.\d+)?", p.executable.name)], "launcher child")
    wine_server = _one([p for p in fresh
                        if p.cwd == server_dir and _runtime(title, p, "wineserver")], "prefix server")
    runtime = _runtime(title, wine_server, "wineserver")
    game = _one([p for p in fresh
                 if p.cwd == title.install_dir and image in p.images
                 and _runtime(title, p, "wine64-preloader") == runtime], "title process")
    for process in (wrapper, wine_server, game):
        if _under_cgroup(process, polaris_cgroup):
            raise AttributionError("Epic runtime shares the Polaris service lifetime")
    return LaunchCandidate(before.boot_id, title, image, prefix, launcher, wrapper, wine_server, game)


def revalidate(binding, snapshot):
    """Never replace a retained claim with another launch of the same title."""
    if not snapshot.complete or snapshot.boot_id != binding.boot_id:
        raise AttributionError("The retained launch can no longer be verified")
    by_identity = {p.identity: p for p in snapshot.processes}
    for process in (binding.launcher, binding.wrapper, binding.wine_server, binding.game):
        if by_identity.get(process.identity) != process:
            raise AttributionError("The retained launch changed or ended")
    return binding
