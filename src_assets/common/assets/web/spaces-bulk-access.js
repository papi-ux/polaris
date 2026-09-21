// Select all or clear all for one Space, or for Desktop ('desktop'), as one host change. The page
// used to save one device at a time, and every save restarts the Spaces controller.
export async function setAccessForAll(profileId, allowed) {
  const response = await fetch('./api/multiseat/access/all', { method: 'POST', credentials: 'include',
    headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ profile_id: profileId, allowed }) })
  const result = await response.json().catch(() => null)
  const pending = response.status === 202
  if (!pending && (!response.ok || result?.status !== true)) {
    const refusal = new Error(result?.message || result?.error || '')
    // An older host has no such route. The caller says so rather than showing a bare 404.
    refusal.unsupported = response.status === 404
    throw refusal
  }
  return { pending }
}
