import fs from 'node:fs/promises'
import path from 'node:path'
import { sourceIdentities } from './mocked.js'

export default async function setup(config) {
  const expected = config.metadata.mockedFrontend
  const response = await fetch(expected.origin)
  if (!response.ok) throw new Error('Mocked frontend preview did not respond')
  const html = await response.text()
  const identities = sourceIdentities(html)
  if (identities.length !== 1 || identities[0] !== expected.identity) {
    throw new Error('Mocked frontend source identity mismatch: rebuild this checkout before testing')
  }
  await fs.mkdir(config.projects[0].outputDir, { recursive: true })
  await fs.writeFile(path.join(config.projects[0].outputDir, 'frontend-identity.json'),
    JSON.stringify({ ...expected, nodeVersion: process.version }, null, 2) + '\n')
  console.error(`[Polaris mocked E2E] checkout=${expected.sourceSha} frontend=${expected.sourceFingerprint}`)
}
