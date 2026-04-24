import { test as base, expect } from '@playwright/test'

export const test = base.extend({
  loggedInPage: async ({ page }, use) => {
    const user = process.env.POLARIS_USER
    const pass = process.env.POLARIS_PASS
    if (!user || !pass) {
      test.skip(true, 'POLARIS_USER and POLARIS_PASS must be set')
    }

    await page.context().clearCookies()
    const response = await page.request.post('/api/login', {
      data: {
        username: user,
        password: pass,
      },
    })
    if (!response.ok()) {
      const body = await response.text().catch(() => '')
      throw new Error(`Login setup failed with ${response.status()}: ${body}`)
    }

    await page.goto('/#/')
    await expect(page.getByRole('navigation')).toBeVisible({ timeout: 15000 })
    await use(page)
  },
})

export { expect }
