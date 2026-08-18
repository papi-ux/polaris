import { describe, expect, it } from 'vitest'
import {
  MAX_ISSUE_URL_LENGTH,
  REDACTED_VALUE,
  buildAnonymizedDiagnosticsBundle,
  buildControllerInputTestReport,
  buildFixMyStreamChecklist,
  buildGithubIssueDraft,
  buildGithubIssueUrl,
  buildNetworkPathTestReport,
  buildPostSessionStreamReport,
  buildSupportSelfTestCopy,
  describeLinuxGpuProfile,
  describePreviousRun,
  isSensitiveFieldName,
  redactSensitiveText,
  sanitizeDiagnosticsValue,
} from './diagnostics-export.js'

const crashedRun = {
  recorded: true,
  outcome: 'crashed',
  version: '1.3.11',
  started_at: '2026-08-17T10:00:00Z',
  signal_name: 'SIGSEGV',
  signal_number: 11,
  evidence: 'polaris-crash-v1\nrun_id: r1\nsignal: 11 SIGSEGV\nbacktrace:\npolaris(+0x1234)',
}

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
        linux_gpu_profile: {
          encoder_adapter: '/dev/dri/renderD129',
          capture_device: '',
          wayland_main_device: '/dev/dri/renderD128',
          adapter_pairing_status: 'mismatched',
        },
        gpu_native_probe: {
          windowed: { attempted: true, cached: false, result: 'failed', failure_stage: 'first_frame', failure_reason: 'no_live_dmabuf_frame' },
          selected_strategy: 'headless_shm',
          fallback: 'headless_shm',
        },
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
    expect(draft).toContain('- Encoder adapter: /dev/dri/renderD129')
    expect(draft).toContain('- Wayland main device: /dev/dri/renderD128')
    expect(draft).toContain('- Adapter pairing: mismatched')
    expect(draft).toContain('- GPU-native probe: windowed[result=failed, attempted=yes, cached=no, stage=first_frame, reason=no_live_dmabuf_frame] — selected headless_shm, fallback headless_shm')
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

  it('reports successful GPU-native probe outcomes without calling them unreported', () => {
    const draft = buildGithubIssueDraft({ session_snapshot: { gpu_native_probe: {
      headless_extcopy: { attempted: true, cached: false, result: 'succeeded', failure_stage: '', failure_reason: '' },
      selected_strategy: 'headless_extcopy_dmabuf', fallback: 'none',
    } } })
    expect(draft).toContain('headless_extcopy[result=succeeded, attempted=yes, cached=no]')
    expect(draft).not.toContain('GPU-native probe: not reported')
  })

  it('reports cached probe outcomes as cached rather than live attempts', () => {
    const draft = buildGithubIssueDraft({ session_snapshot: { gpu_native_probe: {
      windowed: { attempted: false, cached: true, result: 'succeeded', failure_stage: '', failure_reason: '' },
      selected_strategy: 'windowed_dmabuf_override', fallback: 'none',
    } } })
    expect(draft).toContain('windowed[result=succeeded, attempted=no, cached=yes]')
  })

  it('preserves explicit not-attempted probe outcomes', () => {
    const draft = buildGithubIssueDraft({ session_snapshot: { gpu_native_probe: {
      headless_extcopy: { attempted: false, cached: false, result: 'not_attempted', failure_stage: '', failure_reason: '' },
      windowed: { attempted: false, cached: false, result: 'not_attempted', failure_stage: '', failure_reason: '' },
      selected_strategy: 'none', fallback: 'none',
    } } })
    expect(draft).toContain('headless_extcopy[result=not_attempted, attempted=no, cached=no]')
    expect(draft).toContain('windowed[result=not_attempted, attempted=no, cached=no]')
  })

  it('reports both strategy failures instead of discarding one reason', () => {
    const draft = buildGithubIssueDraft({ session_snapshot: { gpu_native_probe: {
      headless_extcopy: { attempted: true, cached: false, result: 'failed', failure_stage: 'capture_init', failure_reason: 'dmabuf_capture_not_initialized' },
      windowed: { attempted: true, cached: false, result: 'failed', failure_stage: 'first_frame', failure_reason: 'no_live_dmabuf_frame' },
      selected_strategy: 'headless_shm', fallback: 'headless_shm',
    } } })
    expect(draft).toContain('headless_extcopy[result=failed, attempted=yes, cached=no, stage=capture_init, reason=dmabuf_capture_not_initialized]')
    expect(draft).toContain('windowed[result=failed, attempted=yes, cached=no, stage=first_frame, reason=no_live_dmabuf_frame]')
  })

  it('omits unavailable GPU topology and probe rows instead of fabricating unknown evidence', () => {
    const draft = buildGithubIssueDraft({
      version: '1.2.3-dev',
      session_snapshot: {
        capture_path: 'unknown',
        linux_gpu_profile: {
          encoder_adapter: '/dev/dri/renderD129',
          capture_device: '',
          wayland_main_device: '',
          adapter_pairing_status: 'unknown',
          adapter_pairing_device: '',
          adapter_pairing_device_source: 'none',
        },
      },
    })

    expect(draft).not.toContain('- Encoder adapter: unknown')
    expect(draft).not.toContain('- Capture device: unknown')
    expect(draft).not.toContain('- Wayland main device: unknown')
    expect(draft).not.toContain('- Adapter pairing: unknown')
    expect(draft).not.toContain('- GPU-native probe: not reported')
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

  it('reports a Wayland-to-encoder adapter mismatch before generic SHM fallback guidance', () => {
    const stats = {
      capture_path: 'shm_cpu_capture',
      capture_path_reason: 'gpu_native_requested_shm_fallback',
      capture_cpu_copy: true,
      capture_gpu_native: false,
      encode_target_device: 'vaapi',
      linux_gpu_profile: {
        encoder_api: 'vaapi',
        encoder_adapter: '/dev/dri/renderD129',
        capture_device: '',
        wayland_main_device: '/dev/dri/renderD128',
        adapter_matches_capture_device: null,
        adapter_matches_wayland_main_device: false,
        adapter_pairing_status: 'mismatched',
        adapter_pairing_device_source: 'wayland_main_device',
        gpu_native_requested: true,
        gpu_native_succeeded: false,
      },
      gpu_native_probe: {
        requested: true,
        attempted: true,
        windowed: {
          result: 'failed',
          failure_stage: 'first_frame',
          failure_reason: 'no_live_dmabuf_frame',
        },
        selected_strategy: 'headless_shm',
        fallback: 'headless_shm',
      },
    }

    const description = describeLinuxGpuProfile(stats)
    expect(description).toContain('/dev/dri/renderD129')
    expect(description).toContain('/dev/dri/renderD128')
    expect(description).toContain('Wayland compositor')
    expect(description).not.toContain('NVIDIA')
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


describe('support self-service reports', () => {
  it('classifies a lossy remote network path and recommends a safer bitrate ceiling', () => {
    const report = buildNetworkPathTestReport({
      host: '203.0.113.40',
      originHostname: 'polaris-host.local',
      controlPortOpen: true,
      streamPortOpen: false,
      mdnsAvailable: false,
      pingSamplesMs: [38, 55, 92, 44],
      packetLossPercent: 4.5,
      currentBitrateKbps: 60000,
    })

    expect(report.status).toBe('fail')
    expect(report.classification).toBe('network')
    expect(report.summary).toContain('remote/VPN')
    expect(report.recommendedBitrateKbps).toBeLessThanOrEqual(30000)
    expect(report.checks.map((check) => check.key)).toEqual([
      'host-reachable',
      'control-port',
      'stream-port',
      'discovery-mdns',
      'lan-vpn-clue',
      'latency-jitter-loss',
      'bitrate-ceiling',
    ])
    expect(report.checks.find((check) => check.key === 'stream-port').status).toBe('fail')
  })

  it('prefers native server-side network probe evidence when available', () => {
    const report = buildNetworkPathTestReport({
      nativeProbe: {
        targetHost: '100.72.10.4',
        classification: 'vpn',
        mdnsAvailable: false,
        hostReachable: true,
        samples: { latencyMs: [16, 19, 18], jitterMs: 2, packetLossPercent: 0 },
        ports: [
          { key: 'control_https', label: 'Web/control HTTPS', port: 47990, transport: 'tcp', status: 'open' },
          { key: 'rtsp_setup', label: 'RTSP setup', port: 48010, transport: 'tcp', status: 'closed' },
          { key: 'video_udp', label: 'Video stream', port: 47998, transport: 'udp', status: 'hint' },
        ],
      },
      currentBitrateKbps: 60000,
    })

    expect(report.summary).toContain('VPN')
    expect(report.checks.find((check) => check.key === 'host-reachable').status).toBe('pass')
    expect(report.checks.find((check) => check.key === 'control-port').detail).toContain('47990/tcp')
    expect(report.checks.find((check) => check.key === 'stream-port').status).toBe('fail')
    expect(report.advancedEvidence.nativeProbe).toEqual(expect.objectContaining({ classification: 'vpn' }))
  })

  it('includes redacted native network evidence in support-copy text', () => {
    const report = buildNetworkPathTestReport({
      nativeProbe: {
        targetHost: '192.168.1.44',
        classification: 'lan',
        hostReachable: true,
        ports: [{ key: 'control_https', label: 'Web/control HTTPS', port: 47990, transport: 'tcp', status: 'open' }],
        notes: ['token=abc123 should never survive copy'],
      },
    })

    const copy = buildSupportSelfTestCopy({ network: report })

    expect(copy).toContain('Native evidence')
    expect(copy).toContain('Web/control HTTPS 47990/tcp: open')
    expect(copy).toContain(`token=${REDACTED_VALUE}`)
    expect(copy).not.toContain('abc123')
  })

  it('summarizes controller input events with native virtual pad, isolation, and haptics evidence', () => {
    const report = buildControllerInputTestReport({
      events: [
        { pad: 1, control: 'A', type: 'buttondown' },
        { pad: 2, control: 'Left Stick', type: 'axis', value: 0.74 },
      ],
      gamepads: [{ index: 0, id: 'Xbox Wireless Controller' }, { index: 1, id: 'DualSense' }],
      native: {
        virtualControllerCreated: true,
        virtualControllerNumber: 2,
        virtualControllerKind: 'xone',
        hostControllerIsolation: 'strict_bwrap',
        hostControllerIsolationDetail: '2 virtual nodes allowed; host pads masked',
        hapticsSupported: true,
        hapticsDetail: 'rumble callbacks registered for client pad 2',
      },
    })

    expect(report.status).toBe('pass')
    expect(report.summary).toContain('2 client control events')
    expect(report.summary).toContain('native virtual pad #2')
    expect(report.checks.find((check) => check.key === 'multi-pad').detail).toContain('2 client pads')
    expect(report.checks.find((check) => check.key === 'rumble').status).toBe('pass')
    expect(report.checks.find((check) => check.key === 'host-isolation').status).toBe('pass')
    expect(report.checks.find((check) => check.key === 'host-isolation').detail).toContain('strict_bwrap')
    expect(report.advancedEvidence.native.virtualControllerKind).toBe('xone')
  })

  it('builds a post-session report with issue owner and next launch profile', () => {
    const report = buildPostSessionStreamReport({
      stats: {
        streaming: false,
        packet_loss: 0.1,
        latency_ms: 6.4,
        encode_time_ms: 15.2,
        dropped_frame_ratio: 0.02,
        capture_cpu_copy: true,
        capture_gpu_native: false,
        stream_display_mode: 'headless_stream',
      },
      logs: 'Warning: encoder queue saturated after capture fell back to SHM',
      disconnectReason: 'client disconnected',
    })

    expect(report.issueOwner).toBe('host')
    expect(report.mainIssue).toContain('encoder')
    expect(report.suggestedNextLaunchProfile).toContain('Private Stream')
    expect(report.copyText).toContain('Issue owner: host')
    expect(report.copyText).not.toContain('token=')
  })

  it('generates support-copy text for network, controller, and post-session reports', () => {
    const copy = buildSupportSelfTestCopy({
      network: buildNetworkPathTestReport({ controlPortOpen: true, streamPortOpen: true, pingSamplesMs: [4, 5], packetLossPercent: 0 }),
      controller: buildControllerInputTestReport({ events: [{ pad: 1, control: 'A' }], virtualController: { created: true } }),
      postSession: buildPostSessionStreamReport({ stats: { packet_loss: 3.4, latency_ms: 70 }, logs: 'Warning: packet loss spike token=abc123' }),
    })

    expect(copy).toContain('Network Path Tester')
    expect(copy).toContain('Controller/Input Tester')
    expect(copy).toContain('Post-session Stream Report')
    expect(copy).toContain(`token=${REDACTED_VALUE}`)
    expect(copy).not.toContain('abc123')
  })
})

describe('redaction defects closed', () => {
  it('redacts a credential whose name carries a prefix', () => {
    // A word-boundary anchor matched token= and missed auth_token=, because an
    // underscore is a word character. Prefixed names are the common shape in a
    // log line, so every one of them was exported verbatim.
    const redacted = redactSensitiveText(
      'auth_token=hunter2 api-key=abc123 session.secret=xyz789 X-Auth-Token: qqq111'
    )

    expect(redacted).not.toContain('hunter2')
    expect(redacted).not.toContain('abc123')
    expect(redacted).not.toContain('xyz789')
    expect(redacted).not.toContain('qqq111')
    expect(redacted).toContain(`auth_token=${REDACTED_VALUE}`)
  })

  it('leaves ordinary words that merely contain a keyword alone', () => {
    // Over-redaction is not free: it eats the diagnostics the bundle exists for.
    const redacted = redactSensitiveText('keyboard=us monkey=banana capture_path=dmabuf packet_loss=2.5')

    expect(redacted).toBe('keyboard=us monkey=banana capture_path=dmabuf packet_loss=2.5')
  })

  it('treats a field named exactly key as a label, not a credential', () => {
    // checklistItem() and the network probe both use a `key` field as an
    // identifier. Blanking those removed the labels that make a bundle readable
    // while protecting nothing.
    expect(isSensitiveFieldName('key')).toBe(false)
    expect(isSensitiveFieldName('monkey')).toBe(false)
    expect(isSensitiveFieldName('keyboard')).toBe(false)
  })

  it('still treats a qualified key as a credential', () => {
    expect(isSensitiveFieldName('api_key')).toBe(true)
    expect(isSensitiveFieldName('apiKey')).toBe(true)
    expect(isSensitiveFieldName('private-key')).toBe(true)
  })

  it('still redacts every field name it protected before', () => {
    for (const name of ['password', 'token', 'api_token', 'accessToken', 'secret', 'cookie', 'auth', 'credential']) {
      expect(isSensitiveFieldName(name)).toBe(true)
    }
    for (const name of ['bitrate', 'capture_path', 'client_ip', 'fps', '']) {
      expect(isSensitiveFieldName(name)).toBe(false)
    }
  })

  it('keeps checklist labels readable through a real export', () => {
    const bundle = buildAnonymizedDiagnosticsBundle({
      fix_my_stream_checklist: [
        { key: 'capture-path', label: 'Capture path', status: 'warning', detail: 'fell back to SHM' },
      ],
      config: { api_key: 'tok_live_secret' },
    })

    expect(bundle.fix_my_stream_checklist[0].key).toBe('capture-path')
    expect(bundle.config.api_key).toBe(REDACTED_VALUE)
  })
})

describe('redaction across naming conventions', () => {
  it('redacts camelCase credential names in free text', () => {
    // The first fix handled snake and kebab and missed camelCase entirely, which
    // is the house style of this codebase's own JavaScript.
    const redacted = redactSensitiveText(
      'apiKey=abc123 authToken=tok_1 accessToken: at_2 clientSecret=cs_3 userPassword=pw_5'
    )

    for (const leaked of ['abc123', 'tok_1', 'at_2', 'cs_3', 'pw_5']) {
      expect(redacted).not.toContain(leaked)
    }
  })

  it('redacts plural and spelled-out credential names', () => {
    const redacted = redactSensitiveText('credentials=zzz tokens=ttt authorization=xyz passwd=ppp')

    for (const leaked of ['zzz', 'ttt', 'xyz', 'ppp']) {
      expect(redacted).not.toContain(leaked)
    }
  })

  it('agrees between a structured field and the same name in log text', () => {
    // Two independent definitions is what let apiKey= leak from a log line while
    // a field named apiKey was correctly redacted.
    for (const name of ['apiKey', 'auth_token', 'accessToken', 'credentials', 'authorization']) {
      expect(isSensitiveFieldName(name)).toBe(true)
      expect(redactSensitiveText(`${name}=zzz999`)).not.toContain('zzz999')
    }
    for (const name of ['keyboard', 'monkey', 'capture_path', 'keyName']) {
      expect(isSensitiveFieldName(name)).toBe(false)
      expect(redactSensitiveText(`${name}=zzz999`)).toContain('zzz999')
    }
  })

  it('treats a bare key as a label in a field and as a credential in free text', () => {
    // The one place the two paths differ, on purpose. A surrounding object says
    // what a `key` field is; a log line says nothing, so free text stays
    // conservative.
    expect(isSensitiveFieldName('key')).toBe(false)
    expect(redactSensitiveText('key=barevalue')).not.toContain('barevalue')
  })

  it('does not treat key as a credential when it leads the name', () => {
    expect(isSensitiveFieldName('keyName')).toBe(false)
    expect(isSensitiveFieldName('keyCode')).toBe(false)
    expect(isSensitiveFieldName('apiKey')).toBe(true)
    expect(isSensitiveFieldName('publicKey')).toBe(true)
  })
})

describe('separator-less credential names', () => {
  it('redacts a qualifier glued to a sensitive word', () => {
    // apikey has no separator and no camelCase hump, so segmentation alone
    // cannot see it. Polaris already treats a bare "apikey" header as sensitive
    // in the artwork request sanitiser; the redactor now agrees with it.
    const redacted = redactSensitiveText(
      'apikey=aaa111 apisecret=bbb222 authtoken=ccc333 accesstoken=ddd444 clientsecret=eee555 privatekey=fff666'
    )

    for (const leaked of ['aaa111', 'bbb222', 'ccc333', 'ddd444', 'eee555', 'fff666']) {
      expect(redacted).not.toContain(leaked)
    }
  })

  it('recognises the same names as fields', () => {
    for (const name of ['apikey', 'apisecret', 'authtoken', 'accesstoken', 'dbpassword', 'apikeys']) {
      expect(isSensitiveFieldName(name)).toBe(true)
    }
  })

  it('still leaves words that merely end in a sensitive word alone', () => {
    // monkey and apikey are structurally identical: a prefix followed by "key".
    // Only a known qualifier makes the difference, which is why the list is
    // explicit rather than a substring test.
    for (const name of ['monkey', 'keyboard', 'turnkey', 'passwordless']) {
      expect(isSensitiveFieldName(name)).toBe(false)
      expect(redactSensitiveText(`${name}=zzz999`)).toContain('zzz999')
    }
  })
})

describe('credentials inside another pair', () => {
  it('redacts a credential that follows an innocent label', () => {
    // The ordinary shape of a log line is "Something: detail". Matching the
    // outer pair and stopping consumed the credential as someone else's value
    // and left it in the output, which made this the most common leak of all.
    const redacted = redactSensitiveText([
      'java.io.IOException: auth_token=hunter2',
      'Error at Foo.bar: apikey=abc123',
      'WARN [stream]: clientSecret=cs9',
      'a: b: c: token=deep',
    ].join('\n'))

    for (const leaked of ['hunter2', 'abc123', 'cs9', 'deep']) {
      expect(redacted).not.toContain(leaked)
    }
  })

  it('leaves an innocent pair intact when nothing inside it is sensitive', () => {
    const survives = 'Info: capture_path=dmabuf\nWarning: packet_loss=2.5\nlevel: info'

    expect(redactSensitiveText(survives)).toBe(survives)
  })

  it('still keeps the auth scheme when the header follows a label', () => {
    expect(redactSensitiveText('Request failed, Authorization: Bearer ey.secret'))
      .toContain(`Bearer ${REDACTED_VALUE}`)
  })
})

describe('redaction is idempotent', () => {
  it('does not accumulate on repeated passes', () => {
    // Nova redacts before it posts a report and the host redacts again on
    // export, so two passes over the same bytes is the normal path. A user
    // should not be able to tell how many times it ran by counting brackets.
    let text = 'Warning: auth_token=hunter2 apiKey=abc123'
    const passes = []
    for (let index = 0; index < 5; index += 1) {
      text = redactSensitiveText(text)
      passes.push(text)
    }

    expect(new Set(passes).size).toBe(1)
    expect(passes[0]).not.toContain('hunter2')
    expect(passes[0]).not.toContain(']]')
  })

  it('captures a bracketed value whole instead of leaving the bracket behind', () => {
    expect(redactSensitiveText('token=[abc]')).toBe(`token=${REDACTED_VALUE}`)
    expect(redactSensitiveText('note=[abc]')).toBe('note=[abc]')
  })

  it('leaves an already redacted document untouched', () => {
    const already = 'Info: capture_path=dmabuf\nWarning: auth_token=[redacted]\nlevel: info'

    expect(redactSensitiveText(already)).toBe(already)
  })
})

describe('quoted and delimited names', () => {
  it('redacts a credential behind a quoted JSON key', () => {
    // JSON always quotes its keys, and Polaris logs JSON response bodies, so
    // requiring the name to reach its separator directly missed every one of
    // them. This is the same family as the two defects above: the scan failing
    // to see a name that is right there.
    const redacted = redactSensitiveText([
      '{"api_key": "abc123"}',
      "{'apiKey': 'x9'}",
      '{"auth_token":"t1"}',
      '"client_secret": cs9',
    ].join('\n'))

    for (const leaked of ['abc123', 'x9', 't1', 'cs9']) {
      expect(redacted).not.toContain(leaked)
    }
  })

  it('redacts through a closing paren or bracket before the separator', () => {
    expect(redactSensitiveText('(api_key): abc123')).not.toContain('abc123')
    expect(redactSensitiveText('[auth_token]: t1')).not.toContain('t1')
  })

  it('leaves innocent quoted keys and their values alone', () => {
    const survives = '{"level": "info", "capture_path": "dmabuf", "keyName": "readable"}'

    expect(redactSensitiveText(survives)).toBe(survives)
  })

  it('stays idempotent on a redacted json document', () => {
    const once = redactSensitiveText('{"api_key": "abc123"}')

    expect(redactSensitiveText(once)).toBe(once)
  })
})

describe('a sensitive name whose value is a structure', () => {
  it('redacts the whole object, not its opening fragment', () => {
    // Stopping early is worse than not matching: it leaves the secret behind
    // while "auth": [redacted] reads as though the subtree was handled, so a
    // human skimming the bundle would sign it off.
    expect(redactSensitiveText('{"auth": {"api_key": "abc123"}}')).toBe(`{"auth": ${REDACTED_VALUE}}`)
    expect(redactSensitiveText('{"secret": {"inner": "s1"}}')).toBe(`{"secret": ${REDACTED_VALUE}}`)
    expect(redactSensitiveText('{auth: {api_key: "a1"}}')).toBe(`{auth: ${REDACTED_VALUE}}`)
  })

  it('redacts an arbitrarily nested structure whole', () => {
    expect(redactSensitiveText('{"cfg": {"auth": {"api_key": "x9"}}}')).not.toContain('x9')
  })

  it('redacts an array value whole', () => {
    expect(redactSensitiveText('{"tokens": ["t1", "t2"]}')).toBe(`{"tokens": ${REDACTED_VALUE}}`)
  })

  it('still reaches a credential nested under an innocent name', () => {
    expect(redactSensitiveText('{"cfg": {"api_key": "x"}}')).toBe(`{"cfg": {"api_key": ${REDACTED_VALUE}}}`)
  })

  it('is not confused by a brace inside a quoted string', () => {
    expect(redactSensitiveText('{"auth": {"note": "a } brace", "api_key": "k1"}}'))
      .toBe(`{"auth": ${REDACTED_VALUE}}`)
  })

  it('still redacts when the structure never closes', () => {
    // Degenerate input must not become a reason to leave a secret alone.
    expect(redactSensitiveText('api_key={"a": "b"')).not.toContain('"b"')
    expect(redactSensitiveText('token=[')).toBe(`token=${REDACTED_VALUE}`)
  })

  it('stays idempotent over a redacted structure', () => {
    const once = redactSensitiveText('{"auth": {"api_key": "a"}}')

    expect(redactSensitiveText(once)).toBe(once)
  })
})

describe('the redaction notice describes what the code does', () => {
  it('does not promise substring matching the code no longer does', () => {
    const notice = buildAnonymizedDiagnosticsBundle({}).redaction_notice

    // It used to say "fields containing ... key", which claimed keyboard and
    // monkey were redacted and that a bare key field was. Neither is true, and
    // this is the promise someone reads before posting a bundle publicly.
    expect(notice).not.toContain('Fields containing')
    expect(notice).toContain('Whole words')
    expect(notice).toContain('qualifies it')
  })

  it('survives its own rules', () => {
    // The first attempt at rewording tripped them: "credential: password" and
    // "key=" inside the notice were themselves redacted.
    expect(buildAnonymizedDiagnosticsBundle({}).redaction_notice).not.toContain(REDACTED_VALUE)
  })

  it('keeps discriminator keys readable in a real bundle', () => {
    // browser-stream-support.js uses key: 'touch', key: 'keyboard' and seven
    // more as capability discriminators. Redacting those destroyed the table.
    const bundle = buildAnonymizedDiagnosticsBundle({
      capabilities: [{ key: 'keyboard', supported: true }, { key: 'gamepad', supported: false }],
    })

    expect(bundle.capabilities[0].key).toBe('keyboard')
    expect(bundle.capabilities[1].key).toBe('gamepad')
  })
})

describe('previous run reporting', () => {
  it('names the signal a crashed run died on', () => {
    expect(describePreviousRun(crashedRun)).toContain('SIGSEGV')
  })

  it('separates a kill from a fault instead of calling both a crash', () => {
    const summary = describePreviousRun({ recorded: true, outcome: 'unclean' })

    expect(summary).toContain('killed')
    expect(summary).not.toContain('crashed')
  })

  it('says nothing about a run that ended cleanly or was never recorded', () => {
    expect(describePreviousRun({ recorded: true, outcome: 'clean' })).toBe('')
    expect(describePreviousRun({ recorded: false, outcome: 'unknown' })).toBe('')
    expect(describePreviousRun()).toBe('')
  })

  it('puts the crash and its backtrace in the issue draft', () => {
    const draft = buildGithubIssueDraft({ version: '1.3.11', crash: crashedRun })

    expect(draft).toContain('## How the previous run ended')
    expect(draft).toContain('SIGSEGV')
    expect(draft).toContain('polaris(+0x1234)')
  })

  it('leaves the crash section out entirely when there was no crash', () => {
    const draft = buildGithubIssueDraft({ version: '1.3.11', crash: { recorded: true, outcome: 'clean' } })

    expect(draft).not.toContain('## How the previous run ended')
  })

  it('redacts a token that appears inside a backtrace', () => {
    const draft = buildGithubIssueDraft({
      version: '1.3.11',
      crash: { ...crashedRun, evidence: 'polaris-crash-v1\nbacktrace:\npolaris(auth_token=hunter2)' },
    })

    expect(draft).not.toContain('hunter2')
    expect(draft).toContain(REDACTED_VALUE)
  })
})

describe('silent failure reporting', () => {
  const silentFailures = [{
    id: 'display_topology.output_mode',
    description: 'Set the streaming output to the mode the session asked for',
    requested: '2560x1440@120Hz',
    actual: '2560x1440@60Hz',
    observed_at: '2026-08-17T10:05:00Z',
  }]

  it('reports what was requested next to what the system actually did', () => {
    const draft = buildGithubIssueDraft({ version: '1.3.11', silent_failures: silentFailures })

    expect(draft).toContain('## Actions that reported success and did not land')
    expect(draft).toContain('requested [2560x1440@120Hz]')
    expect(draft).toContain('system reported [2560x1440@60Hz]')
  })

  it('reads them from the doctor payload when they arrive that way', () => {
    const draft = buildGithubIssueDraft({
      version: '1.3.11',
      session_snapshot: { doctor: { silent_failures: silentFailures } },
    })

    expect(draft).toContain('## Actions that reported success and did not land')
  })

  it('leaves the section out when nothing failed silently', () => {
    expect(buildGithubIssueDraft({ version: '1.3.11', silent_failures: [] }))
      .not.toContain('## Actions that reported success and did not land')
  })

  it('carries crash and silent failures into the bundle at version 3', () => {
    const bundle = buildAnonymizedDiagnosticsBundle({
      version: '1.3.11',
      crash: crashedRun,
      silent_failures: silentFailures,
    })

    expect(bundle.support_bundle_version).toBe(3)
    expect(bundle.crash.outcome).toBe('crashed')
    expect(bundle.silent_failures).toHaveLength(1)
    expect(bundle.issue_draft).toContain('SIGSEGV')
  })
})

describe('prefilled github issue url', () => {
  const context = {
    version: '1.3.11',
    platform: 'Fedora 44',
    system_stats: { gpu: { name: 'RTX 4090', driver: '580.95' } },
    client: { type: 'Nova', name: 'Retroid Pocket 6' },
    session_snapshot: { launch_mode: 'headless_stream', capture_path: 'dmabuf', encoder: 'nvenc' },
    crash: crashedRun,
  }

  it('prefills the issue form fields by their own ids', () => {
    const url = new URL(buildGithubIssueUrl(context))

    expect(url.pathname).toBe('/papi-ux/polaris/issues/new')
    expect(url.searchParams.get('template')).toBe('bug_report.yml')
    expect(url.searchParams.get('host-os')).toBe('Fedora 44')
    expect(url.searchParams.get('gpu')).toContain('RTX 4090')
    expect(url.searchParams.get('client')).toContain('Nova')
    expect(url.searchParams.get('runtime')).toContain('headless_stream')
    expect(url.searchParams.get('describe-bug')).toContain('SIGSEGV')
  })

  it('does not guess the install method', () => {
    // The dropdown takes exact option strings and Polaris cannot know the
    // answer. A wrong guess in that field is worse than an empty one.
    expect(new URL(buildGithubIssueUrl(context)).searchParams.get('install-method')).toBeNull()
  })

  it('carries the user note through to the description', () => {
    const url = new URL(buildGithubIssueUrl({ ...context, user_notes: 'Happens only after resume.' }))

    expect(url.searchParams.get('describe-bug')).toContain('Happens only after resume.')
  })

  it('truncates the evidence rather than dropping the environment answers', () => {
    const noisy = {
      ...context,
      recent_issues: Array.from({ length: 8 }, (_, index) => ({
        timestamp: '2026-08-17T10:00:00Z',
        level: 'Error',
        message: `failure ${index} ${'padding '.repeat(60)}`,
      })),
    }

    const maxLength = 900
    const url = buildGithubIssueUrl(noisy, { maxLength })
    const parsed = new URL(url)

    expect(url.length).toBeLessThanOrEqual(maxLength)
    // The environment answers are the part a maintainer cannot reconstruct, so
    // they must survive a squeeze that the log excerpt does not.
    expect(parsed.searchParams.get('host-os')).toBe('Fedora 44')
    expect(parsed.searchParams.get('gpu')).toContain('RTX 4090')
    expect(parsed.searchParams.get('logs')).toContain('truncated')
  })

  it('stays within the default length cap without being asked', () => {
    expect(buildGithubIssueUrl(context).length).toBeLessThanOrEqual(MAX_ISSUE_URL_LENGTH)
  })

  it('redacts secrets before they can reach a url', () => {
    const url = buildGithubIssueUrl({
      ...context,
      recent_issues: [{ timestamp: '2026-08-17T10:00:00Z', level: 'Error', message: 'auth_token=hunter2 failed' }],
    })

    expect(url).not.toContain('hunter2')
  })

  it('targets an overridden repository when one is given', () => {
    const url = buildGithubIssueUrl(context, { repositoryUrl: 'https://github.com/papi-ux/nova/' })

    expect(new URL(url).pathname).toBe('/papi-ux/nova/issues/new')
  })
})
