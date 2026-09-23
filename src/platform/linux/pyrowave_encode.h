/**
 * @file src/platform/linux/pyrowave_encode.h
 * @brief PyroWave, an intra-only wavelet codec that runs as plain Vulkan compute.
 *
 * Everything that needs a Vulkan header lives behind this one, so the rest of Polaris can ask
 * about the codec without pulling the Vulkan API into every translation unit that includes it.
 */
#pragma once

// standard includes
#include <string>

namespace pyrowave_encode {

  /**
   * @brief The PyroWave version this binary is linked against, as MAJOR.MINOR.PATCH.
   *
   * Read from the library rather than from the build system. The two can disagree, and a codec
   * whose own ABI is unstable before 1.0 is exactly the one to ask rather than assume.
   */
  std::string api_version();

}  // namespace pyrowave_encode
