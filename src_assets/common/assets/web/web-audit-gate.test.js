import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import { decideWebAudit } from '../../../../scripts/ci/audit-web-dependencies.mjs'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')
// npm audit --json against this lockfile on 2026-11-17, after GHSA-fc5q-39xc-vjpf was widened to
// every picomatch release: 12 findings, of which three are advisories of their own.
const realAudit = read('tests/fixtures/npm-audit-2026-11-17.json')
const realList = JSON.parse(read('scripts/ci/web-audit-exceptions.json'))
const today = '2026-11-17'

const withAdvisory = (name, severity, id) => {
  const audit = JSON.parse(realAudit)
  audit.vulnerabilities[name] = {
    name, severity, isDirect: false, effects: [], range: '*', nodes: [`node_modules/${name}`], fixAvailable: false,
    via: [{ source: 1, name, dependency: name, title: `${name} test advisory`, url: `https://github.com/advisories/${id}`,
      severity, cwe: [], cvss: { score: 0, vectorString: null }, range: '*' }],
  }
  return JSON.stringify(audit)
}

describe('web dependency audit gate', () => {
  it('passes the real audit with the dated exception and names what it let through', () => {
    const result = decideWebAudit(realAudit, realList, today)
    expect(result.failures).toEqual([])
    expect(result.ok).toBe(true)
    expect(result.allowed.map((item) => [item.id, item.package, item.severity])).toEqual([['GHSA-fc5q-39xc-vjpf', 'picomatch', 'high']])
    expect(result.notes).toEqual([])
  })

  it('does not count packages that are high only through a dependency as advisories of their own', () => {
    const result = decideWebAudit(realAudit, { exceptions: [] }, today)
    expect(result.ok).toBe(false)
    expect(result.failures).toHaveLength(1)
    expect(result.failures[0]).toContain('GHSA-fc5q-39xc-vjpf')
    expect(result.failures[0]).toContain('picomatch')
    for (const dependent of ['vite', 'vitest', 'fdir', 'tinyglobby']) {
      expect(result.failures.join('\n')).not.toContain(` in ${dependent}:`)
    }
  })

  it('fails on another high advisory that is not listed', () => {
    const result = decideWebAudit(withAdvisory('left-pad', 'high', 'GHSA-aaaa-bbbb-cccc'), realList, today)
    expect(result.ok).toBe(false)
    expect(result.failures).toEqual(['high advisory GHSA-aaaa-bbbb-cccc in left-pad: left-pad test advisory'])
  })

  it('fails on a critical advisory', () => {
    const result = decideWebAudit(withAdvisory('left-pad', 'critical', 'GHSA-dddd-eeee-ffff'), realList, today)
    expect(result.ok).toBe(false)
    expect(result.failures[0]).toContain('critical advisory GHSA-dddd-eeee-ffff')
  })

  it('fails once an exception is past its review date', () => {
    expect(decideWebAudit(realAudit, realList, '2026-12-17').ok).toBe(true)
    const result = decideWebAudit(realAudit, realList, '2026-12-18')
    expect(result.ok).toBe(false)
    expect(result.failures).toEqual(['exception GHSA-fc5q-39xc-vjpf (picomatch) was due for review on 2026-12-17'])
  })

  it('fails closed on anything it cannot read', () => {
    expect(decideWebAudit('{"vulnerabilities": {', realList, today).ok).toBe(false)
    expect(decideWebAudit('', realList, today).ok).toBe(false)
    expect(decideWebAudit('{"error": {"code": "ENOAUDIT", "summary": "no audit"}}', realList, today).ok).toBe(false)
    expect(decideWebAudit('{"advisories": {}, "metadata": {}}', realList, today).ok).toBe(false)
    expect(decideWebAudit(realAudit, { exceptions: [{ id: 'GHSA-fc5q-39xc-vjpf' }] }, today).ok).toBe(false)
  })

  it('passes moderate advisories and notes an exception that no longer matches', () => {
    const audit = JSON.parse(realAudit)
    for (const name of ['picomatch', 'fdir', 'tinyglobby', 'vite', 'vitest']) delete audit.vulnerabilities[name]
    const result = decideWebAudit(JSON.stringify(audit), realList, today)
    expect(result.ok).toBe(true)
    expect(result.allowed).toEqual([])
    expect(result.notes).toEqual(['exception GHSA-fc5q-39xc-vjpf (picomatch) matches no high or critical advisory any more'])
  })

  it('keeps the exception list short-lived and complete', () => {
    expect(realList.exceptions).toHaveLength(1)
    for (const entry of realList.exceptions) {
      for (const field of ['id', 'package', 'added', 'review_by', 'reason']) {
        expect(typeof entry[field] === 'string' && entry[field].trim().length > 0, `${entry.id ?? 'entry'} ${field}`).toBe(true)
      }
      const days = (Date.parse(`${entry.review_by}T00:00:00Z`) - Date.parse(`${entry.added}T00:00:00Z`)) / 86400000
      expect(days).toBeGreaterThanOrEqual(0)
      expect(days).toBeLessThanOrEqual(45)
    }
  })
})
