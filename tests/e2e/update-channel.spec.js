import { test, expect } from '@playwright/test'

for (const width of [390, 1280]) {
  test(`beta opt-in persists and returns to stable at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    let preference = 'disabled'
    let revision = 'a'.repeat(64)
    const writes = []
    const stable = { tag_name: 'v1.4.12', prerelease: false, html_url: 'https://example.invalid/stable', assets: [] }
    const beta = { tag_name: 'v1.4.13-beta.3', prerelease: true, html_url: 'https://example.invalid/beta', assets: [] }
    await page.route('https://api.github.com/repos/papi-ux/polaris/releases**', route =>
      route.fulfill({ json: route.request().url().endsWith('/latest') ? stable : [beta, stable] }))
    await page.route('**/api/**', async route => {
      const request = route.request()
      const path = new URL(request.url()).pathname
      if (path === '/api/configLocale') return route.fulfill({ json: { locale: 'en' } })
      if (path === '/api/config') {
        if (request.method() === 'PATCH') {
          expect(request.headers()['if-match']).toBe(`"${revision}"`)
          const patch = request.postDataJSON()
          expect(Object.keys(patch)).toEqual(['notify_pre_releases'])
          writes.push(patch)
          preference = patch.notify_pre_releases
          revision = (preference === 'enabled' ? 'b' : 'c').repeat(64)
          return route.fulfill({ json: { status: true, configuration_revision: revision, restart_required: false } })
        }
        return route.fulfill({ json: { status: true, platform: 'linux', version: '1.4.12', notify_pre_releases: preference, configuration_revision: revision } })
      }
      if (path === '/api/update-status') return route.fulfill({ json: { version: '1.4.12', platform: 'linux', distro: { id: 'fedora', version_id: '44' } } })
      if (path === '/api/logs') return route.fulfill({ body: '' })
      expect(request.method()).toBe('GET')
      return route.fulfill({ json: { status: true, streaming: false } })
    })
    await page.goto('/#/info#update-center')
    const panel = page.locator('#update-center')
    const control = panel.getByRole('checkbox', { name: 'Include beta releases', exact: true })
    await expect(control).toBeEnabled()
    await expect(control).not.toBeChecked()
    await expect(panel).not.toContainText('Prerelease available')
    await control.click()
    await expect(control).toBeChecked()
    await expect(panel).toContainText('Prerelease available')
    await expect(panel).toContainText('v1.4.13-beta.3')
    await page.reload()
    await expect(control).toBeEnabled()
    await expect(control).toBeChecked()
    await expect(panel).toContainText('Prerelease available')
    await panel.screenshot({ path: `test-results/beta-update-${width}.png` })
    expect(await panel.evaluate(node => node.scrollWidth <= node.clientWidth + 1)).toBe(true)
    await control.focus()
    await page.keyboard.press('Space')
    await expect(control).not.toBeChecked()
    await expect(panel).toContainText('Checking stable releases only')
    await expect(panel).not.toContainText('Prerelease available')
    expect(writes).toEqual([{ notify_pre_releases: 'enabled' }, { notify_pre_releases: 'disabled' }])
  })
}
