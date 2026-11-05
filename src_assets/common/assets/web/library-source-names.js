const SOURCE_NAMES = {
  steam: 'Steam',
  lutris: 'Lutris',
  heroic: 'Heroic',
  emulator: 'Emulator',
  epic_launcher: 'Epic',
}

export function sourceLabel(source = '') {
  return SOURCE_NAMES[source] || source || 'Unknown'
}
