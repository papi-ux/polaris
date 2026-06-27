import { test, expect } from './fixtures/auth.js'

async function expectHeadingVisible(page, heading) {
  const locator = page.getByRole('heading', { name: heading }).first()

  try {
    await expect(locator).toBeVisible({ timeout: 15000 })
  } catch (error) {
    await page.reload({ waitUntil: 'domcontentloaded' })
    await expect(page.getByRole('heading', { name: heading }).first()).toBeVisible({ timeout: 15000 })
  }
}

test.describe('command palette', () => {
  test.setTimeout(60000)

  test('opens from the keyboard and routes to pairing', async ({ loggedInPage }) => {
    await loggedInPage.goto('/#/')
    await loggedInPage.locator('body').focus()
    await loggedInPage.keyboard.press('Control+K')

    const dialog = loggedInPage.getByRole('dialog', { name: /command palette/i })
    await expect(dialog).toBeVisible()

    const search = dialog.getByRole('combobox', { name: /search commands/i })
    await expect(search).toBeFocused()

    await search.fill('pair')
    await loggedInPage.keyboard.press('Enter')

    await expect(loggedInPage).toHaveURL(/#\/pin/)
    await expectHeadingVisible(loggedInPage, /pair/i)
  })

  test('opens, searches, routes, and closes from the keyboard', async ({ loggedInPage }) => {
    await loggedInPage.goto('/#/')
    await loggedInPage.locator('body').focus()
    await loggedInPage.keyboard.press(process.platform === 'darwin' ? 'Meta+K' : 'Control+K')

    const dialog = loggedInPage.getByRole('dialog', { name: /command palette/i })
    await expect(dialog).toBeVisible()

    const search = dialog.getByRole('combobox', { name: /search commands/i })
    await expect(search).toBeFocused()
    await search.fill('settings')
    await loggedInPage.keyboard.press('Enter')

    await expect(loggedInPage).toHaveURL(/#\/config/)
    await expectHeadingVisible(loggedInPage, /settings/i)

    await loggedInPage.locator('body').focus()
    await loggedInPage.keyboard.press(process.platform === 'darwin' ? 'Meta+K' : 'Control+K')
    await expect(dialog).toBeVisible()
    await loggedInPage.keyboard.press('Escape')
    await expect(dialog).toBeHidden()
  })
})
