const DEFAULT_RESTART_URL = './api/restart'
const DEFAULT_HEALTH_URL = './api/config'
const DEFAULT_READY_POLL_ATTEMPTS = 20
const DEFAULT_READY_POLL_INITIAL_DELAY_MS = 1000
const DEFAULT_READY_POLL_INTERVAL_MS = 1000

function defaultSleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms))
}

function defaultReload() {
  window.location.reload()
}

export async function waitForHostReady({
  fetchImpl = fetch,
  healthUrl = DEFAULT_HEALTH_URL,
  sleep = defaultSleep,
  readyPollAttempts = DEFAULT_READY_POLL_ATTEMPTS,
  readyPollInitialDelayMs = DEFAULT_READY_POLL_INITIAL_DELAY_MS,
  readyPollIntervalMs = DEFAULT_READY_POLL_INTERVAL_MS,
} = {}) {
  if (readyPollInitialDelayMs > 0) {
    await sleep(readyPollInitialDelayMs)
  }

  for (let attempt = 1; attempt <= readyPollAttempts; attempt++) {
    try {
      const response = await fetchImpl(healthUrl, {
        credentials: 'include',
        cache: 'no-store',
      })

      if (response.ok) {
        return { ready: true, attempts: attempt }
      }
    } catch {
      // Expected while the HTTPS listener is stopping/starting.
    }

    if (attempt < readyPollAttempts && readyPollIntervalMs > 0) {
      await sleep(readyPollIntervalMs)
    }
  }

  return { ready: false, attempts: readyPollAttempts }
}

export async function requestHostRestart({
  fetchImpl = fetch,
  restartUrl = DEFAULT_RESTART_URL,
  healthUrl = DEFAULT_HEALTH_URL,
  sleep = defaultSleep,
  reload = defaultReload,
  onReady = () => {},
  onTimeout = () => {},
  reloadOnTimeout = false,
  readyPollAttempts = DEFAULT_READY_POLL_ATTEMPTS,
  readyPollInitialDelayMs = DEFAULT_READY_POLL_INITIAL_DELAY_MS,
  readyPollIntervalMs = DEFAULT_READY_POLL_INTERVAL_MS,
} = {}) {
  const response = await fetchImpl(restartUrl, {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
  })

  if (!response.ok) {
    throw new Error(`Restart request failed with HTTP ${response.status}`)
  }

  const readiness = await waitForHostReady({
    fetchImpl,
    healthUrl,
    sleep,
    readyPollAttempts,
    readyPollInitialDelayMs,
    readyPollIntervalMs,
  })

  if (readiness.ready) {
    onReady()
  } else {
    onTimeout()
    if (reloadOnTimeout) {
      reload()
    }
  }

  return {
    accepted: true,
    ready: readiness.ready,
    attempts: readiness.attempts,
  }
}
