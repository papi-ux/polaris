// The English strings, resolved the way vue-i18n resolves them, for the Spaces
// component tests. Text assertions stay in the words a person reads.
import { readFileSync } from 'node:fs'
import { join } from 'node:path'

const messages = JSON.parse(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))

export function translate(key, params = {}) {
  const value = String(key).split('.').reduce((node, part) => (node && typeof node === 'object' ? node[part] : undefined), messages)
  if (typeof value !== 'string') return key
  return value.replace(/\{(\w+)\}/g, (match, name) => (params[name] === undefined ? match : String(params[name])))
}

export const i18n = { t: translate }

export const spacesGlobal = { provide: { i18n }, mocks: { $t: translate } }
