/**
 * Prove a borrowed NVIDIA driver actually loads, for one ABI.
 *
 * The files arriving as read-only mounts is not the same as the loader being
 * able to open them: a missing dependency, a mismatched ABI or a stale loader
 * cache all present as a black screen inside a game instead of an error. This
 * runs once at worker start, names the library that failed, and never touches a
 * GPU device.
 */
#include <dlfcn.h>
#include <stdio.h>

static const char *const libraries[] = {
  "libcuda.so.1",
  "libEGL_nvidia.so.0",
  "libGLX_nvidia.so.0",
};

int main(void) {
  int failures = 0;
  for (unsigned index = 0; index < sizeof(libraries) / sizeof(libraries[0]); ++index) {
    void *handle = dlopen(libraries[index], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
      fprintf(stderr, "graphics-check: %s\n", dlerror());
      ++failures;
      continue;
    }
    dlclose(handle);
  }
  return failures == 0 ? 0 : 1;
}
