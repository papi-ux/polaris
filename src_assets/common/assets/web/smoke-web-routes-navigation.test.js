import { beforeEach, describe, expect, it, vi } from 'vitest'

const smokeHarness = vi.hoisted(() => {
  const gotoURLs = []
  const locator = {
    count: async () => 0,
    first: () => locator,
    innerText: async () => 'Polaris smoke route content '.repeat(5),
  }
  const apiResponse = {
    ok: () => true,
    status: () => 200,
    text: async () => [
      '<!doctype html>',
      '<script type="module" crossorigin src="./assets/index-AbC123.js"></script>',
      '<link rel="stylesheet" crossorigin href="./assets/index-ZxY987.css">',
    ].join('\n'),
  }
  const apiRequest = {
    dispose: async () => {},
    get: async () => apiResponse,
    post: async () => apiResponse,
  }
  const context = {
    clearCookies: async () => {},
    request: apiRequest,
  }
  const page = {
    context: () => context,
    evaluate: async () => {},
    getByLabel: () => locator,
    getByRole: () => locator,
    getByText: () => locator,
    goto: async (url) => {
      gotoURLs.push(url)
    },
    locator: () => locator,
    on: () => {},
    screenshot: async () => {},
    waitForTimeout: async () => {},
  }
  const browser = {
    close: async () => {},
    newContext: async () => ({
      ...context,
      newPage: async () => page,
    }),
  }

  return { apiRequest, browser, gotoURLs }
})

vi.mock('@playwright/test', () => ({
  chromium: {
    launch: async () => smokeHarness.browser,
  },
  expect: () => ({
    toBeVisible: async () => {},
  }),
  request: {
    newContext: async () => smokeHarness.apiRequest,
  },
}))

import { runSmoke } from '../../../../scripts/smoke-web-routes.mjs'

describe('authenticated smoke route navigation', () => {
  beforeEach(() => {
    smokeHarness.gotoURLs.length = 0
  })

  it('loads Dashboard once after login instead of immediately reloading it', async () => {
    const baseURL = 'https://127.0.0.1:58490'

    await runSmoke({
      baseURL,
      mode: 'live',
      noLogin: false,
      password: 'test-password',
      screenshotDir: '/tmp',
      username: 'test-user',
      writeScreenshots: false,
    })

    expect(smokeHarness.gotoURLs).toHaveLength(2)
    expect(smokeHarness.gotoURLs[0]).toMatch(/^https:\/\/127\.0\.0\.1:58490\/\?smoke=\d+#\/login$/)
    expect(smokeHarness.gotoURLs[1]).toMatch(/^https:\/\/127\.0\.0\.1:58490\/\?smoke=\d+-0#\/$/)
  })
})
