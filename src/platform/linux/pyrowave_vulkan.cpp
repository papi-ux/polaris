/**
 * @file src/platform/linux/pyrowave_vulkan.cpp
 * @brief The Vulkan device Polaris owns and PyroWave borrows.
 */
#include "src/platform/linux/pyrowave_vulkan.h"

// standard includes
#include <dlfcn.h>

#include <vector>

// local includes
#include "src/logging.h"

using namespace std::literals;

namespace pyrowave_encode {

  namespace {

    /**
     * Every entry point this file calls, in one place.
     *
     * Two names each because the Vulkan spelling is CamelCase and Polaris is not, and because the
     * string to resolve is the Vulkan one.
     */
#define POLARIS_VK_GLOBAL_FNS(X) \
  X(create_instance, CreateInstance)

#define POLARIS_VK_INSTANCE_FNS(X)                            \
  X(destroy_instance, DestroyInstance)                        \
  X(enumerate_physical_devices, EnumeratePhysicalDevices)      \
  X(get_physical_device_properties, GetPhysicalDeviceProperties) \
  X(get_physical_device_features2, GetPhysicalDeviceFeatures2) \
  X(get_queue_family_properties, GetPhysicalDeviceQueueFamilyProperties) \
  X(create_device, CreateDevice)                              \
  X(destroy_device, DestroyDevice)                            \
  X(get_device_queue, GetDeviceQueue)                         \
  X(get_device_proc_addr, GetDeviceProcAddr)

    /** The handful of entry points needed to stand a device up. Resolved, never linked. */
    struct loader_t {
      void *library = nullptr;
      PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;

#define POLARIS_VK_DECLARE(member, name) PFN_vk##name member = nullptr;
      POLARIS_VK_GLOBAL_FNS(POLARIS_VK_DECLARE)
      POLARIS_VK_INSTANCE_FNS(POLARIS_VK_DECLARE)
#undef POLARIS_VK_DECLARE

      bool open() {
        if (get_instance_proc_addr) {
          return true;
        }
        // The soname first, because that is what a machine that can run Vulkan has. The unversioned
        // name is a development symlink and may well be absent.
        for (const auto *candidate : {"libvulkan.so.1", "libvulkan.so"}) {
          library = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
          if (library) {
            break;
          }
        }
        if (!library) {
          BOOST_LOG(info) << "PyroWave: no Vulkan loader on this host ("sv << dlerror() << ')';
          return false;
        }
        get_instance_proc_addr =
          reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
        if (!get_instance_proc_addr) {
          BOOST_LOG(warning) << "PyroWave: the Vulkan loader has no vkGetInstanceProcAddr"sv;
          return false;
        }

#define POLARIS_VK_RESOLVE_GLOBAL(member, name) \
  member = reinterpret_cast<PFN_vk##name>(get_instance_proc_addr(VK_NULL_HANDLE, "vk" #name));
        POLARIS_VK_GLOBAL_FNS(POLARIS_VK_RESOLVE_GLOBAL)
#undef POLARIS_VK_RESOLVE_GLOBAL

        return create_instance != nullptr;
      }

      void resolve_instance(VkInstance instance) {
#define POLARIS_VK_RESOLVE_INSTANCE(member, name) \
  member = reinterpret_cast<PFN_vk##name>(get_instance_proc_addr(instance, "vk" #name));
        POLARIS_VK_INSTANCE_FNS(POLARIS_VK_RESOLVE_INSTANCE)
#undef POLARIS_VK_RESOLVE_INSTANCE
      }

      /// Whether every instance level pointer came back. A null one here is a driver bug, not a choice.
      bool instance_complete() const {
        return destroy_instance && enumerate_physical_devices && get_physical_device_properties &&
               get_physical_device_features2 && get_queue_family_properties && create_device &&
               destroy_device && get_device_queue && get_device_proc_addr;
      }
    };

    loader_t loader;

    /**
     * How much this host would rather encode on one GPU than another.
     *
     * A discrete GPU first, because that is where the frames already are on the hosts that run this
     * codec, and because the wavelet transform is bandwidth bound. Anything that answers at all
     * comes ahead of nothing.
     */
    int preference_of(VkPhysicalDeviceType type) {
      switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
          return 3;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
          return 2;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
          return 1;
        default:
          return 0;
      }
    }

    /**
     * Ask a GPU what it supports, with the chain wired up so the answer can be handed straight back.
     *
     * The same structs serve both ways round in Vulkan, and that is the point here: what a driver
     * reports as supported is what the device is then created with, minus a short list. Anything
     * narrower is a configuration nobody upstream runs.
     */
    void query_features(VkPhysicalDevice candidate, VkPhysicalDeviceFeatures2 &features,
                        VkPhysicalDeviceVulkan11Features &v11, VkPhysicalDeviceVulkan12Features &v12,
                        VkPhysicalDeviceVulkan13Features &v13) {
      v13 = {};
      v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

      v12 = {};
      v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
      v12.pNext = &v13;

      v11 = {};
      v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
      v11.pNext = &v12;

      features = {};
      features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
      features.pNext = &v11;

      loader.get_physical_device_features2(candidate, &features);
    }

