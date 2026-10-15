import { test, expect } from './fixtures/mocked.js'

const vrr = { schema: 1, stream_instance_id: 'stream-1', requested: 'true', effective: true,
  pacing_mode: 'source_time', fallback_reason: null, host_permission: true, host_pacing: { state: 'source_time', fresh: true } }
const live = { streaming: true, fps: 90, session_target_fps: 90, width: 1920, height: 1080,
  bitrate_kbps: 140000, codec: 'hevc', encoder_backend: 'nvenc', capture_path: 'cpu_copy', capture_gpu_native: false,
  capture_cpu_copy: true, capture_transport: 'shm', capture_path_reason: 'headless_shm_default',
  stream_display_mode: 'Private Stream', runtime_backend: 'labwc', runtime_effective_headless: true,
  encode_time_ms: 2, latency_ms: 4, clients: [{ name: 'Pixel', codec: 'hevc', stream_instance_id: 'stream-1',
    vrr, pyrowave_route: 'gpu_upload' }],
  pyrowave_bitrate: { width: 1920, height: 1080, fps: 90, chroma: '444', request_kbps: 140000, encoder_kbps: 125000 },
  doctor: { primary_issue: 'none', traffic_light: 'green', summary: 'Stream is running' } }

async function mockHost(page, { stats = live, systemMissing = false, mode = 'headless_stream' } = {}) {
  const writes = []
  await page.addInitScript(initial => {
    window.__mockStats = initial
    window.__holdStats = false
    window.EventSource = class {
      constructor() {
        this.timer = setInterval(() => {
          if (!window.__holdStats) { this.onopen?.({}); this.onmessage?.({ data: JSON.stringify(window.__mockStats) }) }
        }, 250)
      }
      close() { clearInterval(this.timer) }
    }
  }, stats)
  let config = { status: true, platform: 'linux', version: '1.4.14-beta.2', configuration_revision: 'a'.repeat(64),
    linux_stream_mode: mode, headless_mode: 'enabled', linux_use_cage_compositor: 'enabled',
    linux_prefer_gpu_native_capture: 'disabled', vrr_source_pacing: 'disabled', vdisplayAvailable: false,
    vdisplayBackend: 'kscreen-doctor', stream_audio: 'enabled',
    stream_display_mode_options: [
      { value: 'headless_stream', available: true }, { value: 'windowed_stream', available: true },
      { value: 'desktop_display', available: true }, { value: 'host_virtual_display', available: false,
        unavailable_reason: 'KWin did not offer a capture protocol for this output.' },
      { value: 'desktop_takeover', available: false }, { value: 'headless_dongle', available: false },
      { value: 'gamescope_stream', available: false },
    ],
    encoder_codec_support: { ready: true, encoder: 'nvenc', hevc_supported: true, av1_supported: true,
      pyrowave: { available: true, hdr: false } } }
  await page.route('**/api/**', async route => {
    const request = route.request(), path = new URL(request.url()).pathname
    if (path === '/api/configLocale') return route.fulfill({ json: { locale: 'en' } })
    if (path === '/api/config') {
      if (request.method() === 'POST') {
        const body = request.postDataJSON(); writes.push(body)
        config = { ...config, ...body, configuration_revision: 'b'.repeat(64) }
        return route.fulfill({ json: { status: true, configuration_revision: config.configuration_revision, restart_required: true } })
      }
      return route.fulfill({ json: config })
    }
    if (path === '/api/settings/metadata') return route.fulfill({ status: 404, json: { status: false } })
    if (path === '/api/stats/system') return route.fulfill({ json: systemMissing ? {} : { displays: [], audio: { sink: 'default' } } })
    if (path === '/api/stats/stream') return route.fulfill({ json: stats })
    if (path === '/api/apps') return route.fulfill({ json: { status: true, current_app: '', apps: [{ uuid: 'control', name: 'Control' }] } })
    if (path === '/api/clients/list') return route.fulfill({ json: { status: true, named_certs: [{ name: 'Pixel', uuid: 'pixel' }] } })
    if (path === '/api/multiseat/profiles') return route.fulfill({ json: { enabled: false, available: true, profiles: [], activity: [] } })
    if (path === '/api/logs') return route.fulfill({ body: '' })
    if (path === '/api/ai/history' || path === '/api/ai/cache') return route.fulfill({ json: [] })
    // An unexpected mutation must never become a live host action or pass silently.
    expect(request.method()).toBe('GET')
    return route.fulfill({ json: { status: true, streaming: false } })
  })
  return writes
}

async function fits(page, locator, width) {
  const box = await locator.boundingBox()
  expect(box.x).toBeGreaterThanOrEqual(0)
  expect(box.x + box.width).toBeLessThanOrEqual(width + 1)
  expect(await locator.evaluate(node => node.scrollWidth <= node.clientWidth + 1)).toBe(true)
}

