// Doctor & Support before a stream, in the fully mocked frontend lane (#152): the host's current
// configuration findings are visible under the neutral waiting headline, the live rows stay out of
// view, a finding the host has not verified is a note, and a dropped connection or a live stream
// keeps its own behaviour. Every host API is fulfilled here; the stats stream is a fake EventSource
// whose behaviour each case chooses.
import { test, expect } from './fixtures/mocked.js'

const waiting = { primary_issue: 'no_active_stream', status: 'unknown', severity: 'info', simple_state: 'Waiting for a stream' }
const setAside = {
  id: 'capture_set_aside_by_mode', severity: 'info', stream_mode: 'windowed_stream',
  configured_capture: 'kms', effective_capture: 'wlr', reason: 'private_compositor',
  message: 'capture override: stream mode [windowed_stream] asks for [wlr], not the configured [kms], because a private labwc session can only be captured through wlroots. Nothing is wrong with this pairing: capture = kms stays saved for the modes that use it, and a stream in this mode captures through wlr.',
  action: 'No change is needed. Keep capture = kms if a later mode switch should use it, or leave capture unset to let each mode choose.',
}
const unavailable = {
  id: 'stream_mode_unavailable', severity: 'warning', stream_mode: 'windowed_stream', reason: 'labwc binary not found on PATH',
  message: 'The configured stream mode [windowed_stream] cannot be entered on this host: labwc binary not found on PATH. A launch into it is refused with the same reason until the host has what it needs.',
  action: 'Install what the reason names, or choose a stream mode this host can enter in Settings.',
}
const staleSubstitution = {
  id: 'capture_backend_substituted', severity: 'warning', stale: true,
  message: 'The capture backend this host is configured to use cannot capture anything in the current stream mode, so Polaris substituted another one (kms -> portal).',
  action: 'Either set capture to the substituted backend so the configuration matches what is running, or go back to a private-compositor stream mode if you want the configured one.',
}
const STALE_NOTE = 'This capture finding has not been verified for the current settings. Polaris checks it again when the next stream starts.'
const STALE_ACTION = 'Start a stream to recheck capture with the current settings.'
const PLACEHOLDERS = [
  'Telemetry is connected, but no active stream is running.',
  'No current confirmed video frame loss measurement is available.',
  'No active connected stream is available to verify capture.',
  'No current encoder timing is available.',
]

const idle = (warnings) => ({
  streaming: false, fps: 0, encode_time_ms: 0, bitrate_kbps: 0, doctor: waiting,
  linux_gpu_profile: { configuration_warnings: warnings },
})
const live = () => ({
  streaming: true, stream_instance_id: 'process-a:1', client_name: 'Living Room',
  clients: [{ stream_instance_id: 'process-a:1', fps: 120 }],
  video_sample_revision: 20, video_policy_sample_count: 20,
  media_loss_sample_revision: 10, media_loss_last_received_age_ms: 100,
  network_sample_revision: 20, network_last_received_age_ms: 100,
  fps: 120, bitrate_kbps: 45000, encode_time_ms: 4, encode_target_fps: 120, dropped_frame_ratio: 0, latency_ms: 7,
  capture_gpu_native: true, capture_path: 'wlr_dmabuf', capture_path_reason: 'gpu_native',
  network_verdict: {
    loss_basis: 'video_frames_lost_after_fec', loss_state: 'clean', loss_pct: 0,
    frames_expected: 2400, frames_lost: 0, media_samples: 20, window_seconds: 20,
    rtt_state: 'clean', rtt_median_ms: 7, rtt_samples: 20,
  },
  doctor: { primary_issue: 'none', status: 'ok', severity: 'info', simple_state: 'Streaming ready', summary: 'Streaming telemetry looks ready.' },
  linux_gpu_profile: { configuration_warnings: [setAside] },
})

const gpu = { name: 'Fixture GPU', vendor: 'nvidia', temperature_c: 0, utilization_pct: 0, encoder_pct: null, vram_used_mb: 2048, vram_total_mb: 8192 }
const config = {
  status: true, platform: 'linux', version: '1.4.14-beta.2', sunshine_name: 'Fixture Host',
  configuration_revision: 'a'.repeat(64), linux_stream_mode: 'windowed_stream', capture: 'kms', headless_mode: 'disabled',
  encoder_codec_support: { ready: true, encoder: 'nvenc', hevc_supported: true, pyrowave: { available: true, hdr: false } },
}

// stream: 'connected' delivers the payload every 250 ms; 'dropped' delivers it once, then the
// source errors and every poll fails, which is how the console loses a host.
async function mockHost(page, { stats, stream = 'connected' }) {
  await page.addInitScript(({ stats, stream }) => {
    localStorage.setItem('polaris-theme', 'polaris')
    window.__delivered = 0
    window.EventSource = class {
      constructor() {
        this.timer = setInterval(() => {
          this.onopen?.({})
          this.onmessage?.({ data: JSON.stringify(stats) })
          window.__delivered += 1
          if (stream === 'dropped') {
            clearInterval(this.timer)
            this.onerror?.({})
          }
        }, 250)
      }
      close() { clearInterval(this.timer) }
    }
  }, { stats, stream })
  await page.route('**/api/**', async route => {
    const path = new URL(route.request().url()).pathname
    if (path === '/api/stats/stream') {
      return stream === 'dropped'
        ? route.fulfill({ status: 503, json: { status: false } })
        : route.fulfill({ json: stats })
    }
    if (path === '/api/configLocale') return route.fulfill({ json: { locale: 'en' } })
    if (path === '/api/config') return route.fulfill({ json: config })
    if (path === '/api/settings/metadata') return route.fulfill({ status: 404, json: { status: false } })
    if (path === '/api/apps') return route.fulfill({ json: { status: true, apps: [], current_app: '', host_name: 'Fixture Host', host_uuid: 'fixture' } })
    if (path === '/api/stats/system') return route.fulfill({ json: { gpu, displays: [], audio: { sink: 'default' }, session_type: 'wayland' } })
    if (path === '/api/multiseat/profiles') return route.fulfill({ json: { enabled: false, available: true, profiles: [], activity: [] } })
    if (path === '/api/logs') return route.fulfill({ body: '' })
    if (path === '/api/ai/status') return route.fulfill({ json: { enabled: false } })
    if (path === '/api/ai/history' || path === '/api/ai/cache') return route.fulfill({ json: [] })
    if (path === '/api/network/connection') return route.fulfill({ status: 404, json: { status: false } })
    return route.fulfill({ json: { status: true, streaming: false } })
  })
}

