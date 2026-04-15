import { test, expect } from '@playwright/test'

test.describe('login', () => {
  test('renders the login form', async ({ page }) => {
    await page.goto('/#/login')
    await expect(page.locator('#usernameInput')).toBeVisible()
    await expect(page.locator('#passwordInput')).toBeVisible()
    await expect(page.getByRole('button', { name: /sign in|login/i })).toBeVisible()
  })

  test('shows error for invalid credentials', async ({ page }) => {
    await page.goto('/#/login')
    await page.locator('#usernameInput').fill('not-a-user')
    await page.locator('#passwordInput').fill('wrong-password')
    await page.getByRole('button', { name: /sign in|login/i }).click()

    await expect(page.getByText(/check your username|error/i).first()).toBeVisible({ timeout: 5000 })
  })
})
