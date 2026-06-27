#!/usr/bin/env node
import { readdir, readFile, stat } from 'node:fs/promises'
import { existsSync } from 'node:fs'
import path from 'node:path'
import { gzipSync } from 'node:zlib'

const root = path.resolve(process.cwd(), 'build/assets/web')
const assetsDir = path.join(root, 'assets')

const KiB = 1024
const budgets = [
  { label: 'index.html', pattern: /^index\.html$/, raw: 240 * KiB, gzip: 35 * KiB, dir: root },
  { label: 'initial app JS', pattern: /^index-[\w-]+\.js$/, raw: 430 * KiB, gzip: 150 * KiB, dir: assetsDir },
  { label: 'initial app CSS', pattern: /^index-[\w-]+\.css$/, raw: 230 * KiB, gzip: 32 * KiB, dir: assetsDir },
  { label: 'lazy uPlot JS', pattern: /^uPlot(?:\.esm)?-[\w-]+\.js$/, raw: 60 * KiB, gzip: 26 * KiB, dir: assetsDir },
  { label: 'lazy uPlot CSS', pattern: /^uPlot-[\w-]+\.css$/, raw: 4 * KiB, gzip: 2 * KiB, dir: assetsDir },
]

function formatBytes(bytes) {
  return `${(bytes / KiB).toFixed(1)} KiB`
}

async function matchingFiles(dir, pattern) {
  const entries = await readdir(dir)
  return entries.filter((entry) => pattern.test(entry)).map((entry) => path.join(dir, entry))
}

async function checkFileBudget({ label, pattern, raw, gzip, dir }) {
  const matches = await matchingFiles(dir, pattern)
  if (matches.length !== 1) {
    throw new Error(`${label}: expected exactly one emitted asset matching ${pattern}, found ${matches.length}`)
  }

  const file = matches[0]
  const content = await readFile(file)
  const rawSize = content.byteLength
  const gzipSize = gzipSync(content).byteLength
  const failures = []

  if (rawSize > raw) failures.push(`raw ${formatBytes(rawSize)} > ${formatBytes(raw)}`)
  if (gzipSize > gzip) failures.push(`gzip ${formatBytes(gzipSize)} > ${formatBytes(gzip)}`)

  return {
    label,
    file: path.relative(process.cwd(), file),
    rawSize,
    gzipSize,
    ok: failures.length === 0,
    failures,
  }
}

async function checkBuildBudget() {
  if (!existsSync(root)) {
    throw new Error(`Build output not found at ${root}; run npm run build first.`)
  }

  const results = await Promise.all(budgets.map(checkFileBudget))
  const appJs = results.find((result) => result.label === 'initial app JS')
  const appJsContent = await readFile(path.resolve(process.cwd(), appJs.file), 'utf8')
  if (appJsContent.includes('uPlot.js') || appJsContent.includes('uPlot =')) {
    appJs.ok = false
    appJs.failures.push('uPlot implementation appears in the initial app chunk instead of the lazy chart chunk')
  }

  const totalInitialRaw = results
    .filter((result) => ['index.html', 'initial app JS'].includes(result.label))
    .reduce((sum, result) => sum + result.rawSize, 0)
  const totalInitialGzip = results
    .filter((result) => ['index.html', 'initial app JS'].includes(result.label))
    .reduce((sum, result) => sum + result.gzipSize, 0)

  const totalRawBudget = 680 * KiB
  const totalGzipBudget = 190 * KiB
  const totalsOk = totalInitialRaw <= totalRawBudget && totalInitialGzip <= totalGzipBudget

  for (const result of results) {
    const status = result.ok ? 'PASS' : 'FAIL'
    console.log(`${status} ${result.label}: ${result.file} raw=${formatBytes(result.rawSize)} gzip=${formatBytes(result.gzipSize)}`)
    for (const failure of result.failures) console.log(`  - ${failure}`)
  }

  console.log(`${totalsOk ? 'PASS' : 'FAIL'} initial HTML+JS total: raw=${formatBytes(totalInitialRaw)} / ${formatBytes(totalRawBudget)} gzip=${formatBytes(totalInitialGzip)} / ${formatBytes(totalGzipBudget)}`)

  const failed = results.some((result) => !result.ok) || !totalsOk
  if (failed) process.exitCode = 1
}

checkBuildBudget().catch((error) => {
  console.error(error.message)
  process.exitCode = 1
})
