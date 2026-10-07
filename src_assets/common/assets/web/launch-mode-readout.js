export function launchModeReadout(mode, config = {}, options = []) {
  const provider = typeof config.vdisplayBackend === 'string' ? config.vdisplayBackend.trim() : ''
  const available = id => options.some(option => option.id === id && option.available === true)
  const borrowed = mode === 'host_virtual_display' && provider === 'kscreen-doctor'
  const physical = {
    headless_stream: 'private', windowed_stream: 'windowed', desktop_display: 'mirror',
    gamescope_stream: 'gamescope', headless_dongle: 'dongle', desktop_takeover: 'takeover',
    host_virtual_display: borrowed ? 'borrowed' :
      ['EVDI', 'Wayland (headless output)', 'KWin virtual output'].includes(provider) ? 'virtual' : 'unknown',
  }[mode] || 'unknown'
  const alternative = ['headless_stream', 'desktop_display', 'windowed_stream']
    .find(id => id !== mode && available(id)) || ''
  return { noProvider: provider === 'None', provider: provider && provider !== 'None' ? provider : '', borrowed, physical,
    alternative, monitorOffAlternative: mode === 'host_virtual_display' && available('desktop_takeover') }
}
