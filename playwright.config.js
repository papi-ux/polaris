import { defineConfig, devices } from '@playwright/test'
import { mockedFrontendSpecs } from './playwright.mocked-specs.js'

const baseURL = process.env.POLARIS_E2E_BASE_URL || process.env.POLARIS_URL

export default defineConfig({
  testDir: './tests/e2e',
  testIgnore: mockedFrontendSpecs,
  globalSetup: './tests/e2e/fixtures/live-setup.js',
  fullyParallel: false,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  workers: 1,
  reporter: process.env.CI ? [['github'], ['html', { open: 'never' }]] : 'list',
  outputDir: './test-results/e2e',
  use: {
    baseURL,
    ignoreHTTPSErrors: true,
    trace: 'on-first-retry',
    screenshot: 'only-on-failure',
    video: 'retain-on-failure',
  },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
  ],
})
