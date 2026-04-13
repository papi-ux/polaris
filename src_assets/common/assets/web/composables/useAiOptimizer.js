import { ref, readonly } from 'vue'

// Shared singleton state across all consumers
const status = ref(null)
const cache = ref([])
const devices = ref([])
const loading = ref(false)

export function useAiOptimizer() {
  async function fetchStatus() {
    try {
      const res = await fetch('./api/ai/status', { credentials: 'include' })
      if (res.ok) status.value = await res.json()
    } catch (e) {
      console.error('AI status fetch failed:', e)
    }
    return status.value
  }

  async function fetchCache() {
    try {
      const res = await fetch('./api/ai/cache', { credentials: 'include' })
      if (res.ok) {
        const data = await res.json()
        // API returns {"device:app": {...}} object — convert to array
        if (data && !Array.isArray(data)) {
          cache.value = Object.entries(data).map(([key, val]) => {
            const [device_name, ...rest] = key.split(':')
            return { device_name, app_name: rest.join(':'), cached_at: val.timestamp, ...val }
          })
        } else {
          cache.value = data || []
        }
      }
    } catch (e) {
      console.error('AI cache fetch failed:', e)
    }
    return cache.value
  }

  async function clearCache() {
    try {
      const res = await fetch('./api/ai/cache/clear', {
        method: 'POST',
        credentials: 'include',
        headers: { 'Content-Type': 'application/json' }
      })
      if (res.ok) {
        cache.value = []
        return true
      }
    } catch (e) {
      console.error('AI cache clear failed:', e)
    }
    return false
  }

  async function optimize(deviceName, appName, gpuInfo) {
    loading.value = true
    try {
      const res = await fetch('./api/ai/optimize', {
        method: 'POST',
        credentials: 'include',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          device_name: deviceName,
          app_name: appName || '',
          gpu_info: gpuInfo || 'NVIDIA RTX (NVENC)'
        })
      })
      if (res.ok) {
        const data = await res.json()
        return data
      }
    } catch (e) {
      console.error('AI optimize failed:', e)
    } finally {
      loading.value = false
    }
    return { status: false, error: 'Request failed' }
  }

  async function testConnection(configDraft, deviceName, appName, gpuInfo) {
    loading.value = true
    try {
      const res = await fetch('./api/ai/test', {
        method: 'POST',
        credentials: 'include',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          ...configDraft,
          device_name: deviceName || 'Test Device',
          app_name: appName || 'Test App',
          gpu_info: gpuInfo || 'NVIDIA RTX (NVENC)'
        })
      })
      if (res.ok) {
        return await res.json()
      }
    } catch (e) {
      console.error('AI connection test failed:', e)
    } finally {
      loading.value = false
    }
    return { status: false, error: 'Request failed' }
  }

  async function fetchDevices() {
    try {
      const res = await fetch('./api/devices', { credentials: 'include' })
      if (res.ok) {
        const data = await res.json()
        // API returns {name: {type, ...}} object — convert to array with name field
        if (data && !Array.isArray(data)) {
          devices.value = Object.entries(data).map(([name, dev]) => ({ name, ...dev }))
        } else {
          devices.value = data || []
        }
      }
    } catch (e) {
      console.error('Device DB fetch failed:', e)
    }
    return devices.value
  }

  async function getSuggestion(deviceName, appName) {
    try {
      const params = new URLSearchParams({ name: deviceName })
      if (appName) params.set('app', appName)
      const res = await fetch(`./api/devices/suggest?${params}`, { credentials: 'include' })
      if (res.ok) return await res.json()
    } catch (e) {
      console.error('Device suggestion fetch failed:', e)
    }
    return null
  }

  return {
    status: readonly(status),
    cache: readonly(cache),
    devices: readonly(devices),
    loading: readonly(loading),
    fetchStatus,
    fetchCache,
    clearCache,
    optimize,
    testConnection,
    fetchDevices,
    getSuggestion
  }
}
