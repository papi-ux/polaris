# Bounded log-tail API v1

Polaris exposes an authenticated, cache-disabled diagnostic log tail that never reads the whole active log. It is intended for the Web UI and diagnostic tooling that needs recent log content without making host or browser memory proportional to the log file's lifetime size.

```text
GET /polaris/v1/diagnostics/logs/tail
```

## Runtime retention

The active runtime log is fixed at 8388608 bytes, with one backup of at most the same size. Polaris rotates only between complete formatted records, replaces the prior backup, and advances the log generation before writing any bytes into the replacement active file. A prior active log that is already oversized is reduced to its newest 8388608 bytes during startup instead of being copied in full. An orphaned oversized backup is reduced the same way when no active log survived. The active and backup logs therefore have a 16777216-byte combined logical bound, apart from filesystem allocation granularity. The active log has a single owner per configuration directory: Polaris takes an exclusive advisory lock on a `.lock` sidecar next to the active file at startup, and a second Polaris process that cannot acquire it continues with console-only logging instead of inheriting, rotating, or truncating the owner's files.

## Request

All query values are strict unsigned decimal integers. Signs, whitespace, trailing characters, duplicate parameters, unknown parameters, zero limits, overflow, and values above the documented maximum return HTTP `400`.

| Parameter | Default | Maximum | Meaning |
|---|---:|---:|---|
| `max_bytes` | 262144 | 1048576 | Maximum source bytes read from the end of the active log. |
| `max_lines` | 2000 | 10000 | Maximum logical lines retained after applying the byte bound. |
| `after` | omitted | uintmax | Optional prior `end_offset` for an incremental response. Zero is valid. |
| `after_generation` | omitted | uint64 | Prior response generation. It must appear together with `after`. |

The route uses the same Web UI authentication and origin policy as the legacy log route.

## Response

Successful responses are JSON with `Cache-Control: no-store`:

```json
{
  "status": true,
  "schema_version": 1,
  "content_encoding": "base64",
  "content": "WzIwMjYtMDgtMTJdOiBJbmZvOiByZWFkeQo=",
  "content_bytes": 29,
  "media_type": "text/plain",
  "charset": "utf-8",
  "start_offset": 4096,
  "end_offset": 4125,
  "generation": 7,
  "truncated": true,
  "reset": true
}
```

Offsets describe the half-open source byte range `[start_offset, end_offset)`. `generation` identifies the active log lifecycle and changes after initialization, automatic rotation, or a successful clear. `content_bytes` is the decoded byte count, not the Base64 string length. Base64 is mandatory so embedded NULs, invalid text bytes, and platform code pages cannot make the JSON invalid or change the offset contract.

`truncated` is true when source bytes before `start_offset` were omitted by either the byte or line bound.

`reset` tells an incremental consumer how to apply the response:

- With no `after`, `reset` is true and the response replaces local state.
- When `after_generation` matches the active generation and `after` lies inside the available byte range, the response begins at `after` and `reset` is false; it is safe to append.
- When the generation differs, or `after` is older than the available range or newer than the current file end, `reset` is true. The generation check prevents a clear or rotation from reusing a numerically plausible offset and being mistaken for a contiguous delta.
- If the line limit removes part of an otherwise incremental delta, `reset` is true because appending would hide a gap.

Consumers must keep their own decoded state bounded even when appending contiguous deltas.

## Legacy compatibility

`GET /api/logs` remains authenticated and returns a plain-text body with the platform charset, but it is capped at the newest 1048576 bytes. It also returns:

- `X-Polaris-Log-Start-Offset`
- `X-Polaris-Log-End-Offset`
- `X-Polaris-Log-Truncated`
- `Cache-Control: no-store`

Existing clients can continue treating the body as text. New clients should use the v1 endpoint so truncation and reset behavior are part of the response schema.
