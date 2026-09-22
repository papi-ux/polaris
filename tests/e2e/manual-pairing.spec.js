import { test, expect } from '@playwright/test'

const first = { id: 'a'.repeat(32), name: 'Same name', address: '192.0.2.1' }
const second = { id: 'b'.repeat(32), name: 'Same name', address: '192.0.2.2' }

test('profile assignments preserve the confirmed route when an active stream refuses a save', async ({ page }, testInfo) => {
  let assignment = null
  const clients = [
    { uuid: 'device-a', name: 'Living room handheld', perm: 0x07001F00, temporary_authorization: false },
    { uuid: 'device-b', name: 'Bedroom TV', perm: 0x07001F00, temporary_authorization: false },
  ]
  await page.route('**/api/**', async route => {
    const path = new URL(route.request().url()).pathname
    if (path === '/api/multiseat/profiles') return route.fulfill({ json: {
      enabled: true, available: true, changing: false, failed: false,
      // Each device may open both Spaces, so each has a Default Space to choose.
      profiles: [{ id: 'profile-a', name: 'Alex', clients: ['device-a'], access_clients: ['device-b'] },
        { id: 'profile-b', name: 'Sam', clients: ['device-b'], access_clients: ['device-a'] }],
    } })
    if (path === '/api/multiseat/assign') {
      assignment = route.request().postDataJSON()
      return route.fulfill({ status: 409, json: { status: false,
        message: 'Stop every Space stream and wait for cleanup before changing Spaces.' } })
    }
    if (path === '/api/spaces/setup') return route.fulfill({ status: 404, json: {} })
    if (path === '/api/clients/list') return route.fulfill({ json: { status: true, platform: 'linux', named_certs: clients } })
    if (path === '/api/pin') return route.fulfill({ json: { pairings: [] } })
    if (path === '/api/config') return route.fulfill({ json: { status: true, platform: 'linux' } })
    return route.fulfill({ json: {} })
  })
  await page.goto('/#/spaces')
  const panel = page.getByRole('region', { name: 'Your Spaces' })
  await expect(panel).toBeVisible()
  // The device name labels its Default Space dropdown in the Device Access table.
  await panel.getByLabel('Living room handheld', { exact: true }).selectOption('profile-b')
  await panel.getByRole('button', { name: 'Save assignment for Living room handheld' }).click()
  await expect.poll(() => assignment).toEqual({ client_id: 'device-a', profile_id: 'profile-b' })
  await expect(panel.getByRole('alert')).toContainText('Stop every Space stream')
  await expect(panel.getByLabel('Living room handheld', { exact: true })).toHaveValue('profile-a')
  await expect(panel.getByLabel('Bedroom TV', { exact: true })).toHaveValue('profile-b')
  await panel.screenshot({ path: testInfo.outputPath('profile-assignments-desktop.png') })
  await page.setViewportSize({ width: 390, height: 844 })
  await expect(panel).toBeVisible()
  expect(await panel.evaluate(node => node.scrollWidth <= node.clientWidth)).toBe(true)
  await panel.screenshot({ path: testInfo.outputPath('profile-assignments-mobile.png') })
})

test('approves only the explicitly selected request and preserves access settings', async ({ page }) => {
  let requests = [first, second]
  let approval = null
  let cancellation = null
  await page.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname
    if (path === '/api/pin') {
      if (route.request().method() === 'GET') return route.fulfill({ json: { pairings: requests } })
      const body = route.request().postDataJSON()
      if (route.request().method() === 'POST') approval = body
      if (route.request().method() === 'DELETE') cancellation = body
      requests = requests.filter((request) => request.id !== body.pairing_id)
      return route.fulfill({ json: { status: true } })
    }
    if (path === '/api/clients/list') return route.fulfill({ json: { status: true, platform: 'linux', named_certs: [] } })
    if (path === '/api/config') return route.fulfill({ json: { status: true, platform: 'linux' } })
    return route.fulfill({ json: {} })
  })
  await page.goto('/#/pin?method=PIN')
  const send = page.getByRole('button', { name: /^send$/i })
  await expect(send).toBeDisabled()
  await expect(page.locator('#pairing-request option')).toHaveCount(3)
  await page.locator('#pairing-request').selectOption(second.id)
  await page.locator('#pin-input').fill('1234')
  await page.locator('#name-input').fill('Approved client')
  await page.locator('#temporary-pairing-authorization').check()
  await send.click()
  await expect.poll(() => approval).toMatchObject({
    pairing_id: second.id, pin: '1234', name: 'Approved client',
    access_preset: 'game_control', temporary_authorization: true,
  })
  await expect(send).toBeDisabled()
  await page.locator('#pairing-request').selectOption(first.id)
  await page.getByRole('button', { name: 'Cancel request', exact: true }).click()
  await expect.poll(() => cancellation).toEqual({ pairing_id: first.id })
  await expect(send).toBeDisabled()
  await expect(page.getByText('Start pairing on your client to create a request.')).toBeVisible()
})
