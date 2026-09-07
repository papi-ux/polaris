/** @file src/platform/linux/encoder_probe_driver_proof.h
 * Retain the actual userspace provider objects while a capability probe is live.
 */
#pragma once

#include "encoder_probe_identity.h"
#include <dlfcn.h>
#include <atomic>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <set>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace platf::encoder_probe_identity {
  struct mapped_file_t {
    std::uintptr_t start = 0, end = 0;
    std::uint64_t offset = 0, inode = 0;
    unsigned device_major = 0, device_minor = 0;
  };

  inline std::optional<std::vector<mapped_file_t>> mapped_files() {
    std::ifstream input("/proc/self/maps");
    if (!input) return std::nullopt;
    std::vector<mapped_file_t> mappings;
    std::string line;
    while (std::getline(input, line)) {
      if (mappings.size() >= 16384 || line.size() > 8192) return std::nullopt;
      mapped_file_t map;
      char permissions[5] {};
      unsigned long start, end, offset, inode;
      if (sscanf(line.c_str(), "%lx-%lx %4s %lx %x:%x %lu", &start, &end, permissions,
                 &offset, &map.device_major, &map.device_minor, &inode) != 7 || start >= end) {
        return std::nullopt;
      }
      map.start = start; map.end = end; map.offset = offset; map.inode = inode;
      mappings.push_back(map);
    }
    if (input.bad() || mappings.empty()) return std::nullopt;
    return mappings;
  }

  struct load_segment_t { std::uintptr_t start, end; std::uint64_t offset; };

  inline bool mapped_to_file(const std::vector<load_segment_t> &segments,
                             const mapped_file_t &identity,
                             const std::vector<mapped_file_t> &mappings) {
    for (const auto &segment : segments) {
      auto position = segment.start;
      while (position < segment.end) {
        const auto found = std::find_if(mappings.begin(), mappings.end(), [&](const auto &map) {
          return map.start <= position && position < map.end;
        });
        if (found == mappings.end() || found->inode != identity.inode ||
            found->device_major != identity.device_major || found->device_minor != identity.device_minor ||
            found->offset + position - found->start != segment.offset + position - segment.start) return false;
        position = std::min(segment.end, found->end);
      }
    }
    return !segments.empty();
  }

  struct retained_object_t {
    void *handle = nullptr;
    int file = -1;
    fs::path path;
    std::string identity;
    std::uintptr_t address = 0;
    std::vector<load_segment_t> segments;
    mapped_file_t mapped_identity;
    ~retained_object_t() {
      if (handle) dlclose(handle);
      if (file >= 0) close(file);
    }
    bool current(const std::vector<mapped_file_t> &mappings) const {
      struct stat pinned {}, named_status {};
      const auto named = file_identity(path, S_IFREG);
      // The named file is an invalidation signal, not the runtime ELF identity.
      // Btrfs VMA and stat device numbers can legitimately differ. Keep both
      // tuples separate; the retained loader object owns the mapped identity.
      return named && *named == identity && fstat(file, &pinned) == 0 &&
             stat(path.c_str(), &named_status) == 0 && pinned.st_dev == named_status.st_dev &&
             pinned.st_ino == named_status.st_ino && mapped_to_file(segments, mapped_identity, mappings);
    }
  };

  inline std::pair<std::uint64_t, std::uint64_t> loader_epoch() {
    std::pair<std::uint64_t, std::uint64_t> epoch;
    dl_iterate_phdr([](dl_phdr_info *info, std::size_t, void *opaque) {
      *static_cast<decltype(epoch) *>(opaque) = {info->dlpi_adds, info->dlpi_subs};
      return 1;
    }, &epoch);
    return epoch;
  }

  inline std::optional<std::string> provider_selection_key() {
    // Nonstandard loader/provider overrides need an explicit observer; their
    // unchanged text does not prove the files they select remain unchanged.
    for (const char *name : {"LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT", "LIBVA_DRIVERS_PATH",
         "LIBVA_DRIVER_NAME", "VK_ICD_FILENAMES", "VK_DRIVER_FILES", "VK_ADD_DRIVER_FILES",
         "VK_LOADER_DRIVERS_SELECT", "VK_LOADER_DRIVERS_DISABLE", "__EGL_VENDOR_LIBRARY_FILENAMES",
         "__EGL_VENDOR_LIBRARY_DIRS", "__GLX_VENDOR_LIBRARY_NAME", "GBM_BACKEND", "GBM_BACKENDS_PATH",
         "MESA_LOADER_DRIVER_OVERRIDE", "GALLIUM_DRIVER", "DRI_PRIME", "CUDA_VISIBLE_DEVICES",
         "NVIDIA_VISIBLE_DEVICES", "__NV_PRIME_RENDER_OFFLOAD", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH",
         "VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_LOADER_LAYERS_DISABLE", "VK_LOADER_LAYERS_ALLOW",
         "CUDA_MODULE_LOADING", "CUDA_FORCE_PTX_JIT", "CUDA_DISABLE_PTX_JIT", "CUDA_CACHE_PATH", "CUDA_CACHE_DISABLE"}) {
      const auto value = getenv(name);
      if (value && *value) return std::nullopt;
    }
    try {
      std::set<fs::path> roots {"/etc", "/etc/xdg", "/usr/share", "/usr/local/share"};
      std::ostringstream environment;
      const auto home = getenv("HOME");
      if (!home || !fs::path(home).is_absolute()) return std::nullopt;
      roots.insert(fs::path(home) / ".config");
      roots.insert(fs::path(home) / ".local/share");
      for (const char *name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CONFIG_DIRS", "XDG_DATA_DIRS"}) {
        const auto value = getenv(name);
        environment << std::quoted(name) << ':' << (value != nullptr) << std::quoted(value ? value : "");
        if (value && *value) {
          std::istringstream values(value);
          std::string path;
          while (std::getline(values, path, ':')) {
            if (!fs::path(path).is_absolute() || roots.size() >= 32) return std::nullopt;
            roots.insert(path);
          }
        }
      }
      const auto loader = file_identity("/etc/ld.so.cache", S_IFREG);
      if (!loader) return std::nullopt;
      std::ostringstream key;
      key << std::quoted(*loader) << std::quoted(environment.str());
      std::size_t total_bytes = 0;
      for (const auto &root : roots) {
        for (const auto suffix : {"vulkan/icd.d", "vulkan/implicit_layer.d", "vulkan/explicit_layer.d", "glvnd/egl_vendor.d"}) {
          const auto directory = root / suffix;
          key << std::quoted(directory.string());
          if (!fs::exists(directory)) { key << "missing"; continue; }
          std::vector<fs::path> files;
          for (const auto &entry : fs::directory_iterator(directory)) {
            if (entry.path().extension() == ".json") files.push_back(entry.path());
            if (files.size() > 128) return std::nullopt;
          }
          std::sort(files.begin(), files.end());
          for (const auto &file : files) {
            const auto identity = file_identity(file, S_IFREG);
            // Khronos validation-layer manifests include a large settings
            // schema. Bound both each manifest and the complete observation.
            const auto contents = bounded_file(file, 256 * 1024);
            if (!identity || !contents) return std::nullopt;
            total_bytes += contents->size();
            if (total_bytes > 4 * 1024 * 1024) return std::nullopt;
            key << std::quoted(file.string()) << std::quoted(*identity) << std::quoted(*contents);
          }
        }
      }
      return key.str();
    } catch (...) { return std::nullopt; }
  }

  class driver_proof_t {
  public:
    void include_capture_route(const std::string &route) {
      if (!route.starts_with("private-wlr/")) valid = false;
      else capture_routes.insert(route);
    }

    bool has_capture_routes() const { return valid && !capture_routes.empty(); }

    // Called before the successful validation's local encoder/device owners
    // die. RTLD_NOLOAD never selects or loads another provider or constructor.
    bool include_live_objects(
#ifdef POLARIS_TESTS
      const std::function<void()> &after_epoch_for_tests = {}
#endif
    ) {
      if (!valid) return false;
      const auto epoch_before = loader_epoch();
#ifdef POLARIS_TESTS
      if (after_epoch_for_tests) after_epoch_for_tests();
#endif
      const auto mappings = mapped_files();
      const auto page = sysconf(_SC_PAGESIZE);
      if (!mappings || page <= 0) return valid = false;
      struct context_t {
        driver_proof_t &proof;
        const std::vector<mapped_file_t> &maps;
        std::uintptr_t page;
      } context {*this, *mappings, static_cast<std::uintptr_t>(page)};
      dl_iterate_phdr([](dl_phdr_info *info, std::size_t, void *opaque) {
        auto &context = *static_cast<context_t *>(opaque);
        auto &proof = context.proof;
        try {
          const std::string name = info->dlpi_name;
          if (name == "linux-vdso.so.1" || name == "linux-gate.so.1") return 0;
          const auto path = fs::canonical(name.empty() ? fs::path("/proc/self/exe") : fs::path(name));
          for (const auto &object : proof.objects) {
            if (object->path == path && object->address == info->dlpi_addr) {
              if (!object->current(context.maps)) proof.valid = false;
              return proof.valid ? 0 : 1;
            }
          }
          if (proof.objects.size() >= 512) { proof.valid = false; return 1; }
          auto object = std::make_unique<retained_object_t>();
          object->path = path;
          object->address = info->dlpi_addr;
          object->file = open(path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
          const auto identity = file_identity(path, S_IFREG);
          if (object->file < 0 || !identity) { proof.valid = false; return 1; }
          object->identity = *identity;
          if (!name.empty()) {
            object->handle = dlopen(name.c_str(), RTLD_LAZY | RTLD_NOLOAD | RTLD_LOCAL);
            struct link_map *link = nullptr;
            if (!object->handle || dlinfo(object->handle, RTLD_DI_LINKMAP, &link) ||
                !link || link->l_addr != info->dlpi_addr) { proof.valid = false; return 1; }
          }
          for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
            const auto &header = info->dlpi_phdr[i];
            if (header.p_type != PT_LOAD || header.p_filesz == 0) continue;
            const auto offset = header.p_offset - header.p_offset % context.page;
            const auto start = info->dlpi_addr + header.p_vaddr - header.p_vaddr % context.page;
            const auto end = info->dlpi_addr + header.p_vaddr + header.p_filesz;
            if (end <= start) { proof.valid = false; return 1; }
            object->segments.push_back({start, end, offset});
          }
          const auto first_map = std::find_if(context.maps.begin(), context.maps.end(), [&](const auto &map) {
            return !object->segments.empty() && map.start <= object->segments.front().start &&
                   object->segments.front().start < map.end;
          });
          if (first_map == context.maps.end() || first_map->inode == 0) { proof.valid = false; return 1; }
          object->mapped_identity = *first_map;
          if (!object->current(context.maps)) { proof.valid = false; return 1; }
          proof.objects.push_back(std::move(object));
          return 0;
        } catch (...) { proof.valid = false; return 1; }
      }, &context);
      epoch = loader_epoch();
      if (epoch != epoch_before) valid = false;
      return valid;
    }

    std::optional<std::string> current_key() const {
      if (!valid || objects.empty() || loader_epoch() != epoch) return std::nullopt;
      const auto maps = mapped_files();
      if (!maps) return std::nullopt;
      std::vector<std::string> entries;
      for (const auto &object : objects) {
        if (!object->current(*maps)) return std::nullopt;
        std::ostringstream key;
        key << std::quoted(object->path.string()) << std::quoted(object->identity) << ':' << object->address
            << ':' << object->mapped_identity.device_major << ':' << object->mapped_identity.device_minor
            << ':' << object->mapped_identity.inode;
        entries.push_back(key.str());
      }
      if (loader_epoch() != epoch) return std::nullopt;
      std::sort(entries.begin(), entries.end());
      std::ostringstream result;
      result << generation << '/' << epoch.first << '/' << epoch.second;
      for (const auto &route : capture_routes) result << std::quoted(route);
      for (const auto &entry : entries) result << std::quoted(entry);
      return result.str();
    }

    bool contains_provider(std::string_view filename_prefix) const {
      return std::any_of(objects.begin(), objects.end(), [&](const auto &object) {
        return object->path.filename().string().starts_with(filename_prefix);
      });
    }

  private:
    inline static std::atomic_uint64_t next_generation {0};
    const std::uint64_t generation = ++next_generation;
    bool valid = true;
    std::set<std::string> capture_routes;
    std::pair<std::uint64_t, std::uint64_t> epoch {};
    std::vector<std::unique_ptr<retained_object_t>> objects;
  };
}  // namespace platf::encoder_probe_identity
