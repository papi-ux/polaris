export function buildReadyCheckDisplay(checks = []) {
  const normalized = checks.filter(Boolean)
  const passing = normalized.filter((check) => check.ok).length
  const total = normalized.length
  const allPassing = total > 0 && passing === total
  const visibleChecks = allPassing ? [] : normalized.filter((check) => !check.ok)

  return {
    passing,
    total,
    allPassing,
    attention: visibleChecks.length,
    primaryIssue: visibleChecks[0] || null,
    visibleChecks,
  }
}
