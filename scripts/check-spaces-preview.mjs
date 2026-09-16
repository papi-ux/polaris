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
const profiles = [
  { id: 'space-a', name: 'Alex', clients: ['device-a'], steam: true, archived: false },
  { id: 'space-b', name: 'Riley', clients: [], steam: true, archived: false },
]
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
    if (url.pathname === '/api/multiseat/profiles/manage' && request.method() === 'POST') {
      const change = request.postDataJSON()
      const space = profiles.find(item => item.id === change.profile_id)
      expect(space).toBeDefined()
      if (change.operation === 'rename') space.name = change.name
      else { space.archived = change.operation === 'remove'; space.clients = [] }
      data = { status: true, profile_id: change.profile_id }
    }
    if (url.pathname === '/api/multiseat/profiles') data = {
      enabled: configured, available: configured, changing: false, failed: false, creation_available: configured, management_available: configured,
      profiles: configured ? profiles : [],
    }
    await route.fulfill({ json: data })
  })
  await page.goto(origin + '/#/spaces')
  await expect(page.getByRole('heading', { name: 'Spaces', exact: true })).toBeVisible()
  await expect(page.getByText('Install Docker step by step', { exact: true })).toBeVisible()
  await page.getByText('Install Docker step by step', { exact: true }).click()
  await expect(page.getByText('Add the Docker package repository', { exact: false })).toBeVisible()
  await page.screenshot({ path: output + '/spaces-setup-desktop.png', fullPage: true, animations: 'disabled' })
  await page.setViewportSize({ width: 390, height: 844 })
  await expect(page.getByRole('heading', { name: 'Spaces', exact: true })).toBeVisible()
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true)
  await page.screenshot({ path: output + '/spaces-setup-mobile.png', fullPage: true, animations: 'disabled' })
  await page.getByText('Install Docker step by step', { exact: true }).scrollIntoViewIfNeeded()
  await page.screenshot({ path: output + '/spaces-docker-mobile.png' })
  await page.getByText('Prepare Spaces security support', { exact: true }).click()
  await expect(page.getByText('sudo -H /usr/bin/polaris-spaces-setup install', { exact: true })).toBeVisible()
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
  await page.screenshot({ path: output + '/spaces-configured-desktop.png', fullPage: true, animations: 'disabled' })
  await page.getByRole('button', { name: 'Rename Alex', exact: true }).click()
  await page.getByLabel('Space name', { exact: true }).fill('Living room')
  await page.getByRole('button', { name: 'Save name', exact: true }).click()
  await expect(page.getByRole('heading', { name: 'Living room', exact: true })).toBeVisible()
  await page.getByRole('button', { name: 'Remove Living room', exact: true }).click()
  await expect(page.getByText('This does not free disk space.', { exact: false })).toBeVisible()
  await page.setViewportSize({ width: 390, height: 844 })
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true)
  await page.getByRole('form', { name: 'Remove space', exact: true }).scrollIntoViewIfNeeded()
  await page.screenshot({ path: output + '/spaces-remove-mobile.png', fullPage: true, animations: 'disabled' })
  await page.getByRole('button', { name: 'Remove space', exact: true }).last().click()
  await expect(page.getByRole('combobox', { name: 'Handheld', exact: true })).toHaveValue('')
  expect(await page.getByRole('combobox', { name: 'Handheld', exact: true }).locator('option[value="space-a"]').count()).toBe(0)
  await page.getByText('Removed spaces (1)', { exact: true }).click()
  await page.getByRole('button', { name: 'Restore Living room', exact: true }).click()
  await page.getByRole('button', { name: 'Restore space', exact: true }).click()
  await expect(page.getByRole('heading', { name: 'Living room', exact: true })).toBeVisible()
  await expect(page.getByRole('combobox', { name: 'Handheld', exact: true })).toHaveValue('')
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true)
  await page.getByRole('heading', { name: 'Your spaces', exact: true }).scrollIntoViewIfNeeded()
  await page.screenshot({ path: output + '/spaces-restored-mobile.png', fullPage: true, animations: 'disabled' })
  expect(errors).toEqual([])
  expect(methods.filter(method => method !== 'GET')).toEqual(['POST', 'POST', 'POST'])
  console.log(JSON.stringify({ result: 'PASS', checks: ['Docker walkthrough', 'SELinux walkthrough', '390px overflow', 'recheck', 'bootstrap boundary', 'existing assignment', 'rename', 'confirmed removal', 'restore without reassignment'], pageErrors: errors, mutations: methods.filter(method => method !== 'GET'), evidence: output }))
} finally { await browser.close(); await new Promise(resolve => server.close(resolve)) }
