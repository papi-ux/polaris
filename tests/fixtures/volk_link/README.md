# Host Vulkan and volk linkage

This fixture links the actual pinned volk target beside a fake Vulkan shared
library. One caller uses ordinary Vulkan function prototypes; another uses
volk's function-pointer variables and instance table, as Granite does. It checks
that both paths reach the fake loader before and after volk initialization and
finalization. It links no real Vulkan loader and opens no graphics device.

Run independently on Linux with LTO:

```sh
cmake -S tests/fixtures/volk_link -B build-volk-link
cmake --build build-volk-link
ctest --test-dir build-volk-link --output-on-failure
```

Configure a separate directory with `-DVOLK_NAMESPACE=OFF` to reproduce the old
LTO collision. Do not execute that binary if a compiler accepts the conflicting
symbols. When included in the host test build, this fixture reuses the actual
production volk target and its namespace configuration.

The host build also tests that the codec and scaler shader interfaces coexist.
The pinned headers originally reused both the `PyroWave::Shaders` type name and
include guards despite different layouts. A hash-checked build copy gives the
scaler interface a private namespace and updates its one C-wrapper use. Shader
and reflection data remain unchanged; canonical dependency files are untouched.
