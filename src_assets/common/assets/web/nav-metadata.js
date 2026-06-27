const ICONS = {
  dashboard: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 5a1 1 0 011-1h4a1 1 0 011 1v5a1 1 0 01-1 1H5a1 1 0 01-1-1V5zm10 0a1 1 0 011-1h4a1 1 0 011 1v2a1 1 0 01-1 1h-4a1 1 0 01-1-1V5zM4 14a1 1 0 011-1h4a1 1 0 011 1v2a1 1 0 01-1 1H5a1 1 0 01-1-1v-2zm10 0a1 1 0 011-1h4a1 1 0 011 1v5a1 1 0 01-1 1h-4a1 1 0 01-1-1v-5z"/></svg>',
  apps: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2V6zm10 0a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2V6zM4 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2v-2zm10 0a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2v-2z"/></svg>',
  pairing: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 7a2 2 0 012 2m4 0a6 6 0 01-7.743 5.743L11 17H9v2H7v2H4a1 1 0 01-1-1v-2.586a1 1 0 01.293-.707l5.964-5.964A6 6 0 1121 9z"/></svg>',
  browserStream: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M7 7h10a4 4 0 014 4v2a4 4 0 01-4 4H7a4 4 0 01-4-4v-2a4 4 0 014-4z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 12h6m-2-2l2 2-2 2"/></svg>',
  config: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.066 2.573c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.573 1.066c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.066-2.573c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z"/></svg>',
  password: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 15v2m-6 4h12a2 2 0 002-2v-6a2 2 0 00-2-2H6a2 2 0 00-2 2v6a2 2 0 002 2zm10-10V7a4 4 0 00-8 0v4h8z"/></svg>',
  info: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>',
  troubleshooting: '<svg class="w-5 h-5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 9l3 3-3 3m5 0h3M5 20h14a2 2 0 002-2V6a2 2 0 00-2-2H5a2 2 0 00-2 2v12a2 2 0 002 2z"/></svg>',
}

export const NAV_SECTION_DEFINITIONS = [
  {
    key: 'streaming',
    labelKey: 'navbar.group_streaming',
    fallbackLabel: 'Streaming',
    items: [
      { commandId: 'dashboard', to: '/', icon: ICONS.dashboard, labelKey: 'navbar.dashboard', fallbackLabel: 'Dashboard', aliases: ['home', 'overview', 'session', 'stream status'], description: 'Open the live control surface and active session overview.' },
      { commandId: 'apps', to: '/apps', icon: ICONS.apps, labelKey: 'navbar.library', fallbackLabel: 'Library', aliases: ['apps', 'applications', 'games', 'game library', 'streamable apps'], description: 'Browse, edit, and import games or streamable applications.' },
      { commandId: 'pairing', to: '/pin', icon: ICONS.pairing, labelKey: 'navbar.pairing', fallbackLabel: 'Pairing', aliases: ['pin', 'devices', 'clients', 'moonlight pairing'], description: 'Pair Moonlight clients and review trusted devices.' },
      { commandId: 'browser-stream', to: '/browser-stream', icon: ICONS.browserStream, labelKey: 'navbar.browser_stream', fallbackLabel: 'Browser Stream', aliases: ['moonlight', 'stream', 'streaming', 'webrtc', 'web stream'], description: 'Open browser streaming tools using familiar Moonlight streaming terms.' },
    ],
  },
  {
    key: 'host',
    labelKey: 'navbar.group_host',
    fallbackLabel: 'Host',
    items: [
      { commandId: 'config', to: '/config', icon: ICONS.config, labelKey: 'navbar.settings', fallbackLabel: 'Settings', aliases: ['config', 'configuration', 'host settings', 'encoder'], description: 'Adjust host, network, encoder, and display settings.' },
      { commandId: 'password', to: '/password', icon: ICONS.password, labelKey: 'navbar.security', fallbackLabel: 'Security', aliases: ['password', 'security', 'credentials'], description: 'Manage web access and host security controls.' },
    ],
  },
  {
    key: 'support',
    labelKey: 'navbar.group_support',
    fallbackLabel: 'Support',
    items: [
      { commandId: 'info', to: '/info', icon: ICONS.info, labelKey: 'navbar.system', fallbackLabel: 'System', aliases: ['system', 'about', 'version', 'host info'], description: 'Review host system information and version details.' },
      { commandId: 'troubleshooting', to: '/troubleshooting', icon: ICONS.troubleshooting, labelKey: 'navbar.troubleshoot', fallbackLabel: 'Troubleshooting', aliases: ['logs', 'diagnostics', 'support', 'runtime'], description: 'Open logs, runtime diagnostics, and support tools.' },
    ],
  },
]

function translate(t, key, fallback) {
  if (typeof t !== 'function') return fallback
  const translated = t(key)
  return translated && translated !== key ? translated : fallback
}

export function createNavSections(t) {
  let shortcut = 1
  return NAV_SECTION_DEFINITIONS.map((section) => {
    const sectionLabel = translate(t, section.labelKey, section.fallbackLabel)
    return {
      key: section.key,
      label: sectionLabel,
      items: section.items.map((item) => ({
        ...item,
        id: item.commandId,
        label: translate(t, item.labelKey, item.fallbackLabel),
        sectionLabel,
        shortcut: shortcut++,
      })),
    }
  })
}

export function flattenNavItems(sections) {
  return sections.flatMap((section) => section.items)
}

export function getNavItemByPath(sections, path) {
  return flattenNavItems(sections).find((item) => item.to === path) || null
}