for (const width of [390, 1280]) {
  test(`pacing permission, save impact and available launch alternative at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    const writes = await mockHost(page, { mode: 'desktop_display' })
    await page.goto('/#/config#vrr_source_pacing')
    const pacing = page.locator('[data-stream-pacing-settings]')
    const control = pacing.getByRole('checkbox', { name: /^Allow source-paced VRR \(experimental\)/ })
    await expect(control).not.toBeChecked()
    await expect(pacing).toContainText('does not verify VRR on the client display')
    await control.check()
    await page.getByRole('button', { name: 'Save', exact: true }).click()
    await expect(page.locator('[data-save-badge]')).toHaveText('Saved · restart needed')
    expect(writes.at(-1).vrr_source_pacing).toBe('enabled')
    await expect(page.locator('[data-restart-disclosure]')).toContainText('disconnects any active streams')
    await expect(page.getByRole('button', { name: 'Save and restart Polaris', exact: true })).toBeEnabled()
    const virtual = page.locator('[data-stream-display-mode-picker] article').filter({ has: page.getByText('Host Virtual Display', { exact: true }) })
    await expect(virtual).toContainText('kscreen-doctor')
    await expect(virtual).toContainText('this provider does not add a new display')
    await expect(virtual.getByRole('button', { name: /Host Virtual Display/ })).toBeDisabled()
    await expect(page.locator('[data-selected-stream-path]')).toContainText('Mirror Desktop')
    await expect(virtual.locator('[data-launch-mode-alternative]')).toBeEnabled()
    await virtual.locator('[data-launch-mode-alternative]').click()
    await expect(page.locator('[data-selected-stream-path]')).toContainText('Private Stream')
    await fits(page, pacing, width)
    await fits(page, page.locator('.settings-command-bar'), width)
    await pacing.screenshot({ path: `test-results/polaris-pacing-settings-${width}.png` })
    await virtual.screenshot({ path: `test-results/polaris-launch-provider-${width}.png` })
    await page.locator('.settings-command-bar').screenshot({ path: `test-results/polaris-save-restart-${width}.png` })
  })

  test(`PyroWave stays visible across encoder profiles at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    await mockHost(page, { stats: { ...live, codec: 'pyrowave', encoder_backend: 'pyrowave', clients: [{ ...live.clients[0], codec: 'pyrowave', vrr: { ...vrr, effective: false, pacing_mode: 'fixed_grid', fallback_reason: 'unsupported_codec' } }] } })
    await page.goto('/#/config#pyrowave')
    const card = page.locator('[data-pyrowave-card]')
    await expect(card).toHaveCount(1)
    await expect(card).toContainText('Nova chooses')
    await expect(card.locator('[data-pyrowave-value="requested"]')).toContainText('140')
    await expect(card.locator('[data-pyrowave-value="encoded"]')).toContainText('125')
    const switcher = page.locator('.settings-subnav-row')
    await switcher.getByRole('button', { name: /VA-API/ }).click()
    await expect(card).toHaveCount(1)
    await switcher.getByRole('button', { name: /Software/ }).click()
    await expect(card).toHaveCount(1)
    await expect(card.locator('input, select, button')).toHaveCount(0)
    await fits(page, card, width)
    await card.screenshot({ path: `test-results/polaris-pyrowave-${width}.png` })
  })

  test(`expected copies stay neutral and old pacing expires at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    await mockHost(page)
    await page.goto('/#/')
    // Issue #408 moved stream pacing into Mission Control's Diagnostics disclosure; open it first.
    await page.getByRole('button', { name: 'Diagnostics', exact: true }).click()
    const pacing = page.locator('[data-stream-pacing-readout]')
    await expect(pacing).toContainText('Source-paced')
    const capture = page.locator('.meta-pill').filter({ hasText: /^CPU Copy$/ }).first()
    await expect(capture).toHaveClass(/text-storm/)
    await expect(capture).not.toHaveClass(/text-warning/)
    await fits(page, pacing, width)
    await pacing.screenshot({ path: `test-results/polaris-pacing-live-${width}.png` })
    await page.evaluate(() => { window.__holdStats = true })
    await expect(pacing).not.toContainText('Source-paced', { timeout: 4000 })
    await expect(pacing).toContainText('Not verified')
    await page.evaluate(() => { window.__holdStats = false; window.__mockStats.clients[0].vrr = { ...window.__mockStats.clients[0].vrr, host_permission: false, effective: false, pacing_mode: 'fixed_grid', fallback_reason: 'route_unreported' } })
    await expect(pacing).toContainText('Source pacing was not allowed when this stream started')
    await expect(pacing).toContainText('Fixed-rate')
  })

  test(`idle and missing data stay neutral but failed starts remain visible at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    const idle = { streaming: false, doctor: { primary_issue: 'no_active_stream', status: 'unknown', severity: 'info', simple_state: 'Needs attention' } }
    await mockHost(page, { stats: idle, systemMissing: true })
    await page.goto('/#/')
    await expect(page.locator('[data-dashboard-idle-hero]')).toContainText('Waiting for host checks')
    await expect(page.locator('[data-dashboard-idle-hero]')).not.toContainText('ATTENTION')
    await page.goto('/#/troubleshooting')
    const doctor = page.locator('[data-doctor-diagnosis]')
    await expect(doctor).toContainText('Waiting for a stream')
    await expect(page.locator('[data-doctor-checklist]')).toHaveCount(0)
    await doctor.screenshot({ path: `test-results/polaris-doctor-idle-${width}.png` })
    await page.evaluate(() => { window.__mockStats = { streaming: false, doctor: { primary_issue: 'stream_failed_to_start', status: 'error', severity: 'warning', simple_state: 'Stream failed to start', summary: 'Capture did not start' } } })
    await expect(doctor).toContainText('Stream failed to start')
    await expect(doctor).not.toContainText('Waiting for a stream')
  })
}
