let currentGeneration = 0
let activeController = null

export function beginWebUiAuthProbeGeneration() {
  currentGeneration += 1
  activeController?.abort()
  activeController = null
  return currentGeneration
}

export function isCurrentWebUiAuthProbeGeneration(generation) {
  return generation === currentGeneration
}

export async function runWebUiAuthProbeForGeneration(generation, probe) {
  if (generation !== currentGeneration) {
    return { current: false }
  }

  activeController?.abort()
  const controller = new AbortController()
  activeController = controller

  try {
    const result = await probe(controller.signal)
    if (
      generation !== currentGeneration
      || activeController !== controller
      || controller.signal.aborted
    ) {
      return { current: false }
    }

    return { current: true, result }
  } catch (error) {
    if (
      generation !== currentGeneration
      || activeController !== controller
      || controller.signal.aborted
    ) {
      return { current: false }
    }
    throw error
  } finally {
    if (activeController === controller) {
      activeController = null
    }
  }
}
