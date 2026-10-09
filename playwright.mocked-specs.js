// These suites fulfill every host API request and need only this checkout's frontend.
// Keep the live and mocked lanes complementary so a case cannot silently move hosts.
export const mockedFrontendSpecs = [
  '**/live-tuning.spec.js',
  '**/manual-pairing.spec.js',
  '**/update-channel.spec.js',
  '**/welcome-layout.spec.js',
  '**/polaris-settings-ux.spec.js',
  '**/polaris-ui-followup.spec.js',
  '**/recording-policy.spec.js',
]
