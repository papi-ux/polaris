export function formatClientTimestamp(timestamp, locale, timeZone, missingLabel = 'Not recorded') {
  const value = Number(timestamp)
  if (!Number.isFinite(value) || value <= 0) return missingLabel

  // The API contract is Unix seconds, but tolerate ordinary JavaScript/JSON
  // millisecond epochs so they cannot render as absurd far-future years.
  const milliseconds = value >= 100_000_000_000 ? value : value * 1000
  const date = new Date(milliseconds)
  if (Number.isNaN(date.getTime())) return missingLabel

  const options = {
    dateStyle: 'medium',
    timeStyle: 'short',
  }
  if (timeZone) options.timeZone = timeZone

  return new Intl.DateTimeFormat(locale || undefined, options).format(date)
}
