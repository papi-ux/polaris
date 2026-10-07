import { defineConfig } from '@playwright/test'
import { mockedFrontendConfig } from './scripts/e2e-mocked-config.mjs'

export default defineConfig(mockedFrontendConfig({
  testMatch: 'manual-pairing.spec.js',
  port: 49387, retries: 0,
  outputDir: './test-results/manual-pairing',
}))
