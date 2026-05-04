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
})