const diagnosis = (page) => page.locator('[data-doctor-diagnosis]')
const grid = (page) => page.locator('[data-doctor-checklist]')
const cards = (page) => page.locator('[data-doctor-checklist] [data-readonly]')

// A screenshot kept as a file beside the trace, so the evidence survives outside the report.
async function keepScreenshot(page, testInfo, name) {
  const path = testInfo.outputPath(`${name}.png`)
  await page.screenshot({ path, animations: 'disabled' })
  await testInfo.attach(name, { path, contentType: 'image/png' })
}

async function openDoctor(page) {
  await page.goto('/#/troubleshooting')
  await expect(page.locator('[data-fix-my-stream]')).toBeVisible()
  await page.waitForFunction(() => window.__delivered > 0)
}

test('shows the current configured-host findings before a stream, under a neutral waiting headline', async ({ page }, testInfo) => {
  await mockHost(page, { stats: idle([setAside, unavailable]) })
  await openDoctor(page)

  await expect(diagnosis(page)).toHaveClass(/doctor-diagnosis-idle/)
  await expect(diagnosis(page)).toContainText('Waiting for a stream')
  await expect(page.locator('[data-ai-doctor-explanation]')).toHaveCount(0)
  await expect(cards(page)).toHaveCount(2)
  await expect(cards(page).nth(0)).toContainText('Host configuration')
  await expect(cards(page).nth(0)).toContainText('Check')
  await expect(cards(page).nth(0)).toContainText(unavailable.message)
  await expect(cards(page).nth(0)).toContainText(unavailable.action)
  await expect(cards(page).nth(1)).toContainText('Note')
  await expect(cards(page).nth(1)).toContainText(setAside.message)
  await expect(cards(page).nth(1)).toContainText(setAside.action)
  const text = await grid(page).innerText()
  for (const placeholder of PLACEHOLDERS) expect(text).not.toContain(placeholder)
  expect(text).not.toContain('No recent auth or pairing errors')
  expect(text).not.toContain('No recent warnings, fatals, or errors')
  await expect(cards(page).first()).toBeInViewport()
  await keepScreenshot(page, testInfo, 'doctor-idle-host-findings')
})

test('shows a capture finding the host has not verified as a note with its recheck step', async ({ page }, testInfo) => {
  await mockHost(page, { stats: idle([staleSubstitution]) })
  await openDoctor(page)

  await expect(diagnosis(page)).toContainText('Waiting for a stream')
  await expect(cards(page)).toHaveCount(1)
  await expect(cards(page).first()).toContainText('Note')
  await expect(cards(page).first()).toContainText(staleSubstitution.message)
  await expect(cards(page).first()).toContainText(STALE_NOTE)
  await expect(cards(page).first()).toContainText(STALE_ACTION)
  await expect(cards(page).first()).not.toContainText('set capture to the substituted backend')
  await expect(cards(page).first()).not.toContainText('Check')
  await keepScreenshot(page, testInfo, 'doctor-idle-stale-finding')
})

test('stays compact before a stream when the host has nothing to report', async ({ page }) => {
  await mockHost(page, { stats: idle([]) })
  await openDoctor(page)

  await expect(diagnosis(page)).toHaveClass(/doctor-diagnosis-idle/)
  await expect(diagnosis(page)).toContainText('Waiting for a stream')
  await expect(grid(page)).toHaveCount(0)
})

test('drops the cards with the connection, keeping the waiting headline', async ({ page }, testInfo) => {
  await mockHost(page, { stats: idle([setAside, unavailable]), stream: 'dropped' })
  await openDoctor(page)

  await expect(page.locator('text=Waiting for stream telemetry')).toBeVisible()
  await expect(diagnosis(page)).toContainText('Waiting for a stream')
  await expect(grid(page)).toHaveCount(0)
  // Nothing comes back while the host stays unreachable.
  await page.waitForTimeout(1500)
  await expect(grid(page)).toHaveCount(0)
  await keepScreenshot(page, testInfo, 'doctor-idle-disconnected')
})

test('keeps the whole checklist for a live stream', async ({ page }, testInfo) => {
  await mockHost(page, { stats: live() })
  await openDoctor(page)

  await expect(diagnosis(page)).not.toHaveClass(/doctor-diagnosis-idle/)
  await expect(diagnosis(page)).not.toContainText('Waiting for a stream')
  await expect(cards(page).first()).toContainText('Live telemetry is connected and a stream is active.')
  await expect(grid(page)).toContainText(setAside.message)
  await expect(grid(page)).toContainText('No recent auth or pairing errors')
  expect(await cards(page).count()).toBeGreaterThanOrEqual(7)
  await keepScreenshot(page, testInfo, 'doctor-live-checklist')
})
