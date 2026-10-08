import { defineConfig } from '@playwright/test'
import { mockedFrontendSpecs } from './playwright.mocked-specs.js'
import { mockedFrontendConfig } from './scripts/e2e-mocked-config.mjs'

// Live-host URL and CMake asset overrides must not select the mocked lane's tree.
export default defineConfig(mockedFrontendConfig({ testMatch: mockedFrontendSpecs }))
