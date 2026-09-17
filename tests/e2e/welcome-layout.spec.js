import { test, expect } from '@playwright/test'

const viewports = [
  { width: 320, height: 900 },
  { width: 768, height: 1024 },
  { width: 1207, height: 1443 },
  { width: 1280, height: 900 },
  { width: 1440, height: 900 },
  { width: 1920, height: 1080 },
]

// The longest copy each new step can show: an AMD card on stock Fedora Mesa with the fix
// commands, every launch mode, and a detected network.
const hardware = {
  status: true,
  platform: 'linux',
  gpus: [{
    render_node: '/dev/dri/renderD128',
    vendor: 'amd',
    model: 'Navi 31 [Radeon RX 7900 XT/7900 XTX/7900 GRE/7900M]',
    driver: 'amdgpu',
    driver_version: '',
    selected: true,
    vaapi: {
      driver_loaded: true,
      driver_vendor: 'Mesa Gallium driver 25.1.9 for AMD Radeon RX 7900 XTX (radeonsi, navi31, LLVM 20.1.8, DRM 3.61, 6.16.7-200.fc42.x86_64)',
      h264: false,
      hevc: false,
      av1: true,
    },
  }],
  build: { cuda: true, vaapi: true },
  encoder: { configured: '', policy: 'amd_established_desktop', planned: 'vaapi', active: '', expected: 'software' },
  encoder_choices: [],
  nvenc_min_driver: '570',
  advice: [
    {
      code: 'amd_vaapi_encode_missing_fedora',
      severity: 'fail',
      render_node: '/dev/dri/renderD128',
      commands: [
        'sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm',
        'sudo dnf swap mesa-va-drivers mesa-va-drivers-freeworld',
      ],
      params: {},
    },
    { code: 'vaapi_av1_only', severity: 'info', render_node: '/dev/dri/renderD128', commands: [], params: {} },
    { code: 'encoder_confirmed_at_first_stream', severity: 'info', render_node: '', commands: [], params: {} },
  ],
}

const streamModes = ['headless_stream', 'windowed_stream', 'gamescope_stream', 'host_virtual_display', 'desktop_takeover', 'headless_dongle', 'desktop_display']
  .map((value) => ({ value, available: value !== 'desktop_takeover', unavailable_reason: 'Desktop Takeover needs a Hyprland session.' }))

test.beforeEach(async ({ page }) => {
  let credentialsSaved = false
  // Exercise the real wizard without reading or changing a host's credentials.
  await page.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname
    if (path === '/api/configLocale') return route.fulfill({ json: { locale: 'en' } })
    if (path === '/api/password') {
      credentialsSaved = true
      return route.fulfill({ json: { status: true } })
    }
    if (path === '/api/config') {
      if (!credentialsSaved) {
        return route.fulfill({ status: 302, headers: { location: '/welcome' } })
      }
      return route.fulfill({
        json: {
          status: true,
          platform: 'linux',
          encoder: '',
          linux_stream_mode: 'headless_stream',
          stream_display_mode_options: streamModes,
          trusted_subnets: '192.168.50.0/24',
          trusted_subnet_auto_pairing: 'enabled',
        },
      })
    }
    if (path === '/api/setup/hardware') return route.fulfill({ json: hardware })
    if (path === '/api/setup/networks') {
      return route.fulfill({
        json: { status: true, supported: true, networks: [{ interface: 'enp5s0', interfaces: ['enp5s0', 'wlp4s0'], cidr: '192.168.1.0/24', address: '192.168.1.20' }] },
      })
    }
    return route.fulfill({ status: 404 })
  })
})

async function expectReadableLayout(page, viewport) {
  const panel = page.locator('section.glass')
  const logo = await panel.getByRole('img', { name: 'Polaris', exact: true }).boundingBox()
  const heading = await panel.getByRole('heading', { level: 1 }).boundingBox()
  expect(logo.width, 'wordmark should leave room for the welcome heading').toBeLessThanOrEqual(240)
  expect(logo.y + logo.height, 'wordmark should sit above the heading').toBeLessThanOrEqual(heading.y)

  const panelBox = await panel.boundingBox()
  const forward = panel.getByRole('button', { name: /^(Next|Finish Setup)$/ })
  const forwardBox = await forward.boundingBox()
  expect(panelBox.y + panelBox.height - forwardBox.y - forwardBox.height,
    'main panel should end after navigation instead of stretching to the sidebar').toBeLessThanOrEqual(34)

  for (const title of ['Resources', 'Legal']) {
    const card = page.locator('section').filter({ has: page.getByRole('heading', { name: title, exact: true }) })
    const copy = await card.locator('p').boundingBox()
    const links = card.getByRole('link')
    for (const link of await links.all()) {
      const box = await link.boundingBox()
      expect(box.y, `${title} links should leave the description its own row`).toBeGreaterThanOrEqual(copy.y + copy.height + 12)
    }
  }

  // Check actual rendered boxes; overflow-hidden on the page can hide broken sizing.
  const clipped = await page.locator('section input, section a, section button, section img, section h1, section h2, section p').evaluateAll((elements, width) => {
    return elements.filter((element) => {
      const box = element.getBoundingClientRect()
      // A filled input scrolls its value horizontally by design; its box must still fit.
      const textClipped = element.tagName !== 'INPUT' && (
        element.scrollWidth > element.clientWidth + 1
        || element.scrollHeight > element.clientHeight + 1)
      return box.width > 0 && (box.left < -1 || box.right > width + 1
        || textClipped)
    }).map((element) => element.textContent.trim() || element.tagName)
  }, viewport.width)
  expect(clipped, 'text and controls should stay readable within the viewport').toEqual([])
}

for (const viewport of viewports) {
  test(`welcome stays readable at ${viewport.width}px`, async ({ page }, testInfo) => {
    await page.setViewportSize(viewport)
    await page.goto('/#/welcome')
    await page.getByLabel('Password', { exact: true }).fill('layout-test-password')
    await page.getByLabel('Confirm password', { exact: true }).fill('layout-test-password')
    await page.getByRole('button', { name: 'Save Credentials', exact: true }).click()
    const next = page.getByRole('button', { name: 'Next', exact: true })
    await expect(next).toBeEnabled()
    // The three steps with the most host-provided copy get a screenshot of their own.
    const shots = { 1: 'gpu-and-encoder', 2: 'launch-mode', 3: 'network' }
    for (let step = 1; step <= 7; step++) {
      await next.click()
      if (shots[step]) {
        await expect(page.locator(`[data-encoder-summary], [data-mode], [data-trusted-network]`).first()).toBeVisible()
        await page.evaluate(() => document.fonts.ready)
        await page.screenshot({ path: testInfo.outputPath(`${shots[step]}.png`), fullPage: true, animations: 'disabled' })
        await expectReadableLayout(page, viewport)
      }
    }
    // First App is last: its forward button finishes instead of going on.
    await expect(page.getByRole('heading', { name: 'First App', exact: true })).toBeVisible()
    await expect(page.getByRole('button', { name: 'Finish Setup', exact: true })).toBeVisible()
    await page.evaluate(() => document.fonts.ready)
    await page.screenshot({ path: testInfo.outputPath('first-app.png'), fullPage: true, animations: 'disabled' })
    await expectReadableLayout(page, viewport)

    // Cover every step, including the longer credentials and network panels.
    for (let step = 6; step >= 0; step--) {
      await page.getByRole('button', { name: 'Back', exact: true }).click()
      await expectReadableLayout(page, viewport)
    }
  })
}
