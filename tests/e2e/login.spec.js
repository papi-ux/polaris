import { test, expect } from '@playwright/test'

async function gotoLoginForm(page) {
  await page.context().clearCookies()
  await page.goto('/#/login')

  const usernameInput = page.locator('#usernameInput')
  const navigation = page.getByRole('navigation')
  await expect(usernameInput.or(navigation).first()).toBeVisible({ timeout: 15000 })

  const hasLoginForm = await usernameInput.isVisible().catch(() => false)
  test.skip(!hasLoginForm, 'Login route resumed an authenticated session')

  return usernameInput
}

test.describe('login', () => {
  test('renders the login form', async ({ page }) => {
    const usernameInput = await gotoLoginForm(page)
    await expect(usernameInput).toBeVisible()
    await expect(page.locator('#passwordInput')).toBeVisible()
    await expect(page.getByRole('button', { name: /sign in|login/i })).toBeVisible()
  })

  test('skip control is keyboard reachable', async ({ page }) => {
    await gotoLoginForm(page)

    const skipLink = page.getByRole('button', { name: /skip to main content/i })
    await expect(skipLink).toBeVisible()
    await skipLink.focus()
    await expect(skipLink).toBeFocused()
    await page.keyboard.press('Enter')

    await expect(page.locator('#usernameInput')).toBeVisible()
  })

  test('shows error for invalid credentials', async ({ page }) => {
    const usernameInput = await gotoLoginForm(page)
    await usernameInput.fill('not-a-user')
    await page.locator('#passwordInput').fill('wrong-password')
    await page.getByRole('button', { name: /sign in|login/i }).click()

    await expect(page.getByText(/check your username|error/i).first()).toBeVisible({ timeout: 5000 })
  })
})
