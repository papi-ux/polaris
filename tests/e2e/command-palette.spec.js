import { test, expect } from './fixtures/auth.js'

test.describe('command palette', () => {
  test('opens from the keyboard and routes to pairing', async ({ loggedInPage }) => {
    await loggedInPage.goto('/#/')
    await loggedInPage.keyboard.press('Control+K')

    const dialog = loggedInPage.getByRole('dialog', { name: /command palette/i })
    await expect(dialog).toBeVisible()

    const search = dialog.getByRole('combobox', { name: /search commands/i })
    await expect(search).toBeFocused()

    await search.fill('pair')
    await loggedInPage.keyboard.press('Enter')

    await expect(loggedInPage).toHaveURL(/#\/pin/)
    await expect(loggedInPage.getByRole('heading', { name: /pair/i }).first()).toBeVisible()
  })
})