    /**
     * Whether a GPU has the features the encoder's shaders are written against.
     *
     * Asked rather than assumed, because vkCreateDevice fails outright when a feature it is handed
     * is unsupported, and on a host with more than one GPU that failure should skip a device rather
     * than end the search. The list is the one PyroWave's own header names, plus timeline semaphores,
     * which it does not name and its Granite core cannot do without: the path Granite takes when a
     * device has no timeline semaphore asks for a legacy one while already holding the device lock,
     * and deadlocks on itself. Core in 1.2 and supported everywhere that matters, so requiring it
     * costs nothing and beats hanging the encode thread.
     */
    bool has_what_the_codec_needs(const VkPhysicalDeviceFeatures2 &features,
                                  const VkPhysicalDeviceVulkan11Features &v11,
                                  const VkPhysicalDeviceVulkan12Features &v12,
                                  const VkPhysicalDeviceVulkan13Features &v13) {
      return features.features.shaderInt16 && v11.storageBuffer16BitAccess &&
             v12.storageBuffer8BitAccess && v12.timelineSemaphore && v13.subgroupSizeControl &&
             v13.computeFullSubgroups;
    }

    /**
     * The first queue family that can do both.
     *
     * Compute because every shader the codec runs is a compute shader, and graphics because its
     * create info says at least one graphics capable queue has to be there. Vulkan guarantees a
     * family with both, so asking for it costs nothing and catches a driver that lied.
     */
    uint32_t graphics_and_compute_family(VkPhysicalDevice candidate) {
      uint32_t count = 0;
      loader.get_queue_family_properties(candidate, &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      loader.get_queue_family_properties(candidate, &count, families.data());

      constexpr VkQueueFlags wanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
      for (uint32_t i = 0; i < count; i++) {
        if ((families[i].queueFlags & wanted) == wanted) {
          return i;
        }
      }
      return VK_QUEUE_FAMILY_IGNORED;
    }

  }  // namespace

  PFN_vkVoidFunction vk_device_t::instance_fn(const char *name) const {
    if (!loader.get_instance_proc_addr || instance == VK_NULL_HANDLE) {
      return nullptr;
    }
    return loader.get_instance_proc_addr(instance, name);
  }

  PFN_vkVoidFunction vk_device_t::device_fn(const char *name) const {
    if (!loader.get_device_proc_addr || device == VK_NULL_HANDLE) {
      return nullptr;
    }
    return loader.get_device_proc_addr(device, name);
  }

  void vk_device_t::destroy() {
    // The codec's device holds the Vulkan one, and its own header says every encoder and decoder has
    // to be gone before it is. That is the caller's business; this end of it is only the order.
    if (codec) {
      pyrowave_device_destroy(codec);
      codec = nullptr;
    }
    if (device != VK_NULL_HANDLE && loader.destroy_device) {
      loader.destroy_device(device, nullptr);
      device = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE && loader.destroy_instance) {
      loader.destroy_instance(instance, nullptr);
      instance = VK_NULL_HANDLE;
    }
    physical_device = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    queue_family = VK_QUEUE_FAMILY_IGNORED;
    gpu_name.clear();
  }

