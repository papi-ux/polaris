"""Read-only observations for private multiseat smoke harnesses.

A running container or client process does not establish media delivery. This
module reports worker identity and state only; playback requires separate proof.
It performs no Docker calls, device access, process control or cleanup.
"""

from dataclasses import dataclass
import re


LABEL = "io.polaris.multiseat."
_WORKER_ID = re.compile(r"[0-9a-f]{64}\Z")
_IMAGE_ID = re.compile(r"sha256:[0-9a-f]{64}\Z")
_ROLE = re.compile(r"[a-z][a-z0-9-]{0,31}\Z")
_PROVIDER = re.compile(
    r"^(?:polaris-seat-(?:worker|launcher|encoder|display-capture|"
    r"nested-compositor|runtime|virtual-input|audio|session-bus):|"
    r"runtime provider failed:)"
)


class ObservationError(ValueError):
    """The inventory cannot establish which expected seat owns a worker."""


@dataclass(frozen=True)
class ExpectedSeat:
    role: str
    volume: str
    image: str


def observe_workers(records, deployment, seats):
    """Bind Docker inspect records to exact expected volumes and image IDs.

    An empty inventory means the seats are absent. Inventory collection errors
    must be handled by the caller as unavailable evidence, not an empty list.
    Unrecognized, duplicate or inconsistent workers reject the observation.
    """
    if not isinstance(deployment, str) or not deployment or not seats:
        raise ObservationError("test deployment and expected seats are required")
    expected = {}
    result = {}
    for seat in seats:
        if (not isinstance(seat, ExpectedSeat) or
                not isinstance(seat.role, str) or not _ROLE.fullmatch(seat.role) or
                not isinstance(seat.volume, str) or not seat.volume or
                not isinstance(seat.image, str) or not _IMAGE_ID.fullmatch(seat.image) or
                seat.volume in expected or seat.role in result):
            raise ObservationError("expected seat identities are ambiguous or invalid")
        expected[seat.volume] = seat
        result[seat.role] = {
            "worker_id": None, "started_at": None, "state": "absent", "running": False,
        }
    if not expected:
        raise ObservationError("test deployment and expected seats are required")
    if not isinstance(records, list):
        raise ObservationError("worker inventory is unavailable")
    identities = set()
    for record in records:
        try:
            identity = record["Id"]
            labels = record["Config"]["Labels"]
            seat = expected[labels[LABEL + "volume"]]
            state = record["State"]
            image = record["Image"]
            mounts = record["Mounts"]
            if labels[LABEL + "deployment"] != deployment:
                raise ObservationError("worker belongs to another test deployment")
            if (not isinstance(identity, str) or not _WORKER_ID.fullmatch(identity) or
                    identity in identities or result[seat.role]["worker_id"] is not None):
                raise ObservationError("worker identity is duplicate or ambiguous")
            if image != seat.image:
                raise ObservationError("worker image differs from the expected seat")
            homes = [mount for mount in mounts if mount["Destination"] == "/var/lib/polaris-seat"]
            if (len(homes) != 1 or homes[0]["Type"] != "volume" or
                    homes[0]["Name"] != seat.volume or homes[0]["RW"] is not True):
                raise ObservationError("worker profile mount differs from its seat label")
            if (state["Status"] not in {
                    "created", "running", "paused", "restarting", "removing", "exited", "dead",
                } or not isinstance(state["StartedAt"], str) or not state["StartedAt"] or
                    any(type(state[key]) is not bool for key in ("Running", "Paused", "Restarting"))):
                raise ObservationError("worker state is incomplete or invalid")
            identities.add(identity)
            result[seat.role] = {
                "worker_id": identity,
                "started_at": state["StartedAt"],
                "state": state["Status"],
                "running": state["Status"] == "running" and state["Running"] and
                           not state["Paused"] and not state["Restarting"],
            }
        except (KeyError, TypeError, AttributeError) as error:
            raise ObservationError("worker inventory is incomplete or contains an unexpected seat") from error
    return result


def seat_transitions(previous, current):
    """Report the seat that actually changed, without inferring an exit cause."""
    changes = []
    for role, after in current.items():
        before = (previous or {}).get(role)
        if before == after or (before is None and after["worker_id"] is None):
            continue
        if before is None or before["worker_id"] is None:
            event = "appeared"
        elif after["worker_id"] is None:
            event = "disappeared"
        elif before["worker_id"] != after["worker_id"]:
            event = "replaced"
        elif before["started_at"] != after["started_at"]:
            event = "restarted"
        else:
            event = "state_changed"
        changes.append({"seat": role, "event": event,
                        "previous": before, "current": after})
    return changes


def is_provider_diagnostic(line):
    """Recognize current and older provider prefixes, not ordinary game output.

    This is a component filter, not a redactor. Retained diagnostics are private.
    """
    return isinstance(line, str) and _PROVIDER.match(line) is not None
