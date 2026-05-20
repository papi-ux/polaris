export function buildReadyCheckDisplay(checks = []) {
  const normalized = checks.filter(Boolean)
  const passing = normalized.filter((check) => check.ok).length
  const total = normalized.length
  const allPassing = total > 0 && passing === total

  return {
    passing,
    total,
    allPassing,
    visibleChecks: allPassing ? [] : normalized.filter((check) => !check.ok),
  }
}
