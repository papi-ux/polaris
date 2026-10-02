import { describe, expect, it } from 'vitest'
import { REDACTED_VALUE, buildAnonymizedDiagnosticsBundle } from './diagnostics-export.js'

// The host names each stream in its live stats with stream_instance_id: a value drawn once per
// process, a dot, and the session generation. It is not called session_id because the export
// redacts that exact name as a Web UI credential. These run the exporter as it is, so a rename on
// either side that would blank the identity, or let a credential through, fails here.
describe('stream identity in a support export', () => {
  const capture = {
    preference: 'kms',
    requested: 'portal',
    opened: 'portal',
    route: 'portal_screencast',
    mode_override_reason: 'gamescope_session',
    route_fallback_reason: 'gamescope_node_missing',
    transport: 'unknown',
    residency: 'unknown',
    format: 'unknown',
  }

  it('keeps each stream_instance_id and its capture readable while a session_id stays redacted', () => {
    const bundle = buildAnonymizedDiagnosticsBundle({
      stats: {
        clients: [
          { name: 'Deck', stream_instance_id: '9f3c2a7b1d4e6f80.12', session_id: 'web-ui-bearer-value', capture },
          { name: 'Phone', stream_instance_id: '9f3c2a7b1d4e6f80.13', capture },
        ],
        capture_backend_opened: 'portal',
        capture_route_fallback_reason: 'gamescope_node_missing',
      },
    })

    const [deck, phone] = bundle.stats.clients
    expect(deck.stream_instance_id).toBe('9f3c2a7b1d4e6f80.12')
    expect(phone.stream_instance_id).toBe('9f3c2a7b1d4e6f80.13')
    expect(deck.session_id).toBe(REDACTED_VALUE)
    expect(deck.capture).toEqual(capture)
    expect(bundle.stats.capture_backend_opened).toBe('portal')
    expect(bundle.stats.capture_route_fallback_reason).toBe('gamescope_node_missing')
    expect(JSON.stringify(bundle)).not.toContain('web-ui-bearer-value')
  })

  // last_session is what the host keeps once a stream ends, and the one a support bundle exported
  // after disconnecting carries. A report about one stream matches it by stream_instance_id.
  it('keeps the last session\'s identity, times and capture readable while a session_id beside it stays redacted', () => {
    const lastSession = {
      state: 'ended',
      stream_instance_id: '9f3c2a7b1d4e6f80.11',
      client_name: 'Deck',
      started_at: '2026-09-27T10:00:00Z',
      ended_at: '2026-09-27T10:42:13Z',
      capture,
      codec: 'hevc',
      encoder_backend: 'vaapi',
    }
    const bundle = buildAnonymizedDiagnosticsBundle({
      stats: { clients: [], last_session: { ...lastSession, session_id: 'web-ui-bearer-value' } },
    })

    const kept = bundle.stats.last_session
    expect(kept.state).toBe('ended')
    expect(kept.stream_instance_id).toBe('9f3c2a7b1d4e6f80.11')
    expect(kept.started_at).toBe('2026-09-27T10:00:00Z')
    expect(kept.ended_at).toBe('2026-09-27T10:42:13Z')
    expect(kept.capture).toEqual(capture)
    expect(kept.codec).toBe('hevc')
    expect(kept.encoder_backend).toBe('vaapi')
    expect(kept.session_id).toBe(REDACTED_VALUE)
    expect(JSON.stringify(bundle)).not.toContain('web-ui-bearer-value')
  })
})
