import { describe, expect, it } from 'vitest'
import {
  REDACTED_VALUE,
  buildAnonymizedDiagnosticsBundle,
  buildFixMyStreamChecklist,
  buildGithubIssueDraft,
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


  it('redacts Doctor inputs before support export and preserves safe action boundaries', () => {
    const bundle = buildAnonymizedDiagnosticsBundle({
      stream: {
        client_ip: '10.0.0.22',
        auth_token: 'tok_live_secret',
        doctor: {
          result_id: 'doctor-v1-watch-gpu_native_requested_shm_fallback',
          primary_issue: 'gpu_native_requested_shm_fallback',
          safe_recovery_action: {
            id: 'export_support_bundle',
            destructive: false,
            requires_confirmation: true,
            payload_preview: { session_token: 'tok_live_secret' },
          },
          evidence: [
            { id: 'capture_path', detail: 'Authorization: Bearer tok_live_secret' },
          ],
        },
      },
    })

    expect(bundle.stream.doctor.result_id).toBe('doctor-v1-watch-gpu_native_requested_shm_fallback')
    expect(bundle.stream.doctor.safe_recovery_action.destructive).toBe(false)
    expect(bundle.stream.auth_token).toBe(REDACTED_VALUE)
    expect(bundle.stream.doctor.safe_recovery_action.payload_preview.session_token).toBe(REDACTED_VALUE)
    expect(bundle.stream.doctor.evidence[0].detail).toContain(`Bearer ${REDACTED_VALUE}`)
    expect(JSON.stringify(bundle)).not.toContain('tok_live_secret')
  })
})


describe('GitHub issue draft support flow', () => {
  it('builds a redacted GitHub-ready issue draft from support evidence without submitting it', () => {
    const draft = buildGithubIssueDraft({
      platform: 'linux',
      version: '1.2.3-dev',
      config: {
        distro: 'Fedora 44',
        session_type: 'wayland',
        compositor: 'kwin_wayland',
        encoder: 'vaapi',
        gpu: 'AMD Radeon RX 7900 XTX',
        driver: 'Mesa 26.0.0',
      },
      system_stats: {
        gpu: { name: 'AMD Radeon RX 7900 XTX', driver: 'Mesa 26.0.0' },
        os: { distro: 'Fedora 44' },
        session: { type: 'wayland', compositor: 'kwin_wayland' },
      },
      client: { type: 'Nova', name: 'Living Room Deck' },
      session_snapshot: {
        client_name: 'Nova',
        launch_mode: 'Private Stream',
        encode_target_device: 'vaapi',
        capture_path: 'shm_cpu_capture',
        capture_path_reason: 'gpu_native_requested_shm_fallback',
        streaming: true,
        fps: 118.5,
        session_target_fps: 120,
        bitrate_kbps: 45000,
        packet_loss: 2.4,
        encode_time_ms: 9.8,
        doctor: {
          simple_state: 'Stream is playable, but capture fell back to system memory.',
          primary_issue: 'gpu_native_requested_shm_fallback',
          evidence: [{ id: 'capture', detail: 'password=hunter2 capture_path=shm_cpu_capture' }],
          safe_recovery_action: { id: 'export_support_bundle', destructive: false },
        },
      },
      fix_my_stream_checklist: [
        { label: 'Capture path', status: 'warning', detail: 'GPU-native capture fell back to SHM/system-memory frames.', action: 'Export support bundle.' },
      ],
      recent_issues: [
        { timestamp: '[12:00:01]', level: 'Warning', message: 'Authorization: Bearer abc123 capture fallback', count: 2 },
      ],
      user_notes: 'I already tried restarting Polaris and re-pairing Nova.',
    })

    expect(draft).toContain('## Environment')
    expect(draft).toContain('- Polaris version: 1.2.3-dev')
    expect(draft).toContain('- Distro: Fedora 44')
    expect(draft).toContain('- GPU: AMD Radeon RX 7900 XTX')
    expect(draft).toContain('- Driver: Mesa 26.0.0')
    expect(draft).toContain('- Session/compositor: wayland / kwin_wayland')
    expect(draft).toContain('- Client: Nova (Living Room Deck)')
    expect(draft).toContain('- Launch mode: Private Stream')
    expect(draft).toContain('- Encoder: vaapi')
    expect(draft).toContain('- Capture: shm_cpu_capture — gpu_native_requested_shm_fallback')
    expect(draft).toContain('- Active stream: yes, 118.5 FPS / 120.0 target, 45000 kbps, 2.40% loss, 9.8 ms encode')
    expect(draft).toContain('## What Polaris thinks happened')
    expect(draft).toContain('Stream is playable, but capture fell back to system memory.')
    expect(draft).toContain('gpu_native_requested_shm_fallback')
    expect(draft).toContain('## Recent warnings/errors')
    expect(draft).toContain('Bearer [redacted]')
    expect(draft).toContain('## What I already tried')
    expect(draft).toContain('I already tried restarting Polaris and re-pairing Nova.')
    expect(draft).toContain('This report was generated locally from a redacted Polaris support bundle. Nothing was submitted automatically.')
    expect(draft).not.toContain('hunter2')
    expect(draft).not.toContain('abc123')
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
    expect(capture.action).toContain('conservative Private Stream baseline')
    expect(`${capture.detail} ${capture.action}`).not.toContain('CUDA')
    expect(`${capture.detail} ${capture.action}`).not.toContain('NVIDIA')
  })

  it('surfaces NVIDIA true-headless GPU-native configuration warnings before capture tuning', () => {
    const checklist = buildFixMyStreamChecklist({
      statsConnected: true,
      stats: {
        streaming: true,
        packet_loss: 0,
        encode_time_ms: 4.2,
        linux_gpu_profile: {
          configuration_warnings: [
            {
              id: 'nvidia_headless_gpu_native_disabled',
              severity: 'warning',
              message: 'NVIDIA true-headless labwc is configured with GPU-native capture disabled, which can cause cold-cache 503 encoder-init failures.',
              action: 'Set linux_prefer_gpu_native_capture = enabled, restart Polaris, then retry Private Stream (GPU-native).',
            },
          ],
        },
      },
    })

    const hostConfig = checklist.find((item) => item.key === 'host-config')
    expect(hostConfig.status).toBe('warning')
    expect(hostConfig.detail).toContain('cold-cache 503')
    expect(hostConfig.action).toContain('linux_prefer_gpu_native_capture = enabled')
    expect(checklist.map((item) => item.key)[1]).toBe('host-config')
  })
})
