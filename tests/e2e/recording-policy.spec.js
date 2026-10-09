import { test, expect } from './fixtures/mocked.js'
import { readFileSync } from 'node:fs'

// The rendered English strings, so a locator can never drift from the real label.
const en = JSON.parse(readFileSync(new URL('../../src_assets/common/assets/web/public/assets/locale/en.json', import.meta.url), 'utf8'))

const DEVICE = '11111111-2222-4333-8444-555555555555'
const GAME_CONTROL = 0x07001F00
const FULL_CONTROL = 0x071F1F00

/**
 * Fulfills every host API request and records it. Only the one mutation each case is about may be
 * a POST, and nothing may reach the recorder routes.
 */
async function mockHost(page, { allowedPost }) {
  const requests = []
  const posts = []
  const profilePosts = []
  const client = {
    name: 'handheld', friendly_name: 'Handheld', client_family: 'nova', uuid: DEVICE, display_mode: '',
    perm: String(GAME_CONTROL), connected: false, do: [], undo: [], allow_client_commands: true,
    always_use_virtual_display: false, close_desktop_steam: false, enable_legacy_ordering: true,
    temporary_authorization: false, recording_authorized: false,
  }
  await page.route('**/api/**', async (route) => {
    const request = route.request()
    const path = new URL(request.url()).pathname
    requests.push(`${request.method()} ${path}`)
    if (request.method() !== 'GET') {
      // PinView saves the existing display profile after a device edit. Account for that
      // established write explicitly; an unrelated mutation must fail this case.
      if (allowedPost === '/api/clients/update' && path === '/api/clients/profiles/update') {
        expect(request.method()).toBe('POST')
        profilePosts.push(request.postDataJSON())
        return route.fulfill({ json: { status: true } })
      }
      expect(path).toBe(allowedPost)
      expect(request.method()).toBe('POST')
      posts.push({ path, body: request.postDataJSON() })
      if (path === '/api/config') return route.fulfill({ json: { status: true, configuration_revision: 'b'.repeat(64) } })
      Object.assign(client, request.postDataJSON())
      return route.fulfill({ json: { status: true } })
    }
    if (path === '/api/configLocale') return route.fulfill({ json: { locale: 'en' } })
    if (path === '/api/config') return route.fulfill({ json: { status: true, platform: 'linux',
      version: '1.4.14-beta.2', configuration_revision: 'a'.repeat(64), sunshine_name: 'Host' } })
    if (path === '/api/settings/metadata') return route.fulfill({ status: 404, json: { status: false } })
    if (path === '/api/clients/list') return route.fulfill({ json: { status: true, platform: 'linux', named_certs: [client] } })
    if (path === '/api/clients/profiles') return route.fulfill({ json: {} })
    if (path.startsWith('/api/devices/suggest')) return route.fulfill({ json: { status: false } })
    if (path === '/api/multiseat/profiles') return route.fulfill({ json: { enabled: false, available: true,
      changing: false, failed: false, profiles: [], activity: [] } })
    if (path === '/api/apps') return route.fulfill({ json: { status: true, apps: [], current_app: '' } })
    if (path === '/api/logs') return route.fulfill({ body: '' })
    return route.fulfill({ json: { status: true } })
  })
  return { requests, posts, profilePosts }
}

function expectNoRecorderRequest(requests) {
  expect(requests.filter((request) => request.includes('/api/recording'))).toEqual([])
}

async function fits(locator, width) {
  const box = await locator.boundingBox()
  expect(box).not.toBeNull()
  expect(box.x).toBeGreaterThanOrEqual(0)
  expect(box.x + box.width).toBeLessThanOrEqual(width + 1)
}

