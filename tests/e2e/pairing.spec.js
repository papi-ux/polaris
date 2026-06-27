import { test, expect } from './fixtures/auth.js'

test.describe('pairing', () => {
  test.beforeEach(async ({ loggedInPage }) => {
    await loggedInPage.route('**/api/clients/profiles', async (route) => {
      await route.fulfill({ json: {} })
    })
    await loggedInPage.route('**/api/devices/suggest?**', async (route) => {
      await route.fulfill({ json: { status: false } })
    })
  })

  test('supports keyboard switching between pairing methods', async ({ loggedInPage }) => {
    await loggedInPage.getByRole('navigation').getByRole('link', { name: /^pairing$/i }).click()
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

    await loggedInPage.getByRole('navigation').getByRole('link', { name: /^pairing$/i }).click()

    const editButtons = loggedInPage.getByRole('button', { name: /edit access/i })
    await editButtons.first().click()
    const dialog = loggedInPage.getByRole('dialog')
    await expect(dialog).toBeVisible()

    await loggedInPage.keyboard.press('Escape')
    await expect(dialog).toBeHidden()
  })

  test('QR pairing requests Standard Access by default', async ({ loggedInPage }) => {
    let payload = null
    await loggedInPage.route('**/api/otp', async (route) => {
      payload = route.request().postDataJSON()
      await route.fulfill({
        json: {
          status: true,
          otp: '1234',
          ip: '10.0.0.2',
          name: 'Polaris',
          access_preset: 'standard',
          message: 'OTP created',
        },
      })
    })

    await loggedInPage.getByRole('navigation').getByRole('link', { name: /^pairing$/i }).click()
    await loggedInPage.locator('#otp-passphrase').fill('abcd')
    await loggedInPage.getByRole('button', { name: /generate qr/i }).click()

    await expect.poll(() => payload?.access_preset).toBe('standard')
  })

  test('QR pairing sends Game Control when selected', async ({ loggedInPage }) => {
    let payload = null
    await loggedInPage.route('**/api/otp', async (route) => {
      payload = route.request().postDataJSON()
      await route.fulfill({
        json: {
          status: true,
          otp: '1234',
          ip: '10.0.0.2',
          name: 'Polaris',
          access_preset: 'game_control',
          message: 'OTP created',
        },
      })
    })

    await loggedInPage.getByRole('navigation').getByRole('link', { name: /^pairing$/i }).click()
    await loggedInPage.getByRole('radio', { name: /game control/i }).click()
    await loggedInPage.locator('#otp-passphrase').fill('abcd')
    await loggedInPage.getByRole('button', { name: /generate qr/i }).click()

    await expect.poll(() => payload?.access_preset).toBe('game_control')
  })

  test('manual PIN approval sends the selected access preset', async ({ loggedInPage }) => {
    let payload = null
    await loggedInPage.route('**/api/pin', async (route) => {
      payload = route.request().postDataJSON()
      await route.fulfill({ json: { status: true, access_preset: 'game_control' } })
    })

    await loggedInPage.getByRole('navigation').getByRole('link', { name: /^pairing$/i }).click()
    await loggedInPage.getByRole('button', { name: /manual pin/i }).click()
    await loggedInPage.getByRole('radio', { name: /game control/i }).click()
    await loggedInPage.locator('#pin-input').fill('1234')
    await loggedInPage.getByRole('button', { name: /^send$/i }).click()

    await expect.poll(() => payload?.access_preset).toBe('game_control')
  })

  test('shows existing Game Control clients as a named preset', async ({ loggedInPage }) => {
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
            perm: '117448448',
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

    await loggedInPage.getByRole('navigation').getByRole('link', { name: /^pairing$/i }).click()

    const clientCard = loggedInPage.locator('article').filter({ hasText: 'QA Client' }).first()
    await expect(clientCard.getByText('Game Control').first()).toBeVisible()
  })
})
