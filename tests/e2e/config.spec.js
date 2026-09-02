import { test, expect } from './fixtures/auth.js'

test('config view loads', async ({ loggedInPage }) => {
  const nav = loggedInPage.getByRole('navigation')
  await nav.getByRole('link', { name: /^settings$/i }).click()
  await expect(loggedInPage).toHaveURL(/#\/config/)
  await expect(loggedInPage.getByRole('heading', { name: /^settings$/i })).toBeVisible({ timeout: 10000 })
})

test('apps view loads', async ({ loggedInPage }) => {
  const nav = loggedInPage.getByRole('navigation')
  await nav.getByRole('link', { name: /^library$/i }).click()
  await expect(loggedInPage).toHaveURL(/#\/apps/)
  await expect(loggedInPage.getByRole('heading', { name: /^library$/i })).toBeVisible({ timeout: 10000 })
})

test('settings metadata projection answers a session with version 1', async ({ loggedInPage }) => {
  const response = await loggedInPage.request.get('/api/settings/metadata')
  expect(response.status()).toBe(200)
  const body = await response.json()
  expect(body.status).toBe(true)
  expect(body.version).toBe(1)
  expect(Array.isArray(body.modes)).toBe(true)
  expect(typeof body.fields).toBe('object')
  expect(typeof body.stream_display).toBe('object')
  expect(typeof body.auto_quality).toBe('object')

  // The stats channel carries the same tuning and auto-quality blocks at 1 Hz.
  const stats = await loggedInPage.request.get('/api/stats/stream')
  expect(stats.status()).toBe(200)
  const statsBody = await stats.json()
  expect(typeof statsBody.tuning).toBe('object')
  expect(typeof statsBody.auto_quality).toBe('object')
})

test('settings metadata projection requires a web session', async ({ request }) => {
  const response = await request.get('/api/settings/metadata')
  expect([401, 403]).toContain(response.status())
})
