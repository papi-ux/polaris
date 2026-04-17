import { test, expect } from './fixtures/auth.js'

test.describe('dashboard', () => {
  test('sidebar navigation renders', async ({ loggedInPage }) => {
    const sidebar = loggedInPage.locator('nav').first()
    await expect(sidebar.getByRole('link', { name: /mission control/i })).toBeVisible()
    await expect(sidebar.getByRole('link', { name: /applications/i })).toBeVisible()
    await expect(sidebar.getByRole('link', { name: /configuration/i })).toBeVisible()
    await expect(sidebar.getByRole('link', { name: /pin/i })).toBeVisible()
  })

  test('metric chart cards are present when streaming', async ({ loggedInPage }) => {
    const statsRes = await loggedInPage.request.get('/api/stats')
    test.skip(statsRes.status() !== 200, 'stats endpoint unavailable')

    const stats = await statsRes.json()
    test.skip(!stats.streaming, 'no active stream — chart grid is hidden when idle')

    for (const label of ['FPS', 'Bitrate', 'Encode', 'Latency', 'GPU Load', 'Packet Loss']) {
      await expect(loggedInPage.getByText(label, { exact: false }).first()).toBeVisible()
    }
  })
})
