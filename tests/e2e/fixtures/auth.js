import { test as base, expect } from '@playwright/test'

export const test = base.extend({
  loggedInPage: async ({ page }, use) => {
    const user = process.env.POLARIS_USER
    const pass = process.env.POLARIS_PASS
    if (!user || !pass) {
      test.skip(true, 'POLARIS_USER and POLARIS_PASS must be set')
    }

    await page.context().clearCookies()
    await page.goto('/#/login')
    const nav = page.getByRole('navigation')
    const loginButton = page.getByRole('button', { name: /^login$/i })
    await expect(loginButton).toBeVisible({ timeout: 15000 })
    await page.getByRole('textbox', { name: /^username$/i }).fill(user)
    await page.getByRole('textbox', { name: /^password$/i }).fill(pass)
    await loginButton.click()
    await expect(nav).toBeVisible({ timeout: 15000 })
    await use(page)
  },
})

export { expect }
