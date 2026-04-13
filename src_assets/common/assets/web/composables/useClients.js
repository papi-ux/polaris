import { ref } from 'vue'

/**
 * Permission bitmask mapping matching the C++ PERM enum in crypto.h.
 */
export const permissionMapping = {
  input_controller: 0x00000100,
  input_touch: 0x00000200,
  input_pen: 0x00000400,
  input_mouse: 0x00000800,
  input_kbd: 0x00001000,
  _all_inputs: 0x00001F00,
  clipboard_set: 0x00010000,
  clipboard_read: 0x00020000,
  file_upload: 0x00040000,
  file_dwnload: 0x00080000,
  server_cmd: 0x00100000,
  _all_operations: 0x001F0000,
  list: 0x01000000,
  view: 0x02000000,
  launch: 0x04000000,
  _allow_view: 0x06000000,
  _all_actions: 0x07000000,
  _default: 0x03000000,
  _no: 0x00000000,
  _all: 0x071F1F00
}

/**
 * Permission groups for the UI toggle display.
 */
export const permissionGroups = [
  { name: 'Action', permissions: [
    { name: 'list', suppressed_by: ['view', 'launch'] },
    { name: 'view', suppressed_by: ['launch'] },
    { name: 'launch', suppressed_by: [] }
  ] },
  { name: 'Operation', permissions: [
    { name: 'clipboard_set', suppressed_by: [] },
    { name: 'clipboard_read', suppressed_by: [] },
    { name: 'server_cmd', suppressed_by: [] }
  ] },
  { name: 'Input', permissions: [
    { name: 'input_controller', suppressed_by: [] },
    { name: 'input_touch', suppressed_by: [] },
    { name: 'input_pen', suppressed_by: [] },
    { name: 'input_mouse', suppressed_by: [] },
    { name: 'input_kbd', suppressed_by: [] }
  ] },
]

/**
 * Format a permission bitmask as a hex string "XX XX XX".
 */
export function permToStr(perm) {
  return [
    (perm >> 24) & 0xFF,
    (perm >> 16) & 0xFF,
    (perm >> 8) & 0xFF
  ].map(seg => seg.toString(16).toUpperCase().padStart(2, '0')).join(' ')
}

/**
 * Check if a specific permission is set in the bitmask.
 */
export function checkPermission(perm, permission) {
  return (perm & permissionMapping[permission]) !== 0
}

/**
 * Check if a permission toggle should be disabled (suppressed by a higher permission).
 */
export function isSuppressed(perm, permission, suppressed_by) {
  for (const suppressed of suppressed_by) {
    if (checkPermission(perm, suppressed)) return true
  }
  return false
}

/**
 * Composable for client management — fetching, updating, pairing, disconnecting.
 *
 * @returns Reactive client list and management functions.
 */
export function useClients() {
  const clients = ref([])
  const clientsLoaded = ref(false)
  const platform = ref('')

  function refreshClients() {
    return fetch('./api/clients/list', { credentials: 'include' })
      .then(r => r.json())
      .then(data => {
        if (data.status && data.named_certs && data.named_certs.length) {
          platform.value = data.platform
          clients.value = data.named_certs.map(({
            name, uuid, display_mode, perm, connected,
            do: _do, undo, allow_client_commands,
            always_use_virtual_display, enable_legacy_ordering
          }) => ({
            name, uuid, display_mode, perm: parseInt(perm, 10), connected,
            editing: false, wolSending: false, do: _do, undo,
            allow_client_commands, enable_legacy_ordering, always_use_virtual_display
          }))
        } else {
          clients.value = []
        }
      })
      .catch(e => console.error('Failed to refresh clients:', e))
      .finally(() => { clientsLoaded.value = true })
  }

  function saveClient(client) {
    const editedClient = {
      uuid: client.uuid,
      name: client.editName,
      display_mode: client.editDisplayMode.trim(),
      allow_client_commands: client.editAllowClientCommands,
      enable_legacy_ordering: client.editEnableLegacyOrdering,
      always_use_virtual_display: client.editAlwaysUseVirtualDisplay,
      perm: client.editPerm & permissionMapping._all,
      do: (client.edit_do || []).reduce((filtered, { cmd: _cmd, elevated }) => {
        const cmd = _cmd.trim()
        if (cmd) filtered.push({ cmd, elevated })
        return filtered
      }, []),
      undo: (client.edit_undo || []).reduce((filtered, { cmd: _cmd, elevated }) => {
        const cmd = _cmd.trim()
        if (cmd) filtered.push({ cmd, elevated })
        return filtered
      }, [])
    }
    return fetch('./api/clients/update', {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
      body: JSON.stringify(editedClient)
    })
      .then(r => r.json())
      .then(r => { if (r.status) refreshClients() })
      .finally(() => setTimeout(refreshClients, 1000))
  }

  function unpairSingle(uuid) {
    return fetch('./api/clients/unpair', {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
      body: JSON.stringify({ uuid })
    }).then(() => refreshClients())
  }

  function unpairAll() {
    return fetch('./api/clients/unpair-all', {
      credentials: 'include',
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
    }).then(r => r.json()).then(() => refreshClients())
  }

  function disconnectClient(uuid) {
    return fetch('./api/clients/disconnect', {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
      body: JSON.stringify({ uuid })
    }).finally(() => setTimeout(refreshClients, 1000))
  }

  function sendWol(client) {
    if (!client.name || client.wolSending) return Promise.resolve()
    client.wolSending = true
    return fetch('./api/clients/wol', {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
      body: JSON.stringify({ name: client.name })
    })
      .then(r => r.json())
      .finally(() => { client.wolSending = false })
  }

  // Client display profiles
  const profiles = ref({})

  function refreshProfiles() {
    return fetch('./api/clients/profiles', { credentials: 'include' })
      .then(r => r.json())
      .then(data => { profiles.value = data })
      .catch(() => {})
  }

  function saveProfile(name, profile) {
    return fetch('./api/clients/profiles/update', {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
      body: JSON.stringify({ name, ...profile })
    }).then(r => r.json()).then(() => refreshProfiles())
  }

  function deleteProfile(name) {
    return fetch('./api/clients/profiles/delete', {
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
      body: JSON.stringify({ name })
    }).then(r => r.json()).then(() => refreshProfiles())
  }

  return {
    clients,
    clientsLoaded,
    platform,
    profiles,
    refreshClients,
    refreshProfiles,
    saveClient,
    saveProfile,
    deleteProfile,
    unpairSingle,
    unpairAll,
    disconnectClient,
    sendWol,
  }
}