for (const width of [390, 1280]) {
  test(`recording policy saves only settings and never calls the recorder at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    const host = await mockHost(page, { allowedPost: '/api/config' })
    await page.goto('/#/config#recording_enabled')
    const tab = page.locator('#recording')
    await expect(tab).toBeVisible()
    await expect(page.locator('#recording_policy_off')).toBeChecked()
    await expect(page.locator('[data-recording-format-notice]')).toHaveText(en.config.recording_format_notice)
    await expect(tab.getByRole('button')).toHaveCount(0)

    await page.locator('#recording_policy_manual').check()
    await expect(page.locator('#recording_policy_manual')).toBeChecked()
    await expect(page.locator('[data-pending-change="recording_enabled"]')).toBeVisible()
    expect(host.posts).toEqual([])
    expectNoRecorderRequest(host.requests)

    await page.getByRole('button', { name: en._common.save, exact: true }).first().click()
    await expect.poll(() => host.posts.length).toBe(1)
    expect(host.posts[0].path).toBe('/api/config')
    expect(host.posts[0].body.recording_enabled).toBe('enabled')
    expect(host.posts[0].body).not.toHaveProperty('recording_replay_buffer')
    expectNoRecorderRequest(host.requests)
    await fits(tab, width)
    await tab.screenshot({ path: `test-results/polaris-recording-policy-${width}.png` })
  })

  test(`device recording approval is separate from presets, cleared for a guest, and reset on cancel at ${width}px`, async ({ page }) => {
    await page.setViewportSize({ width, height: 960 })
    const host = await mockHost(page, { allowedPost: '/api/clients/update' })
    await page.goto('/#/pin')
    await page.getByRole('button', { name: en.pin.edit_access, exact: true }).first().click()
    const dialog = page.getByRole('dialog')
    await expect(dialog).toBeVisible()
    const approval = dialog.locator(`#recording-authorized-${DEVICE}`)
    const guest = dialog.locator(`#temporary-authorization-${DEVICE}`)
    await expect(approval).not.toBeChecked()
    await expect(approval).toBeEnabled()

    // A preset changes permissions only.
    await approval.check()
    await dialog.getByRole('button', { name: en.pin.full_control, exact: true }).click()
    await expect(approval).toBeChecked()

    // A guest cannot hold it, and becoming permanent again does not restore it.
    await guest.check()
    await expect(approval).toBeDisabled()
    await expect(approval).not.toBeChecked()
    await expect(dialog.locator('[data-recording-approval-guest]')).toHaveText(en.pin.recording_authorized_guest)
    await guest.uncheck()
    await expect(approval).toBeEnabled()
    await expect(approval).not.toBeChecked()

    // Cancel puts the stored choice back.
    await approval.check()
    await dialog.getByRole('button', { name: en._common.cancel, exact: true }).click()
    await expect(dialog).toBeHidden()
    await page.getByRole('button', { name: en.pin.edit_access, exact: true }).first().click()
    await expect(page.getByRole('dialog').locator(`#recording-authorized-${DEVICE}`)).not.toBeChecked()
    expect(host.posts).toEqual([])

    // Save sends the approval beside the permissions the editor holds.
    await page.getByRole('dialog').locator(`#recording-authorized-${DEVICE}`).check()
    await fits(page.getByRole('dialog').locator('[data-recording-approval]'), width)
    await page.getByRole('dialog').locator('[data-recording-approval]').screenshot({ path: `test-results/polaris-recording-approval-${width}.png` })
    await page.getByRole('dialog').getByRole('button', { name: en._common.save, exact: true }).click()
    await expect.poll(() => host.posts.length).toBe(1)
    expect(host.posts[0].path).toBe('/api/clients/update')
    expect(host.posts[0].body).toMatchObject({ uuid: DEVICE, recording_authorized: true, temporary_authorization: false })
    expect([GAME_CONTROL, FULL_CONTROL]).toContain(host.posts[0].body.perm)
    await expect.poll(() => host.profilePosts.length).toBe(1)
    expect(host.profilePosts[0]).toEqual({ name: 'handheld', output_name: '', color_range: 0, hdr: null, mac_address: '' })
    await expect(page.getByRole('dialog')).toBeHidden()
    await page.getByRole('button', { name: en.pin.edit_access, exact: true }).first().click()
    await expect(page.getByRole('dialog').locator(`#recording-authorized-${DEVICE}`)).toBeChecked()
    expectNoRecorderRequest(host.requests)
  })
}
