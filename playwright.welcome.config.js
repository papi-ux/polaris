import { defineConfig, devices } from '@playwright/test'

export default defineConfig({
  testDir: './tests/e2e',
  testMatch: 'welcome-layout.spec.js',
  workers: 1,
  outputDir: './test-results/welcome-layout',
  use: {
    ...devices['Desktop Chrome'],
    baseURL: 'http://127.0.0.1:49388',
    screenshot: 'only-on-failure',
  },
  webServer: {
    command: 'npx vite preview --host 127.0.0.1 --port 49388 --strictPort',
    url: 'http://127.0.0.1:49388',
    reuseExistingServer: false,
  },
})
