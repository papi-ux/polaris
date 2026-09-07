import { test, expect } from '@playwright/test'

const first = { id: 'a'.repeat(32), name: 'Same name', address: '192.0.2.1' }
const second = { id: 'b'.repeat(32), name: 'Same name', address: '192.0.2.2' }

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
