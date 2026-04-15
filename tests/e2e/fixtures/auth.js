import { test as base, expect } from '@playwright/test'

export const test = base.extend({
  loggedInPage: async ({ page }, use) => {
    const user = process.env.POLARIS_USER
    const pass = process.env.POLARIS_PASS
    if (!user || !pass) {
      test.skip(true, 'POLARIS_USER and POLARIS_PASS must be set')
    }

    await page.goto('/#/login')
    await page.locator('#usernameInput').fill(user)
    await page.locator('#passwordInput').fill(pass)
    await page.getByRole('button', { name: /sign in|login/i }).click()

    await page.waitForURL((url) => !url.hash.includes('/login'), { timeout: 10000 })
    await use(page)
  },
})

export { expect }
