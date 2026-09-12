// A libnvidia-encode.so.1 that reports whatever NVENC API version POLARIS_FAKE_NVENC_VERSION
// asks for, and forwards everything else to the real driver library. Lets a host on a new
// driver reproduce what a host on an older one sees.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void *real_library(void) {
  static void *handle;
  if (!handle) {
    const char *path = getenv("POLARIS_REAL_NVENC");
    handle = dlopen(path ? path : "libnvidia-encode.so.1", RTLD_NOW);
  }
  return handle;
}

int NvEncodeAPIGetMaxSupportedVersion(uint32_t *version) {
  unsigned major = 13, minor = 0;
  const char *wanted = getenv("POLARIS_FAKE_NVENC_VERSION");
  if (wanted) {
    sscanf(wanted, "%u.%u", &major, &minor);
  }
  *version = (major << 4) | minor;
  return 0;
}

int NvEncodeAPICreateInstance(void *function_list) {
  void *handle = real_library();
  if (!handle) {
    return 1;
  }
  int (*create)(void *) = (int (*)(void *)) dlsym(handle, "NvEncodeAPICreateInstance");
  return create ? create(function_list) : 1;
}
