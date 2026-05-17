import { test, expect } from './fixtures/auth.js'

test.describe('dashboard', () => {
  test('sidebar navigation renders', async ({ loggedInPage }) => {
    const sidebar = loggedInPage.locator('nav').first()
    await expect(sidebar.getByRole('link', { name: /mission control/i })).toBeVisible()
    await expect(sidebar.getByRole('link', { name: /library/i })).toBeVisible()
    await expect(sidebar.getByRole('link', { name: /settings/i })).toBeVisible()
    await expect(sidebar.getByRole('link', { name: /pairing/i })).toBeVisible()
  })

  test('metric chart cards are present when streaming', async ({ loggedInPage }) => {
    const streamStats = {
      streaming: true,
      client_name: 'QA Client',
      client_ip: '127.0.0.1',
      clients: [{ name: 'QA Client', ip: '127.0.0.1', fps: 59.8, latency_ms: 12 }],
      fps: 59.8,
      bitrate_kbps: 45000,
      encode_time_ms: 4.2,
      latency_ms: 12,
      packet_loss: 0.1,
      codec: 'av1',
      width: 1920,
      height: 1080,
      headless_mode: true,
      ai_enabled: true,
      adaptive_target_bitrate_kbps: 45000,
      runtime_backend: 'labwc',
      runtime_effective_headless: true,
      runtime_gpu_native_override_active: false,
      capture_transport: 'dmabuf',
      capture_residency: 'gpu',
      capture_format: 'bgra8',
      capture_path: 'gpu_native',
      capture_cpu_copy: false,
      capture_gpu_native: true,
      encode_target_device: 'cuda',
      encode_target_residency: 'gpu',
      encode_target_format: 'nv12',
    }

    await loggedInPage.addInitScript((payload) => {
      window.EventSource = class MockEventSource {
        constructor() {
          setTimeout(() => {
            this.onopen?.({})
            this.onmessage?.({ data: JSON.stringify(payload) })
          }, 25)
        }

        close() {}
      }
    }, streamStats)

    await loggedInPage.route('**/api/stats/system', async (route) => {
      await route.fulfill({
        json: {
          gpu: {
            temperature_c: 54,
            power_draw_w: 120,
            utilization_pct: 42,
            clock_mhz: 2100,
            encoder_pct: 16,
            vram_used_mb: 4096,
            vram_total_mb: 24576,
          },
          displays: [{ name: 'HEADLESS-1' }],
          audio: { sink: 'Polaris Virtual Sink' },
          session_type: 'streaming',
        },
      })
    })

    await loggedInPage.reload({ waitUntil: 'domcontentloaded' })

    for (const label of ['FPS', 'Bitrate', 'Encode', 'Latency', 'GPU Load', 'Packet Loss']) {
      await expect(loggedInPage.getByText(label, { exact: false }).first()).toBeVisible()
    }
    await expect(loggedInPage.getByText(/GPU Native/i).first()).toBeVisible()
  })

  test('AMD GPU with partial metrics does not render undefined or NaN', async ({ loggedInPage }) => {
    const streamStats = {
      streaming: true,
      client_name: 'QA Client',
      client_ip: '127.0.0.1',
      clients: [{ name: 'QA Client', ip: '127.0.0.1', fps: 59.8, latency_ms: 12 }],
      fps: 59.8,
      bitrate_kbps: 20000,
      encode_time_ms: 4.2,
      latency_ms: 12,
      packet_loss: 0.0,
      codec: 'hevc',
      width: 2780,
      height: 1264,
      headless_mode: true,
      ai_enabled: false,
      adaptive_target_bitrate_kbps: 20000,
      runtime_backend: 'labwc',
      runtime_effective_headless: true,
      runtime_gpu_native_override_active: false,
      capture_transport: 'dmabuf',
      capture_residency: 'gpu',
      capture_format: 'bgra8',
      capture_path: 'headless_extcopy_dmabuf',
      capture_cpu_copy: false,
      capture_gpu_native: true,
      encode_target_device: 'vaapi',
      encode_target_residency: 'gpu',
      encode_target_format: 'nv12',
    }

    // Intercept the SSE endpoint so the remounted DashboardView gets streaming stats
    // without needing a full page reload (which would reset the module-level authed flag
    // and trigger a real /api/config auth check against the server).
    await loggedInPage.route('**/api/stats/stream-sse', async (route) => {
      await route.fulfill({
        status: 200,
        contentType: 'text/event-stream',
        headers: { 'Cache-Control': 'no-cache' },
        body: `data: ${JSON.stringify(streamStats)}\n\n`,
      })
    })

    // AMD-style payload: vendor/utilization/clocks/VRAM present, temperature_c and encoder_pct absent
    await loggedInPage.route('**/api/stats/system', async (route) => {
      await route.fulfill({
        json: {
          gpu: {
            vendor: 'amd',
            name: 'AMD Radeon RX 7900 XTX',
            utilization_pct: 55,
            clock_gpu_mhz: 2400,
            vram_used_mb: 8192,
            vram_total_mb: 24576,
            // temperature_c intentionally absent
            // encoder_pct intentionally absent
            // power_draw_w intentionally absent
          },
          displays: [{ name: 'HEADLESS-1' }],
          audio: { sink: 'Polaris Virtual Sink' },
          session_type: 'streaming',
        },
      })
    })

    // Navigate away then back using hash routing — unmounts and remounts DashboardView
    // without triggering a full page reload, so the in-memory auth state is preserved.
    await loggedInPage.evaluate(() => { window.location.hash = '#/apps' })
    await loggedInPage.evaluate(() => { window.location.hash = '#/' })

    // Wait for the streaming dashboard to render with AMD GPU data
    await expect(loggedInPage.getByText('GPU Load', { exact: false }).first()).toBeVisible({ timeout: 10000 })
    await expect(loggedInPage.getByText('VRAM', { exact: false }).first()).toBeVisible({ timeout: 10000 })

    // Encoder tile must be hidden when encoder_pct is absent
    await expect(loggedInPage.locator('.dashboard-metric-tile').filter({ hasText: /^Encoder$/ })).not.toBeVisible()

    // No raw undefined or NaN values anywhere on the page
    const bodyText = await loggedInPage.locator('body').innerText()
    expect(bodyText).not.toMatch(/\bundefined\b/)
    expect(bodyText).not.toMatch(/\bNaN\b/)

    // Missing temperature should render as '--', not a raw number
    const tempTile = loggedInPage.locator('.dashboard-metric-tile').filter({ hasText: 'GPU Temp' })
    await expect(tempTile.getByText('--', { exact: true })).toBeVisible()
  })
})
