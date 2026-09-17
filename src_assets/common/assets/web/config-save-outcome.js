// Whether a settings save still needs a restart, from the host's answer. The
// host applies the SteamGridDB key, the AI provider settings, adaptive bitrate
// and the trusted network while it runs and says so with
// restart_required: false. A host without that field predates live apply, so
// every save there needed one.
export function saveNeedsRestart(result) {
  return result?.restart_required !== false
}
