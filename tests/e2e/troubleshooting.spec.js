import { test, expect } from './fixtures/auth.js'

test.describe('troubleshooting', () => {
  test('log filters remain keyboard reachable', async ({ loggedInPage }) => {
    await loggedInPage.getByRole('link', { name: /troubleshooting/i }).click()
    await expect(loggedInPage).toHaveURL(/#\/troubleshooting/)
    await expect(loggedInPage.getByRole('heading', { name: /^troubleshooting$/i })).toBeVisible({ timeout: 15000 })
    await expect(loggedInPage.getByRole('heading', { name: /^recovery$/i })).toBeVisible({ timeout: 15000 })

    const fatalFilter = loggedInPage.getByRole('button', { name: /^fatal$/i })
    await fatalFilter.focus()
    await loggedInPage.keyboard.press('Enter')
    await expect(fatalFilter).toHaveClass(/is-active/)

    const search = loggedInPage.getByRole('textbox', { name: /find/i }).first()
    await expect(search).toBeVisible()
    await search.fill('Warning')

    const clearButton = loggedInPage
      .locator('.troubleshooting-filter-panel')
      .getByRole('button', { name: /^clear log filters$/i })
    await expect(clearButton).toBeVisible()
  })
})
