import { createNavSections, flattenNavItems } from './nav-metadata.js'

const RECENT_COMMANDS_KEY = 'polaris:recent-commands'
const MAX_RECENT_COMMANDS = 5

const SETTINGS_ACTIONS = [
  { id: 'audio', group: 'Settings', icon: '🔊', labelKey: 'command_palette.actions.audio', fallbackLabel: 'Audio settings', description: 'Jump straight to audio sink and output tuning.', hint: 'Audio', aliases: ['audio', 'sound', 'sink'], to: '/config#audio_sink' },
  { id: 'display', group: 'Settings', icon: '🖥️', labelKey: 'command_palette.actions.display', fallbackLabel: 'Display settings', description: 'Jump to display device configuration and mode control.', hint: 'Display', aliases: ['display', 'monitor', 'resolution', 'headless'], to: '/config#dd_configuration_option' },
  { id: 'ai', group: 'Settings', icon: '✨', labelKey: 'command_palette.actions.ai', fallbackLabel: 'AI optimization', description: 'Review AI optimization and provider settings.', hint: 'AI', aliases: ['ai', 'optimizer', 'quality', 'codex'], to: '/config#ai_enabled' },
  { id: 'network', group: 'Settings', icon: '🌐', labelKey: 'command_palette.actions.network', fallbackLabel: 'Network access', description: 'Review pairing access, trusted subnets, and exposure.', hint: 'Network', aliases: ['network', 'subnets', 'lan', 'pairing access'], to: '/config#trusted_subnets' },
]

function translate(t, key, fallback) {
  if (typeof t !== 'function') return fallback
  const translated = t(key)
  return translated && translated !== key ? translated : fallback
}

function normalize(text) {
  return String(text || '').trim().toLowerCase()
}

function searchableText(action) {
  return [
    action.label,
    action.hint,
    action.description,
    action.group,
    ...(action.aliases || []),
  ].map(normalize).join(' ')
}

function createNavigationActions(t, router) {
  return flattenNavItems(createNavSections(t)).map((item) => ({
    id: item.commandId,
    group: 'Navigation',
    icon: item.shortcut,
    label: item.label,
    description: item.description,
    hint: item.to,
    aliases: item.aliases,
    action: () => router.push(item.to),
  }))
}

function createSettingsActions(t, router) {
  return SETTINGS_ACTIONS.map((action) => ({
    id: action.id,
    group: action.group,
    icon: action.icon,
    label: translate(t, action.labelKey, action.fallbackLabel),
    description: action.description,
    hint: action.hint,
    aliases: action.aliases,
    action: () => router.push(action.to),
  }))
}

function createHostActions(t, fetchImpl, toast) {
  return [
    {
      id: 'restart',
      group: 'Host Controls',
      icon: '🔄',
      label: translate(t, 'command_palette.actions.restart', 'Restart Polaris'),
      description: 'Restart Polaris to apply host-level changes. Active streams may disconnect.',
      hint: 'POST /api/restart',
      aliases: ['restart', 'reboot host', 'apply restart'],
      dangerous: true,
      confirmation: {
        title: 'Restart Polaris?',
        message: 'This will restart the host service from the web UI.',
        impactItems: ['Active Moonlight and browser streams can disconnect.', 'The web console may briefly go offline.'],
        confirmLabel: 'Restart Polaris',
        pendingLabel: 'Restarting…',
      },
      action: async () => {
        await fetchImpl('./api/restart', { method: 'POST' })
        toast(translate(t, 'command_palette.restart_sent', 'Restart requested'), 'info')
      },
    },
    {
      id: 'quit',
      group: 'Host Controls',
      icon: '⏹️',
      label: translate(t, 'command_palette.actions.quit', 'Quit Polaris'),
      description: 'Shut down the Polaris host service from the web UI.',
      hint: 'POST /api/quit',
      aliases: ['quit', 'shutdown', 'stop host', 'exit polaris'],
      dangerous: true,
      confirmation: {
        title: 'Quit Polaris?',
        message: 'This will shut down the Polaris host service from the web UI.',
        impactItems: ['Connected Moonlight clients and browser streams will disconnect.', 'The web console will stop responding until Polaris starts again.'],
        confirmLabel: 'Quit Polaris',
        pendingLabel: 'Quitting…',
      },
      action: async () => {
        await fetchImpl('./api/quit', { method: 'POST' })
        toast(translate(t, 'command_palette.quit_sent', 'Quit requested'), 'info')
      },
    },
  ]
}

export function createCommandActions({ t, router, fetchImpl = fetch, toast = () => {} }) {
  return [
    ...createNavigationActions(t, router),
    ...createSettingsActions(t, router),
    ...createHostActions(t, fetchImpl, toast),
  ]
}

export function scoreCommandAction(action, query) {
  const q = normalize(query)
  if (!q) return 0
  const label = normalize(action.label)
  if (label === q) return 100
  if (label.startsWith(q)) return 80
  if ((action.aliases || []).some((alias) => normalize(alias) === q)) return 75
  if (label.includes(q)) return 60
  if ((action.aliases || []).some((alias) => normalize(alias).includes(q))) return 50
  if (searchableText(action).includes(q)) return 25
  const terms = q.split(/\s+/).filter(Boolean)
  if (terms.length && terms.every((term) => searchableText(action).includes(term))) return 20
  return 0
}

export function readRecentCommandIds(storage = globalThis.localStorage) {
  try {
    return JSON.parse(storage?.getItem(RECENT_COMMANDS_KEY) || '[]').filter((id) => typeof id === 'string')
  } catch {
    return []
  }
}

export function recordRecentCommand(id, storage = globalThis.localStorage) {
  if (!id || !storage) return []
  const recent = [id, ...readRecentCommandIds(storage).filter((recentId) => recentId !== id)].slice(0, MAX_RECENT_COMMANDS)
  storage.setItem(RECENT_COMMANDS_KEY, JSON.stringify(recent))
  return recent
}

export function filterCommandActions(actions, query, { recentCommandIds = [] } = {}) {
  const q = normalize(query)
  const recentRank = new Map(recentCommandIds.map((id, index) => [id, index]))

  return actions
    .map((action, index) => ({
      action,
      index,
      score: q ? scoreCommandAction(action, q) : 1,
      recentIndex: recentRank.has(action.id) ? recentRank.get(action.id) : Number.POSITIVE_INFINITY,
    }))
    .filter((entry) => entry.score > 0)
    .sort((a, b) => {
      if (!q && a.recentIndex !== b.recentIndex) return a.recentIndex - b.recentIndex
      if (b.score !== a.score) return b.score - a.score
      return a.index - b.index
    })
    .map((entry) => entry.action)
}

export async function executeCommandAction(action, { confirmDangerousAction } = {}) {
  if (!action) return false
  if (action.dangerous) {
    const confirmed = await confirmDangerousAction?.(action)
    if (!confirmed) return false
  }

  await action.action()
  return true
}
