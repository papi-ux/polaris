import { defineConfig, devices } from '@playwright/test'
import { fileURLToPath } from 'node:url'
import { mockedFrontendSpecs } from './playwright.mocked-specs.js'

// Live-host URL and CMake asset overrides must not select the mocked lane's tree.
const baseURL = 'http://127.0.0.1:49386'
const frontendEnv = {
  POLARIS_SOURCE_ASSETS_DIR: '',
  SUNSHINE_SOURCE_ASSETS_DIR: '',
  POLARIS_ASSETS_DIR: '',
  SUNSHINE_ASSETS_DIR: '',
}

export default defineConfig({
  testDir: './tests/e2e',
  testMatch: mockedFrontendSpecs,
  fullyParallel: false,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  workers: 1,
  reporter: process.env.CI ? [['github'], ['html', { open: 'never' }]] : 'list',
  outputDir: './test-results/e2e-mocked',
  use: {
    baseURL,
    trace: 'on-first-retry',
    screenshot: 'only-on-failure',
    video: 'retain-on-failure',
  },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
  ],
  webServer: {
    command: 'npm run build-clean && node node_modules/vite/bin/vite.js preview --host 127.0.0.1 --port 49386 --strictPort',
    url: baseURL,
    cwd: fileURLToPath(new URL('.', import.meta.url)),
    env: frontendEnv,
    reuseExistingServer: false,
  },
})
