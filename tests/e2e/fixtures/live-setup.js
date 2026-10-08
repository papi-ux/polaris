export default async function setup(config) {
  const target = process.env.POLARIS_E2E_BASE_URL || process.env.POLARIS_URL
  if (!target) throw new Error('Set POLARIS_E2E_BASE_URL or POLARIS_URL explicitly for live-host tests')
  const url = new URL(target)
  if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password) {
    throw new Error('Live-host tests require an HTTP(S) URL; supply credentials through POLARIS_USER and POLARIS_PASS')
  }
  console.error(`[Polaris live E2E] frontend=host-served origin=${url.origin}; checkout assets are not tested in this lane`)
}
