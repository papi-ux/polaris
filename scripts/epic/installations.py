"""Conservative, read-only decoding of Epic Preview's installed-state log.

This pure reader accepts bytes supplied by a separate, bounded filesystem
reader. It does not open files, recover or rewrite Epic's log, import library
entries, or authorize a launch. A candidate still needs filesystem, runtime,
stream-mode and per-launch ownership admission.

The observed v1 file is an append-only transaction log, not an unordered list
of completed installs. We deliberately refuse damaged or ambiguous snapshots
instead of recovering an older complete record. Header.entries is only a hint:
its relationship to appended lines is not established by the current schema.
"""

from dataclasses import dataclass, replace
import json
from pathlib import PurePosixPath

from .ownership import AttributionError, Title


MAX_BYTES = 16 * 1024 * 1024
MAX_LINES = 50_000
MAX_LINE_BYTES = 64 * 1024
MAX_TEXT = 4096


class ManifestError(ValueError):
    """The supplied snapshot cannot safely resolve installed state."""


@dataclass(frozen=True)
class Installation:
    installation_id: str
    status: str
    reason: str
    title: Title | None = None
    version: str | None = None


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ManifestError("Duplicate JSON key")
        result[key] = value
    return result


def _nonfinite(_value):
    raise ManifestError("Non-finite JSON number")


def _text(value, max_length=MAX_TEXT):
    return (isinstance(value, str) and 0 < len(value) <= max_length
            and value.isprintable())


def _absolute(value):
    if not _text(value) or value.startswith("//"):
        raise ValueError("Invalid installation path")
    path = PurePosixPath(value)
    if not path.is_absolute() or ".." in path.parts or path == PurePosixPath("/"):
        raise ValueError("Invalid installation path")
    return path


def _row(installation_id, record, data_root):
    """Decode the current raw record only; invalid records still supersede old ones."""
    try:
        directory = _absolute(record.get("dir"))
        platform = record.get("platform")
        if not _text(platform):
            return Installation(installation_id, "invalid", "missing-platform")
        if platform != "Windows":
            return Installation(installation_id, "unsupported", "unsupported-platform")
        title = Title(record.get("namespace"), record.get("itemId"),
                      record.get("artifactId"), directory,
                      record.get("launchExecutable"), data_root)
        # Title paths must not smuggle a control character through a log or shell.
        if not _text(record.get("launchExecutable")):
            return Installation(installation_id, "invalid", "invalid-executable")
        if type(record.get("complete")) is not bool:
            return Installation(installation_id, "invalid", "missing-completion-state")
        if not record["complete"] or record.get("pendingVersion") is not None:
            return Installation(installation_id, "updating", "installing-or-updating")
        if not _text(record.get("version")):
            return Installation(installation_id, "invalid", "missing-version")
        # The observed title has no custom launch command. Do not pretend a
        # different command's target is the declared executable we would bind.
        if record.get("launchCommand") not in (None, ""):
            return Installation(installation_id, "unsupported", "custom-launch-command")
        return Installation(installation_id, "candidate", "complete-metadata", title,
                            record["version"])
    except (AttributionError, TypeError, ValueError):
        return Installation(installation_id, "invalid", "invalid-title-metadata")


def parse_installations(raw, data_root):
    """Resolve a bounded v1 log; return one current result per surviving install.

    Normal records use installationId; observed tombstone framing uses uid and
    deleted=true. Conflicting or unkeyable framing refuses the entire snapshot.
    Last raw record wins before validation, so a malformed newer installation
    cannot resurrect an older completed installation. This conservative rule
    does not claim to reproduce Epic's recovery/window-settlement machinery.
    """
    if not isinstance(raw, bytes) or not raw or len(raw) > MAX_BYTES:
        raise ManifestError("Missing or oversized installed-state snapshot")
    try:
        root = _absolute(data_root)
    except (TypeError, ValueError):
        raise ManifestError("Invalid Epic data root") from None
    lines = raw.split(b"\n")
    if lines[-1] == b"":
        lines.pop()
    if len(lines) > MAX_LINES + 1:
        raise ManifestError("Too many installed-state entries")
    records = []
    for line in lines:
        if not line or len(line) > MAX_LINE_BYTES:
            raise ManifestError("Empty or oversized installed-state line")
        try:
            record = json.loads(line.decode("utf-8"), object_pairs_hook=_object,
                                parse_constant=_nonfinite)
        except (UnicodeError, json.JSONDecodeError, RecursionError):
            raise ManifestError("Malformed installed-state JSON") from None
        if not isinstance(record, dict):
            raise ManifestError("Installed-state line must be an object")
        records.append(record)

    header = records[0]
    if type(header.get("version")) is not int or header["version"] != 1:
        raise ManifestError("Unsupported installed-state version")
    if type(header.get("entries")) is not int or header["entries"] < 0:
        raise ManifestError("Invalid installed-state header")
    current = {}
    for record in records[1:]:
        deleted = record.get("deleted", False)
        if type(deleted) is not bool:
            raise ManifestError("Unknown installed-state discriminant")
        if deleted:
            key = record.get("uid")
            if not _text(key, 256) or ("installationId" in record and record["installationId"] != key):
                raise ManifestError("Ambiguous installed-state tombstone")
            current.pop(key, None)
        else:
            key = record.get("installationId")
            if not _text(key, 256) or ("uid" in record and record["uid"] != key):
                raise ManifestError("Unkeyable installed-state record")
            current[key] = record

    resolved = {key: _row(key, record, root) for key, record in current.items()}
    # Installation identity and directory are separate. Two current installs
    # claiming one title or directory require an explicit decision, not a merge.
    app_ids = {}
    directories = {}
    for key, record in current.items():
        ids = tuple(record.get(field) for field in ("namespace", "itemId", "artifactId"))
        if all(_text(value, 256) for value in ids):
            app_ids.setdefault(ids, set()).add(key)
        try:
            directories.setdefault(_absolute(record.get("dir")), set()).add(key)
        except (TypeError, ValueError):
            pass
    conflicts = set()
    for keys in (*app_ids.values(), *directories.values()):
        if len(keys) > 1:
            conflicts.update(keys)
    for key in conflicts:
        resolved[key] = replace(resolved[key], status="conflict", reason="ambiguous-installation",
                                title=None, version=None)
    return tuple(resolved[key] for key in sorted(resolved))
