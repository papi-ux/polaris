# Paired client profile admission

The controller can now reserve a seat from an immutable profile route. The
authenticated launch supplies its retained paired client UUID and an already
validated display mode. The operator catalog supplies the persistent profile,
workload, runtime image family and GPU candidates. A device name, launch preset,
application path or client supplied profile identifier cannot select a route.

This is a controller API. There is no HTTP handler, saved settings format,
pairing migration or production activation callsite for it yet.

## Trusted configuration

`production_controller_options_t::profile_routes` binds each profile key to
zero or more paired client UUIDs and one image-owned workload selector. The
factory resolves the runtime family from the exact entry in
`container.profiles`, requires the workload in `container.workloads`, and derives
GPU preference order from the admitted GPU catalog. Each candidate must already
be compatible with the selected runtime image. This layer does not probe or
rank encoder performance.

Several devices may share one profile. A client may occur in only one route,
and duplicate profile keys or clients are rejected before dependencies are
created. An empty client list leaves an unassigned profile. Routes remain
immutable for the controller epoch; editing assignments during a running seat
is not exposed. No worker receives these routing keys in its resource names.

Routed admission requires authenticated worker media. Metadata-only worker
selection cannot enable this path. Invalid routes, unresolved profiles,
unknown workloads and runtime family mismatches fail before host factories.

## Admission and refusal

`admit_authenticated_profile_launch` reads the paired identity retained in the
pending `launch_session_t`. Existing authentication assigns this identity from
the verified certificate record. Its caller must pass that retained object,
not reconstruct it from HTTP parameters. The API rejects cancelled or invalid
launch generations, view-only launches, input-only attachments, temporary
authorization and missing launch permission for a routed device.

Reservation tries compatible GPUs in catalog order under one registry lock.
Every candidate is validated before trying any reservation. A full GPU may be
skipped, but the search never expands beyond the catalog. Shared profile and
client exclusion apply across all GPUs, including reserved and stopping seats.

| Result | Meaning |
| --- | --- |
| `unselected` | No routes, or this paired client has no route; continue ordinary host launch |
| `invalid_launch` | The selected launch lacks valid lifecycle or launch authority |
| `controller_not_ready` | Reconciliation has not completed, or shutdown has begun |
| `admitted` | The caller owns the returned exact reservation |
| `rejected` + `profile_already_active` | Another client already occupies this profile |
| `rejected` + `client_already_active` | This client already occupies a seat |
| `rejected` + `seat_capacity_reached` | Every candidate GPU has its seats occupied |
| `rejected` + `encoder_capacity_reached` | At least one GPU has a free seat, but none can reserve the requested encoder budget |

Invalid display requests and invalid GPU catalogs remain explicit admission
errors. These typed results are ready for an HTTP refusal mapping; no new wire
fields are claimed here.

As soon as a device matches a route, its launch retains the worker connection
requirement. Admission failure cannot silently select shared host capture.
`use_host_launch()` is true only for `unselected`. A later stream start also
rejects a selected launch without its authenticated worker connection.

## Lifecycle and the ordinary path

Admission only reserves. The caller must bind the compositor, prepare input,
start and authenticate the worker, and select the same retained launch through
the existing controller methods. On cancellation or subsequent failure, it
must stop the exact returned handle and reconcile before admitting again.
Reservation release does not imply that a live worker can be removed without
the existing ownership and cleanup checks.

An enabled production factory with no profiles and no routes returns disabled
before GPU validation or invoking a host dependency. Controllers used by the
explicit physical harness may still use typed seat requests with a profile
catalog and no routes. Routing with no entries, or with an unmapped client,
leaves that launch's media requirement unchanged.

Remaining integration work includes persistent profile provisioning, pairing
assignments, public refusal fields, one owner for HTTP/RTSP cancellation and
seat teardown, concrete launcher adapters, and client playback acceptance.
The single-user path and production activation remain unchanged.

## Verification

Offline tests cover conflicting routes before dependency creation, catalog
resolution, fallback between full GPUs, distinct capacity refusals, concurrent
claims of one profile, reservation release, permission and lifecycle failures,
and a selected busy-profile launch reaching stream startup without invoking
host capture. These tests do not execute a launcher, container engine or GPU.
