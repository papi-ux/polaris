import { devices } from '@playwright/test'
import { fileURLToPath } from 'node:url'
import { frontendSourceIdentity } from './e2e-frontend-identity.mjs'

const root = fileURLToPath(new URL('../', import.meta.url))

export function mockedFrontendConfig({ testMatch, port = 49386, outputDir = './test-results/e2e-mocked', retries = 0 }) {
  const origin = `http://127.0.0.1:${port}`
  return {
    testDir: './tests/e2e', testMatch,
    fullyParallel: false, forbidOnly: !!process.env.CI, retries, workers: 1,
    reporter: process.env.CI ? [['github'], ['html', { open: 'never' }]] : 'list',
    outputDir,
    metadata: { mockedFrontend: { origin, ...frontendSourceIdentity(root) } },
    globalSetup: fileURLToPath(new URL('../tests/e2e/fixtures/mocked-setup.js', import.meta.url)),
    // Guards reconcile after context close. Playwright decides whether to save
    // the browser trace before that verdict, so retain the chunk for every case.
    use: { baseURL: origin, serviceWorkers: 'block', trace: 'on',
      screenshot: 'only-on-failure', video: 'retain-on-failure' },
    projects: [{ name: 'chromium', use: { ...devices['Desktop Chrome'] } }],
    webServer: {
      command: `npm run build-clean && node node_modules/vite/bin/vite.js preview --host 127.0.0.1 --port ${port} --strictPort`,
      url: origin, cwd: root, reuseExistingServer: false,
      env: { POLARIS_SOURCE_ASSETS_DIR: '', SUNSHINE_SOURCE_ASSETS_DIR: '',
        POLARIS_ASSETS_DIR: '', SUNSHINE_ASSETS_DIR: '',
        POLARIS_BUILD_HOMEBREW: '', SUNSHINE_BUILD_HOMEBREW: '',
        POLARIS_E2E_FRONTEND: '1' },
    },
  }
}
