// Remove artwork and Find artwork again for one app entry on the Apps page.
// The host answers {status, uuid, automatic_lookup, error?}. lookupOff is reported
// only when the host said what the lookup state is now, so a failed request never
// flips the page's state on a guess.
async function postArtwork(path, uuid) {
  let response
  try {
    response = await fetch(path, {
      method: 'POST',
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ uuid }),
    })
  } catch {
    return { ok: false, error: '' }
  }
  let body = null
  try { body = await response.json() } catch { body = null }
  if (!body || typeof body !== 'object') return { ok: false, error: '' }
  const result = { ok: response.ok && body.status === true, error: typeof body.error === 'string' ? body.error : '' }
  if (typeof body.automatic_lookup === 'boolean') result.lookupOff = !body.automatic_lookup
  return result
}

export const removeAppArtwork = (uuid) => postArtwork('./api/apps/artwork/remove', uuid)
export const findAppArtwork = (uuid) => postArtwork('./api/apps/artwork/find', uuid)

// The uuids GET /api/apps lists under artwork_lookup_off.
export function artworkLookupOffSet(appsResponse) {
  const list = Array.isArray(appsResponse?.artwork_lookup_off) ? appsResponse.artwork_lookup_off : []
  return new Set(list.filter((uuid) => typeof uuid === 'string' && uuid))
}
