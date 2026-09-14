import { chromium, expect } from '@playwright/test'
import fs from 'node:fs/promises'
import http from 'node:http'
import path from 'node:path'
const root = path.resolve('build/assets/web')
const output = path.resolve('build/validation/spaces-ui')
await fs.mkdir(output, { recursive: true })
const server = http.createServer(async (req, res) => {
  const target = path.resolve(root, '.' + (new URL(req.url, 'http://localhost').pathname === '/' ? '/index.html' : new URL(req.url, 'http://localhost').pathname))
  if (!target.startsWith(root + path.sep)) { res.writeHead(403); res.end(); return }
  try {
    const content = await fs.readFile(target)
    res.setHeader('content-type', ({ '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.svg': 'image/svg+xml' })[path.extname(target)] || 'application/octet-stream')
    res.end(content)
  } catch { res.writeHead(404); res.end() }
})
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
const origin = 'http://127.0.0.1:' + server.address().port
const browser = await chromium.launch()
let ready = false, configured = false
const clients = [{ uuid: 'device-a', name: 'Handheld', perm: 0x07001F00, temporary_authorization: false }]
const titles = ['Docker Engine', 'Polaris access to Docker', 'Gaming runtime account', 'Controller and input access', 'Graphics device access', 'Spaces security support', 'Spaces configuration']
const facts = () => ({
  version: 2, distribution: 'fedora', immutable_host: false, service_uid: 1000,
  host_prerequisites_ready: ready, configured, available: configured,
  checks: ['docker','docker_access','identity','input','gpu','security','spaces'].map((id, index) => ({
    id, title: titles[index], action: id === 'security' ? 'install_selinux' : '', state: id === 'spaces' ? configured ? 'ready' : 'not_configured' : ready ? 'ready' : 'required',
    detail: id === 'spaces' ? configured ? 'The configured Spaces controller is available.' : 'Host preparation comes first. No spaces have been configured on this host.' :
      ready ? 'This prerequisite passed.' : 'Complete this host setup step, then recheck.',
  })),
})
try {
  const page = await browser.newPage({ viewport: { width: 1365, height: 1000 } })
  const errors = [], methods = []
  page.on('pageerror', error => errors.push(error.message))
  await page.route('**/*', async route => {
    const request = route.request(), url = new URL(request.url())
    if (url.origin !== origin) { await route.abort(); return }
    if (!url.pathname.startsWith('/api/')) { await route.continue(); return }
    methods.push(request.method())
    let data = { status: true }
    if (url.pathname === '/api/config') data = { status: true, platform: 'linux', version: 'preview', sunshine_name: 'Gaming PC' }
    if (url.pathname === '/api/spaces/setup') data = facts()
    if (url.pathname === '/api/clients/list') data = { status: true, named_certs: clients }
    if (url.pathname === '/api/multiseat/profiles') data = {
      enabled: configured, available: configured, changing: false, failed: false, creation_available: configured,
      profiles: configured ? [
        { id: 'space-a', name: 'Alex', clients: ['device-a'], steam: true },
        { id: 'space-b', name: 'Riley', clients: [], steam: true },
      ] : [],
    }
    await route.fulfill({ json: data })
  })
  await page.goto(origin + '/#/spaces')
  await expect(page.getByRole('heading', { name: 'Spaces', exact: true })).toBeVisible()
  await expect(page.getByText('Install Docker step by step', { exact: true })).toBeVisible()
  await page.getByText('Install Docker step by step', { exact: true }).click()
  await expect(page.getByText('Add the Docker package repository', { exact: false })).toBeVisible()
  await page.screenshot({ path: output + '/spaces-setup-desktop.png', fullPage: true })
  await page.setViewportSize({ width: 390, height: 844 })
  await expect(page.getByRole('heading', { name: 'Spaces', exact: true })).toBeVisible()
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true)
  await page.screenshot({ path: output + '/spaces-setup-mobile.png', fullPage: true })
  await page.getByText('Install Docker step by step', { exact: true }).scrollIntoViewIfNeeded()
  await page.screenshot({ path: output + '/spaces-docker-mobile.png' })
  await page.getByText('Prepare Spaces security support', { exact: true }).click()
  await expect(page.getByText('sudo -H polaris-spaces-setup install', { exact: true })).toBeVisible()
  await page.getByText('Prepare Spaces security support', { exact: true }).scrollIntoViewIfNeeded()
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true)
  await page.screenshot({ path: output + '/spaces-security-mobile.png' })
  await page.setViewportSize({ width: 1365, height: 1000 })
  await page.screenshot({ path: output + '/spaces-security-desktop.png' })
  ready = true
  await page.getByRole('button', { name: 'Recheck setup', exact: true }).click()
  await expect(page.getByText('Host prerequisites checked.', { exact: false })).toBeVisible()
  await expect(page.getByText('Host prerequisites checked.', { exact: false })).toBeVisible()
  configured = true
  await page.reload()
  await expect(page.getByRole('heading', { name: 'Your spaces', exact: true })).toBeVisible()
  await expect(page.getByRole('heading', { name: 'Alex', exact: true })).toBeVisible()
  await expect(page.getByRole('combobox', { name: 'Handheld', exact: true })).toHaveValue('space-a')
  await page.setViewportSize({ width: 1365, height: 1000 })
  await page.screenshot({ path: output + '/spaces-configured-desktop.png', fullPage: true })
  expect(errors).toEqual([])
  expect(methods.every(method => method === 'GET')).toBe(true)
  console.log(JSON.stringify({ result: 'PASS', checks: ['Docker walkthrough', 'SELinux walkthrough', '390px overflow', 'recheck', 'bootstrap boundary', 'existing assignment'], pageErrors: errors, mutations: methods.filter(method => method !== 'GET'), evidence: output }))
} finally { await browser.close(); await new Promise(resolve => server.close(resolve)) }
