import { test, expect } from './fixtures/auth.js'

test('config view loads', async ({ loggedInPage }) => {
  await loggedInPage.goto('/#/config')
  await expect(loggedInPage).toHaveURL(/#\/config/)
  await expect(loggedInPage.getByRole('heading', { name: /^settings$/i })).toBeVisible({ timeout: 10000 })
})

test('apps view loads', async ({ loggedInPage }) => {
  await loggedInPage.goto('/#/apps')
  await expect(loggedInPage).toHaveURL(/#\/apps/)
  await expect(loggedInPage.getByRole('heading', { name: /^library$/i })).toBeVisible({ timeout: 10000 })
})
