import { defineConfig, devices } from '@playwright/test'

export default defineConfig({
  testDir: './tests/e2e',
  testMatch: 'manual-pairing.spec.js',
  workers: 1,
  use: { baseURL: 'http://127.0.0.1:49387', ...devices['Desktop Chrome'] },
  webServer: {
    command: 'python3 -m http.server 49387 --bind 127.0.0.1 --directory build/assets/web',
    url: 'http://127.0.0.1:49387',
    reuseExistingServer: false,
  },
})
