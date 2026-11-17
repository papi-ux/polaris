#!/usr/bin/env node
// The web dependency audit gate's decision, run when `npm audit --audit-level=high` fails.
//
// It runs `npm audit --json` and fails closed: on any high or critical advisory that is not on the
// dated exception list (scripts/ci/web-audit-exceptions.json), on an exception whose review_by date
// has passed, and on anything it cannot read (npm did not run, no JSON, JSON of another shape).
// An advisory is an object entry under a package's `via`; a package listed only because it depends
// on a vulnerable one is not an advisory of its own. The decision is decideWebAudit(), a pure
// function, so it is tested without the network; npm runs only when this file is executed directly.
import { spawnSync } from 'node:child_process'
import { readFileSync, realpathSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

const SEVERITIES = ['info', 'low', 'moderate', 'high', 'critical']
const BLOCKING = new Set(['high', 'critical'])
const DATE = /^\d{4}-\d{2}-\d{2}$/
const isObject = (value) => value !== null && typeof value === 'object' && !Array.isArray(value)
const validDate = (value) => typeof value === 'string' && DATE.test(value) &&
  new Date(`${value}T00:00:00Z`).toISOString().slice(0, 10) === value

function readExceptions(list, failures) {
  if (!isObject(list) || !Array.isArray(list.exceptions)) {
    failures.push('the exception list is not an object with an "exceptions" array')
    return []
  }
  const entries = []
  list.exceptions.forEach((entry, index) => {
    const fields = ['id', 'package', 'added', 'review_by', 'reason']
    const missing = fields.filter((field) => !isObject(entry) || typeof entry[field] !== 'string' || !entry[field].trim())
    if (missing.length) {
      failures.push(`exception ${index + 1} is missing ${missing.join(', ')}`)
    } else if (!validDate(entry.added) || !validDate(entry.review_by) || entry.review_by < entry.added) {
      failures.push(`exception ${entry.id} needs real YYYY-MM-DD dates with review_by on or after added`)
    } else {
      entries.push(entry)
    }
  })
  return entries
}

function readAdvisories(auditText, failures) {
  let audit
  try {
    audit = JSON.parse(auditText)
  } catch {
    failures.push('npm audit --json did not print JSON')
    return []
  }
  if (!isObject(audit) || audit.auditReportVersion !== 2 || !isObject(audit.vulnerabilities)) {
    const error = isObject(audit) && isObject(audit.error) ? `: ${audit.error.code ?? ''} ${audit.error.summary ?? ''}`.trimEnd() : ''
    failures.push(`npm audit --json returned JSON of another shape (want auditReportVersion 2 with "vulnerabilities")${error}`)
    return []
  }
  const advisories = new Map()
  for (const [name, entry] of Object.entries(audit.vulnerabilities)) {
    if (!isObject(entry) || !Array.isArray(entry.via)) {
      failures.push(`npm audit entry ${name} has no "via" list`)
      continue
    }
    for (const via of entry.via) {
      if (typeof via === 'string') continue  // only here because it depends on a vulnerable package
      if (!isObject(via) || typeof via.name !== 'string' || !SEVERITIES.includes(via.severity)) {
        failures.push(`npm audit entry ${name} has an advisory of another shape`)
        continue
      }
      const id = typeof via.url === 'string' ? via.url.split('/').pop() : ''
      const key = `${id || via.source}:${via.name}`
      if (!advisories.has(key)) {
        advisories.set(key, { id: id || `npm advisory ${via.source}`, package: via.name, severity: via.severity, title: via.title ?? '' })
      }
    }
  }
  return [...advisories.values()]
}

/**
 * The gate's decision. auditText is the text `npm audit --json` printed; exceptionList is the parsed
 * exception file; today is a UTC date, YYYY-MM-DD. Returns { ok, failures, allowed, notes }.
 */
export function decideWebAudit(auditText, exceptionList, today) {
  const failures = []
  const allowed = []
  const notes = []
  if (!validDate(today)) failures.push(`today must be a YYYY-MM-DD date, not ${today}`)
  const exceptions = readExceptions(exceptionList, failures)
  const advisories = readAdvisories(auditText, failures)

  for (const entry of exceptions) {
    if (validDate(today) && today > entry.review_by) {
      failures.push(`exception ${entry.id} (${entry.package}) was due for review on ${entry.review_by}`)
    }
  }
  const used = new Set()
  for (const advisory of advisories) {
    if (!BLOCKING.has(advisory.severity)) continue
    const entry = exceptions.find((item) => item.id === advisory.id && item.package === advisory.package)
    if (entry) {
      used.add(entry)
      allowed.push({ ...advisory, reason: entry.reason, review_by: entry.review_by })
    } else {
      failures.push(`${advisory.severity} advisory ${advisory.id} in ${advisory.package}: ${advisory.title}`)
    }
  }
  for (const entry of exceptions) {
    if (!used.has(entry)) notes.push(`exception ${entry.id} (${entry.package}) matches no high or critical advisory any more`)
  }
  return { ok: failures.length === 0, failures, allowed, notes }
}

function main() {
  const exceptionsFile = fileURLToPath(new URL('./web-audit-exceptions.json', import.meta.url))
  const today = new Date().toISOString().slice(0, 10)
  let exceptionList
  try {
    exceptionList = JSON.parse(readFileSync(exceptionsFile, 'utf8'))
  } catch (error) {
    console.error(`FAIL: cannot read ${exceptionsFile}: ${error.message}`)
    return 1
  }
  const npm = spawnSync('npm', ['audit', '--json'], { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 })
  if (npm.error || npm.status === null) {
    console.error(`FAIL: npm audit --json did not run: ${npm.error?.message ?? `signal ${npm.signal}`}`)
    return 1
  }
  const result = decideWebAudit(npm.stdout, exceptionList, today)
  for (const item of result.allowed) {
    console.log(`ALLOWED ${item.severity} advisory ${item.id} in ${item.package}: ${item.title}`)
    console.log(`  on the exception list until ${item.review_by}: ${item.reason}`)
  }
  for (const note of result.notes) console.log(`NOTE: ${note}`)
  for (const failure of result.failures) console.error(`FAIL: ${failure}`)
  console.log(result.ok ? 'Web dependency audit: no high or critical advisory outside the dated exception list.'
    : 'Web dependency audit failed.')
  return result.ok ? 0 : 1
}

function invokedDirectly() {
  try {
    return Boolean(process.argv[1]) && realpathSync(process.argv[1]) === realpathSync(fileURLToPath(import.meta.url))
  } catch {
    return false
  }
}

if (invokedDirectly()) process.exitCode = main()
