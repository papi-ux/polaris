import { test, expect } from './fixtures/auth.js'

test.describe('troubleshooting', () => {
  test('renders self-service Doctor support actions without auto-submitting issues', async ({ loggedInPage }) => {
    await loggedInPage.getByRole('link', { name: /troubleshooting/i }).click()
    await expect(loggedInPage).toHaveURL(/#\/troubleshooting/)

    await expect(loggedInPage.getByRole('heading', { name: /^fix my stream$/i })).toBeVisible({ timeout: 15000 })
    await expect(loggedInPage.getByText('Plain Diagnosis', { exact: true })).toBeVisible()
    await expect(loggedInPage.getByText('Advanced / raw diagnostics', { exact: true })).toBeVisible()
    await expect(loggedInPage.getByRole('button', { name: /copy issue draft/i })).toBeVisible()
    await expect(loggedInPage.getByRole('button', { name: /download issue draft/i })).toBeVisible()
    await expect(loggedInPage.getByText(/nothing was submitted automatically|will not submit it/i)).toBeVisible()
    await expect(loggedInPage.getByText(/no passwords, tokens, cookies, auth headers, or credentials/i)).toBeVisible()
  })

  test('log filters remain keyboard reachable', async ({ loggedInPage }) => {
    await loggedInPage.getByRole('link', { name: /troubleshooting/i }).click()
    await expect(loggedInPage).toHaveURL(/#\/troubleshooting/)
    await expect(loggedInPage.getByRole('heading', { name: /^troubleshooting$/i })).toBeVisible({ timeout: 15000 })
    await expect(loggedInPage.getByRole('heading', { name: /^quick recovery$/i })).toBeVisible({ timeout: 15000 })

    const fatalFilter = loggedInPage.getByRole('button', { name: /^fatal$/i })
    await fatalFilter.focus()
    await loggedInPage.keyboard.press('Enter')
    await expect(fatalFilter).toHaveClass(/is-active/)

    const search = loggedInPage.getByRole('textbox', { name: /find/i }).first()
    await expect(search).toBeVisible()
    await search.fill('Warning')

    const clearButton = loggedInPage.getByRole('button', { name: /^(clear log filters|clear)$/i })
    await expect(clearButton).toBeVisible()
  })
})
