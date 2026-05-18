/**
 * @file src/platform/linux/libva_compat.cpp
 * @brief Compatibility symbols for older libva runtimes.
 */
// standard includes
#include <cstdint>

extern "C" {
#include <dlfcn.h>
#include <va/va.h>
}

extern "C" VAStatus
vaMapBuffer2(VADisplay dpy, VABufferID buf_id, void **pbuf, uint32_t flags) {
  using va_map_buffer2_fn = VAStatus (*)(VADisplay, VABufferID, void **, uint32_t);

  static auto real_va_map_buffer2 =
    reinterpret_cast<va_map_buffer2_fn>(dlsym(RTLD_NEXT, "vaMapBuffer2"));

  if (real_va_map_buffer2) {
    return real_va_map_buffer2(dpy, buf_id, pbuf, flags);
  }

  return vaMapBuffer(dpy, buf_id, pbuf);
}
