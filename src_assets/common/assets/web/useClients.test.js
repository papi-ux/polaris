import { describe, expect, it } from 'vitest'
import {
  permissionMapping,
  permissionPresetKey,
  permissionPresetMask,
} from './composables/useClients'

describe('client permission presets', () => {
  it('maps Standard Access to the existing default permission mask', () => {
    expect(permissionPresetMask('standard')).toBe(permissionMapping._default)
    expect(permissionPresetKey(permissionMapping._default)).toBe('standard')
  })

  it('maps Game Control to launch, view, list, and all input permissions only', () => {
    const mask = permissionPresetMask('game_control')

    expect(mask).toBe(permissionMapping._game_control)
    expect(mask & permissionMapping._all_actions).toBe(permissionMapping._all_actions)
    expect(mask & permissionMapping._all_inputs).toBe(permissionMapping._all_inputs)
    expect(mask & permissionMapping._all_operations).toBe(0)
    expect(permissionPresetKey(mask)).toBe('game_control')
  })

  it('maps Full Control to the existing full permission mask', () => {
    expect(permissionPresetMask('full')).toBe(permissionMapping._all)
    expect(permissionPresetKey(permissionMapping._all)).toBe('full')
  })
})
