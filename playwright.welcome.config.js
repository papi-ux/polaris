import { defineConfig } from '@playwright/test'
import { mockedFrontendConfig } from './scripts/e2e-mocked-config.mjs'

export default defineConfig(mockedFrontendConfig({
  testMatch: 'welcome-layout.spec.js',
  port: 49388, retries: 0,
  outputDir: './test-results/welcome-layout',
}))
