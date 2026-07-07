import { describe, expect, it } from 'vitest'
import {
  REDACTED_VALUE,
  buildAnonymizedDiagnosticsBundle,
  buildFixMyStreamChecklist,
  describeLinuxGpuProfile,
  redactSensitiveText,
  sanitizeDiagnosticsValue,
} from './diagnostics-export.js'

describe('diagnostics export redaction', () => {
  it('redacts sensitive field names recursively before export', () => {
    const bundle = buildAnonymizedDiagnosticsBundle({
      username: 'player-one',
      password: 'super-secret',
      nested: {
        api_token: 'tok_123',
        capture_path: 'dmabuf',
      },
      history: [
        { cookie: 'session=abc', bitrate_kbps: 45000 },
      ],
    })

    expect(bundle.username).toBe('player-one')
    expect(bundle.password).toBe(REDACTED_VALUE)
    expect(bundle.nested.api_token).toBe(REDACTED_VALUE)
    expect(bundle.nested.capture_path).toBe('dmabuf')
    expect(bundle.history[0].cookie).toBe(REDACTED_VALUE)
    expect(JSON.stringify(bundle)).not.toContain('super-secret')
    expect(JSON.stringify(bundle)).not.toContain('tok_123')
  })

  it('redacts sensitive assignments and auth headers from log text', () => {
    const redacted = redactSensitiveText([
      'Warning: auth failed token=abc123 password="hunter2"',
      'Authorization: Bearer ey.secret.value',
      'Cookie: sessionid=abc123',
      'Info: packet_loss=2.5 capture_path=dmabuf',
    ].join('\n'))

    expect(redacted).toContain(`token=${REDACTED_VALUE}`)
    expect(redacted).toContain(`password=${REDACTED_VALUE}`)
    expect(redacted).toContain(`Bearer ${REDACTED_VALUE}`)
    expect(redacted).toContain(`Cookie: ${REDACTED_VALUE}`)
    expect(redacted).toContain('packet_loss=2.5')
    expect(redacted).toContain('capture_path=dmabuf')
    expect(redacted).not.toContain('hunter2')
    expect(redacted).not.toContain('sessionid=abc123')
  })

  it('handles circular diagnostic values without throwing', () => {
    const value = { name: 'root' }
    value.self = value

    expect(sanitizeDiagnosticsValue(value)).toEqual({ name: 'root', self: '[circular]' })
  })
})

describe('Fix My Stream checklist', () => {
  it('prioritizes connection, packet loss, capture path, encoder pressure, auth pairing, and logs', () => {
    const checklist = buildFixMyStreamChecklist({
      statsConnected: true,
      stats: {
        streaming: true,
        packet_loss: 3.2,
        capture_cpu_copy: true,
        encode_time_ms: 13.5,
      },
      logs: 'Warning: auth failed token=abc123 for paired client',
      recentIssues: [{ level: 'Warning', message: 'auth failed token=abc123' }],
    })

    expect(checklist.map((item) => item.key)).toEqual([
      'connection',
      'packet-loss',
      'capture-path',
      'encoder-pressure',
      'auth-pairing',
      'logs',
    ])
    expect(checklist.map((item) => item.status)).toEqual([
      'pass',
      'fail',
      'fail',
      'fail',
      'fail',
      'warning',
    ])
    expect(checklist[4].detail).toContain(`token=${REDACTED_VALUE}`)
    expect(checklist[4].detail).not.toContain('abc123')
  })


  it('does not treat FEC frame warnings as auth or pairing failures', () => {
    const checklist = buildFixMyStreamChecklist({
      statsConnected: true,
      stats: {
        streaming: true,
        packet_loss: 0,
        capture_gpu_native: true,
        encode_time_ms: 3.2,
      },
      logs: 'Warning: Skipping FEC for abnormally large encoded frame (needed 7 FEC blocks)',
      recentIssues: [
        { level: 'Warning', message: 'Skipping FEC for abnormally large encoded frame (needed 7 FEC blocks)' },
      ],
    })

    const authPairing = checklist.find((item) => item.key === 'auth-pairing')
    expect(authPairing.status).toBe('pass')
    expect(authPairing.detail).toContain('No recent auth or pairing errors')
  })

  it('marks a healthy stream as clean enough to keep playing', () => {
    const checklist = buildFixMyStreamChecklist({
      statsConnected: true,
      stats: {
        streaming: true,
        packet_loss: 0,
        capture_gpu_native: true,
        encode_time_ms: 3.2,
      },
      logs: 'Info: stream started',
      recentIssues: [],
    })

    expect(checklist.every((item) => item.status === 'pass')).toBe(true)
  })

  it('explains AMD VAAPI SHM fallback without vendor-biased copy', () => {
    const stats = {
      streaming: true,
      packet_loss: 0,
      capture_path: 'shm_cpu_capture',
      capture_path_reason: 'gpu_native_requested_shm_fallback',
      capture_cpu_copy: true,
      capture_gpu_native: false,
      encode_target_device: 'vaapi',
      encode_time_ms: 5.2,
      linux_gpu_profile: {
        encoder_api: 'vaapi',
        encoder_adapter: '/dev/dri/renderD128',
        capture_device: '/dev/dri/renderD128',
        adapter_matches_capture_device: true,
        gpu_native_requested: true,
        gpu_native_succeeded: false,
      },
    }

    expect(describeLinuxGpuProfile(stats)).toContain('AMD/VAAPI')
    expect(describeLinuxGpuProfile(stats)).toContain('SHM/system-memory')

    const checklist = buildFixMyStreamChecklist({ statsConnected: true, stats })
    const capture = checklist.find((item) => item.key === 'capture-path')

    expect(capture.detail).toContain('AMD/VAAPI')
    expect(capture.detail).toContain('SHM/system-memory')
    expect(capture.action).toContain('conservative Headless Stream baseline')
    expect(`${capture.detail} ${capture.action}`).not.toContain('CUDA')
    expect(`${capture.detail} ${capture.action}`).not.toContain('NVIDIA')
  })
})
