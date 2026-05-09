export const PREVIEW_REFRESH_MS = 2000
export const PREVIEW_FAILURE_BACKOFF_MS = 15000
export const PREVIEW_MAX_BACKOFF_MS = 60000
export const PREVIEW_MAX_CONSECUTIVE_FAILURES = 3

export function nextPreviewBackoffMs(
  currentMs,
  {
    failureBackoffMs = PREVIEW_FAILURE_BACKOFF_MS,
    maxBackoffMs = PREVIEW_MAX_BACKOFF_MS,
  } = {},
) {
  return Math.min(
    Math.max(failureBackoffMs, currentMs * 2),
    maxBackoffMs,
  )
}

export function shouldPausePreviewPolling(
  consecutiveFailures,
  maxFailures = PREVIEW_MAX_CONSECUTIVE_FAILURES,
) {
  return consecutiveFailures >= maxFailures
}
