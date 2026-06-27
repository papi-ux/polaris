import { describe, expect, it } from 'vitest'

import { rankSettingsSearchTabs, scoreSettingsOption } from './settings-search.js'

const tabs = [
  {
    id: 'general',
    name: 'General',
    groupLabel: 'Core Setup',
    summary: 'Identity and desktop UI behavior.',
    options: { sunshine_name: '', min_log_level: 2 },
  },
  {
    id: 'av',
    name: 'Audio/Video',
    groupLabel: 'Core Setup',
    summary: 'Capture strategy, display outputs, and stream delivery controls.',
    searchAliases: ['Linux streaming setup', 'headless display pairing'],
    options: {
      audio_sink: '',
      adapter_name: '',
      output_name: '',
      headless_mode: 'disabled',
      linux_use_cage_compositor: 'disabled',
      linux_prefer_gpu_native_capture: 'disabled',
    },
  },
  {
    id: 'ai',
    name: 'AI',
    groupLabel: 'Host & Runtime',
    summary: 'Provider choice and optimization defaults.',
    searchAliases: ['Auto Quality headless tuning'],
    options: { ai_enabled: 'disabled', adaptive_bitrate_enabled: 'disabled' },
  },
]

function optionFactory(key) {
  const labels = {
    adapter_name: 'GPU / Display Adapter',
    output_name: 'Display output',
    linux_prefer_gpu_native_capture: 'GPU-native capture preference',
    ai_enabled: 'AI profile optimizer',
    adaptive_bitrate_enabled: 'Adaptive bitrate',
  }
  const descriptions = {
    adapter_name: 'Pick the encoder GPU that owns the display path.',
    output_name: 'Pair Polaris with the monitor or virtual output to capture.',
    linux_prefer_gpu_native_capture: 'Wayland NVIDIA copy avoidance; keep frames GPU resident.',
    ai_enabled: 'Let Auto Quality choose per-game profiles.',
    adaptive_bitrate_enabled: 'Let Auto Quality adjust bitrate for headless streams.',
  }

  return { label: labels[key] || key, description: descriptions[key] || '', aliases: [] }
}

describe('ranked settings search helper', () => {
  it('ranks exact setting labels over broad tab matches', () => {
    const results = rankSettingsSearchTabs(tabs, 'display output', optionFactory)

    expect(results[0]).toMatchObject({ tab: expect.objectContaining({ id: 'av' }), firstOptionKey: 'output_name' })
    expect(results[0].matchedOptionKeys).toEqual(expect.arrayContaining(['output_name']))
  })

  it('finds Linux operator aliases for headless display pairing discovery', () => {
    const results = rankSettingsSearchTabs(tabs, 'linux display pairing', optionFactory)

    expect(results[0].tab.id).toBe('av')
    expect(results[0].firstOptionKey).toBeTruthy()
  })

  it('scores Wayland NVIDIA copy queries against the GPU-native capture setting', () => {
    expect(scoreSettingsOption({
      key: 'linux_prefer_gpu_native_capture',
      label: 'GPU-native capture preference',
      description: 'Wayland NVIDIA copy avoidance and DMA-BUF/CUDA capture residency.',
      aliases: ['nvidia wayland copy', 'headless gpu native'],
    }, 'wayland nvidia copy')).toBeGreaterThan(0)
  })

  it('keeps Auto Quality discoverable from both AI and bitrate language', () => {
    const aiResults = rankSettingsSearchTabs(tabs, 'auto quality headless', optionFactory)
    const bitrateResults = rankSettingsSearchTabs(tabs, 'adaptive bitrate', optionFactory)

    expect(aiResults.map(result => result.tab.id)).toContain('ai')
    expect(bitrateResults[0]).toMatchObject({ tab: expect.objectContaining({ id: 'ai' }), firstOptionKey: 'adaptive_bitrate_enabled' })
  })
})
