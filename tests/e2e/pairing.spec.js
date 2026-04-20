import { test, expect } from './fixtures/auth.js'

test.describe('pairing', () => {
  test('supports keyboard switching between pairing methods', async ({ loggedInPage }) => {
    await loggedInPage.goto('/#/pin')
    await expect(loggedInPage.getByRole('heading', { name: /pair/i }).first()).toBeVisible()

    const manualPinButton = loggedInPage.getByRole('button', { name: /manual pin/i })
    await manualPinButton.focus()
    await loggedInPage.keyboard.press('Enter')

    await expect(loggedInPage.locator('#pin-input')).toBeVisible()

    const qrButton = loggedInPage.getByRole('button', { name: /nova|qr/i }).first()
    await qrButton.focus()
    await loggedInPage.keyboard.press('Enter')

    await expect(loggedInPage.getByText(/qr|passphrase/i).first()).toBeVisible()
  })

  test('paired client editor closes with escape', async ({ loggedInPage }) => {
    await loggedInPage.goto('/#/pin')

    const editButtons = loggedInPage.getByRole('button', { name: /edit access/i })
    test.skip((await editButtons.count()) === 0, 'no paired clients available')

    await editButtons.first().click()
    const dialog = loggedInPage.getByRole('dialog')
    await expect(dialog).toBeVisible()

    await loggedInPage.keyboard.press('Escape')
    await expect(dialog).toBeHidden()
  })
})