  bool vk_device_t::create() {
    destroy();

    if (!loader.open()) {
      return false;
    }

    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "polaris";
    application_info.pEngineName = "polaris";
    // The codec asks for 1.3, where subgroup size control is core.
    application_info.apiVersion = VK_API_VERSION_1_3;

    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;

    // No instance extensions. A host encodes and hands the bytes to the network; nothing here
    // presents, so there is no surface to ask for.
    const auto instance_result = loader.create_instance(&instance_info, nullptr, &instance);
    if (instance_result != VK_SUCCESS) {
      BOOST_LOG(info) << "PyroWave: no Vulkan 1.3 instance on this host (result "sv
                      << static_cast<int>(instance_result) << ')';
      instance = VK_NULL_HANDLE;
      return false;
    }
    loader.resolve_instance(instance);
    if (!loader.instance_complete()) {
      BOOST_LOG(warning) << "PyroWave: this Vulkan loader is missing core 1.3 entry points"sv;
      destroy();
      return false;
    }

    uint32_t count = 0;
    loader.enumerate_physical_devices(instance, &count, nullptr);
    if (count == 0) {
      BOOST_LOG(info) << "PyroWave: this host has a Vulkan loader and no Vulkan devices"sv;
      destroy();
      return false;
    }
    std::vector<VkPhysicalDevice> candidates(count);
    loader.enumerate_physical_devices(instance, &count, candidates.data());

    // The best of what is here rather than the first of it. A host with a discrete GPU and an
    // integrated one enumerates both, in whatever order the loader feels like, and the frames are on
    // the discrete one.
    int best = -1;
    VkPhysicalDeviceProperties chosen = {};
    for (auto candidate : candidates) {
      VkPhysicalDeviceProperties properties = {};
      loader.get_physical_device_properties(candidate, &properties);

      if (properties.apiVersion < VK_API_VERSION_1_3) {
        BOOST_LOG(debug) << "PyroWave: skipping "sv << properties.deviceName
                         << ", it speaks Vulkan below 1.3"sv;
        continue;
      }
      VkPhysicalDeviceFeatures2 supported = {};
      VkPhysicalDeviceVulkan11Features supported11 = {};
      VkPhysicalDeviceVulkan12Features supported12 = {};
      VkPhysicalDeviceVulkan13Features supported13 = {};
      query_features(candidate, supported, supported11, supported12, supported13);
      if (!has_what_the_codec_needs(supported, supported11, supported12, supported13)) {
        BOOST_LOG(debug) << "PyroWave: skipping "sv << properties.deviceName
                         << ", it lacks a feature the encoder's shaders need"sv;
        continue;
      }
      const auto family = graphics_and_compute_family(candidate);
      if (family == VK_QUEUE_FAMILY_IGNORED) {
        BOOST_LOG(debug) << "PyroWave: skipping "sv << properties.deviceName
                         << ", no queue family does both graphics and compute"sv;
        continue;
      }

      const auto preference = preference_of(properties.deviceType);
      if (preference > best) {
        best = preference;
        physical_device = candidate;
        queue_family = family;
        chosen = properties;
      }
    }

    if (physical_device == VK_NULL_HANDLE) {
      BOOST_LOG(info) << "PyroWave: no GPU on this host has what the encoder needs"sv;
      destroy();
      return false;
    }
    gpu_name = chosen.deviceName;

    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    // Everything this GPU reports, handed back as what to enable, which is how the codec's own
    // Granite context builds a device: it copies the supported set and switches off a short list.
    // Doing less is not a smaller device, it is a device shaped like nothing upstream tests, and
    // Granite's fallbacks for the features it finds missing are where the bodies are buried.
    //
    // The chain has to point at these members rather than at anything on the stack. PyroWave's C API
    // says the create infos and everything inside them outlive the device it makes from them.
    query_features(physical_device, features, vulkan11, vulkan12, vulkan13);

    // The short list. Bounds checking every buffer access costs throughput and buys a host encoder
    // nothing; the rest is for capture and replay tooling, or for a feature Granite itself turns off.
    features.features.robustBufferAccess = VK_FALSE;
    vulkan11.protectedMemory = VK_FALSE;
    vulkan11.multiviewGeometryShader = VK_FALSE;
    vulkan11.multiviewTessellationShader = VK_FALSE;
    vulkan12.bufferDeviceAddressCaptureReplay = VK_FALSE;
    vulkan12.bufferDeviceAddressMultiDevice = VK_FALSE;
    vulkan13.privateData = VK_FALSE;

    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;

    const auto device_result = loader.create_device(physical_device, &device_info, nullptr, &device);
    if (device_result != VK_SUCCESS) {
      BOOST_LOG(warning) << "PyroWave: "sv << gpu_name << " refused a device with the features it "sv
                         << "reported (result "sv << static_cast<int>(device_result) << ')';
      device = VK_NULL_HANDLE;
      destroy();
      return false;
    }
    loader.get_device_queue(device, queue_family, 0, &queue);

    pyrowave_device_create_queue_info lent_queue = {};
    lent_queue.queue = queue;
    lent_queue.familyIndex = queue_family;
    lent_queue.index = 0;

    pyrowave_device_create_info codec_info = {};
    codec_info.GetInstanceProcAddr = loader.get_instance_proc_addr;
    codec_info.instance = instance;
    codec_info.physical_device = physical_device;
    codec_info.device = device;
    codec_info.instance_create_info = &instance_info;
    codec_info.device_create_info = &device_info;
    codec_info.queue_info = &lent_queue;
    codec_info.queue_info_count = 1;

    // No locking callbacks. The codec's own header offers a third way out of queue synchronisation,
    // which is that it only submits inside its own entry points, and Polaris calls those from one
    // thread per session with its own queue.
    const auto codec_result = pyrowave_create_device(&codec_info, &codec);
    if (codec_result != PYROWAVE_SUCCESS || !codec) {
      BOOST_LOG(warning) << "PyroWave: the codec refused a borrowed device (result "sv
                         << static_cast<int>(codec_result) << ')';
      codec = nullptr;
      destroy();
      return false;
    }

    BOOST_LOG(info) << "PyroWave: encoding on "sv << gpu_name << ", queue family "sv << queue_family;
    return true;
  }

}  // namespace pyrowave_encode
