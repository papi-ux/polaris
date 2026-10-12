// These suites fulfill every host API request and need only this checkout's frontend.
// Keep the live and mocked lanes complementary so a case cannot silently move hosts.
export const mockedFrontendSpecs = [
  '**/live-tuning.spec.js',
  '**/manual-pairing.spec.js',
  '**/update-channel.spec.js',
  '**/welcome-layout.spec.js',
  '**/polaris-settings-ux.spec.js',
  '**/polaris-ui-followup.spec.js',
  '**/polaris-look-feel.spec.js',
  '**/recording-policy.spec.js',
  // Doctor & Support before a stream: its cases need their own stats stream per case (connected,
  // dropped, live), which the look-and-feel fixture's fixed idle EventSource cannot express.
  '**/doctor-idle-host-findings.spec.js',
]
