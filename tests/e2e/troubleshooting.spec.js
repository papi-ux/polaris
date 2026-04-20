import { test, expect } from './fixtures/auth.js'

test.describe('troubleshooting', () => {
  test('log filters remain keyboard reachable', async ({ loggedInPage }) => {
    await loggedInPage.goto('/#/troubleshooting')
    await expect(loggedInPage.getByRole('heading', { name: /quick recovery/i })).toBeVisible()

    const fatalFilter = loggedInPage.getByRole('button', { name: /^fatal$/i })
    await fatalFilter.focus()
    await loggedInPage.keyboard.press('Enter')
    await expect(fatalFilter).toHaveClass(/is-active/)

    const search = loggedInPage.getByRole('textbox', { name: /find/i }).first()
    await expect(search).toBeVisible()
    await search.fill('Warning')

    const clearButton = loggedInPage.getByRole('button', { name: /^clear$/i })
    await expect(clearButton).toBeVisible()
  })
})
