/**
 * @file src/platform/linux/pyrowave_encode.cpp
 * @brief PyroWave encode support.
 */
// PyroWave's header refuses to compile unless the Vulkan API is already declared, and says so with
// an #error.
//
// The system header, not the volk copy PyroWave builds against. volk declares every entry point as
// a function pointer variable, Polaris's own Vulkan encoder uses the ordinary prototypes, and with
// link time optimisation the compiler sees both and refuses: "function redeclared as variable".
// The types are the same either way; only the linkage of the entry points differs, and Polaris does
// not call any of them from here.
#include <vulkan/vulkan.h>

#include "pyrowave.h"

#include "src/platform/linux/pyrowave_encode.h"

namespace pyrowave_encode {

  std::string api_version() {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    pyrowave_get_api_version(&major, &minor, &patch);
    return std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch);
  }

}  // namespace pyrowave_encode
