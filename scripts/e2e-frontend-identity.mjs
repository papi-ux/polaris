import { createHash } from 'node:crypto'
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import { resolve } from 'node:path'

// Generated HTML and build caches are ignored by Git. Include new, unignored
// frontend inputs too, so a developer's uncommitted view cannot borrow an ID.
export function frontendSourceIdentity(root) {
  const inputs = ['src_assets/common/assets/web', 'package.json', 'package-lock.json',
    'vite.config.js', 'scripts/e2e-frontend-identity.mjs']
  const files = [...new Set(execFileSync('git', ['ls-files', '--cached', '--others',
    '--exclude-standard', '-z', '--', ...inputs], { cwd: root, encoding: 'utf8' })
    .split('\0').filter(Boolean))].sort()
  const hash = createHash('sha256')
  for (const name of files) {
    hash.update(name).update('\0')
    try {
      hash.update('present\0').update(fs.readFileSync(resolve(root, name)))
    } catch (error) {
      if (error.code !== 'ENOENT') throw error
      // Git still lists an unstaged deletion. Give absence its own fingerprint,
      // rather than crashing or borrowing the clean checkout's identity.
      hash.update('missing\0')
    }
    hash.update('\0')
  }
  const sourceSha = execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim()
  const sourceFingerprint = hash.digest('hex')
  const identity = Buffer.from(JSON.stringify({ schema: 1, sourceSha, sourceFingerprint })).toString('base64url')
  return { identity, sourceSha, sourceFingerprint }
}

export function mockedFrontendIdentityPlugin(root, source, destination) {
  if (process.env.POLARIS_E2E_FRONTEND !== '1') return null
  if (resolve(source) !== resolve(root, 'src_assets/common/assets/web') ||
      resolve(destination) !== resolve(root, 'build/assets/web')) {
    throw new Error('Mocked frontend identity requires the checkout source and build directory')
  }
  const { identity } = frontendSourceIdentity(root)
  return {
    name: 'polaris-mocked-frontend-source',
    apply: 'build',
    transformIndexHtml() {
      return [{ tag: 'meta', attrs: { name: 'polaris-e2e-source', content: identity }, injectTo: 'head' }]
    },
  }
}
