import { describe, expect, it } from 'vitest'
import { normalizeTailscalePeer, safeTailscalePeerSnapshot, tailscalePeerSummary } from './tailscale-connection-readout.js'
import { buildAnonymizedDiagnosticsBundle, buildGithubIssueDraft } from './diagnostics-export.js'

const now = 1791446400000
const sample = () => ({ schema_version: 1, status: 'tailscale', stream_instance_id: 'process.stream.42',
  tailscale: { confirmed: true, transport: 'direct', scope: 'peer_status', source: 'tailscaled_localapi',
    observed_at_ms: now - 1000, max_age_ms: 10000, reason: 'observed' } })
const options = { now, generation: 'process.stream.42', streaming: true }

describe('Tailscale peer readout', () => {
  it('keeps direct, DERP, peer relay and unknown transport distinct without grading quality', () => {
    for (const transport of ['direct', 'derp', 'peer_relay', 'unknown']) {
      const value = sample(); value.tailscale.transport = transport
      const result = normalizeTailscalePeer(value, options)
      expect(result.confirmed).toBe(true)
      expect(result.transport).toBe(transport)
      expect(tailscalePeerSummary(result)).toContain('video/audio route unverified')
      expect(result).not.toHaveProperty('bitrate')
      expect(result).not.toHaveProperty('quality')
    }
  })

  it('expires observations, rejects future clocks, unknown schemas and different sessions', () => {
    const mutations = [
      v => { v.tailscale.observed_at_ms = now - 10001 },
      v => { v.tailscale.observed_at_ms = now + 1001 },
      v => { v.tailscale.observed_at_ms = 0 },
      v => { v.tailscale.observed_at_ms = '1791446400000' },
      v => { v.tailscale.max_age_ms = 999999 },
      v => { v.tailscale.source = 'client_guess' },
      v => { v.tailscale.scope = 'control_connection' },
      v => { v.schema_version = 2 },
      v => { v.stream_instance_id = 'process.stream.41' },
      v => { v.tailscale.transport = 'high-speed' },
      v => { v.tailscale.confirmed = 'true' },
    ]
    for (const mutate of mutations) {
      const value = sample(); mutate(value)
      expect(normalizeTailscalePeer(value, options).confirmed).toBe(false)
    }
    expect(normalizeTailscalePeer(sample(), { ...options, streaming: false }).reason).toBe('no_active_stream')
    expect(normalizeTailscalePeer(null, options).confirmed).toBe(false)
    expect(normalizeTailscalePeer(sample(), { ...options, now: now + 9000 }).confirmed).toBe(true)
  })

  it('does not promote address hints or a daemon error into a confirmed connection', () => {
    const value = { schema_version: 1, status: 'unknown', address_kind: 'tailscale', stream_instance_id: 'process.stream.42',
      tailscale: { confirmed: false, transport: 'direct', scope: 'peer_status', reason: 'secret tailnet example.ts.net' } }
    const result = normalizeTailscalePeer(value, options)
    expect(result.confirmed).toBe(false)
    expect(result.reason).toBe('peer_not_confirmed')
    expect(tailscalePeerSummary(result)).toBe('Unknown')
  })

  it('allows only the public vocabulary into support JSON and issue text', () => {
    const value = normalizeTailscalePeer(sample(), options)
    const poisoned = { ...value, node_key: 'sensitive-node-value', hostname: 'private-device.example.ts.net', endpoint: '100.64.0.2', inventory: ['hidden'] }
    const bundle = buildAnonymizedDiagnosticsBundle({ network_connection: poisoned })
    expect(bundle.network_connection).toEqual(safeTailscalePeerSnapshot(value))
    const serialized = JSON.stringify(bundle)
    for (const secret of ['sensitive-node-value', 'private-device', '100.64.0.2', 'hidden']) expect(serialized).not.toContain(secret)
    expect(buildGithubIssueDraft({ network_connection: poisoned })).toContain('Direct (matched Tailscale peer; video/audio route unverified)')
  })
})
