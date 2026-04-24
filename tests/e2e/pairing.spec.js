import { test, expect } from './fixtures/auth.js'

test.describe('pairing', () => {
  test('supports keyboard switching between pairing methods', async ({ loggedInPage }) => {
    await loggedInPage.getByRole('link', { name: /pairing/i }).click()
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
    await loggedInPage.route('**/api/clients/list', async (route) => {
      await route.fulfill({
        json: {
          status: true,
          platform: 'linux',
          named_certs: [{
            name: 'qa-client',
            friendly_name: 'QA Client',
            client_family: 'nova',
            uuid: '11111111-2222-4333-8444-555555555555',
            display_mode: '',
            perm: '50331648',
            connected: false,
            do: [],
            undo: [],
            allow_client_commands: false,
            always_use_virtual_display: false,
            enable_legacy_ordering: true,
          }],
        },
      })
    })
    await loggedInPage.route('**/api/clients/profiles', async (route) => {
      await route.fulfill({ json: {} })
    })
    await loggedInPage.route('**/api/devices/suggest?**', async (route) => {
      await route.fulfill({ json: { status: false } })
    })

    await loggedInPage.getByRole('link', { name: /pairing/i }).click()

    const editButtons = loggedInPage.getByRole('button', { name: /edit access/i })
    await editButtons.first().click()
    const dialog = loggedInPage.getByRole('dialog')
    await expect(dialog).toBeVisible()

    await loggedInPage.keyboard.press('Escape')
    await expect(dialog).toBeHidden()
  })
})
