/**
 * @file src/nvenc/nvenc_cuda.cpp
 * @brief Register the existing Linux CUDA converter surface with native NVENC.
 */
#if defined(__linux__) && !defined(__ANDROID__)
#include "nvenc_cuda.h"

#include <dlfcn.h>

namespace nvenc {

  nvenc_cuda::nvenc_cuda(void *cuda_context, void *input, std::uint32_t pitch):
      nvenc_base(NV_ENC_DEVICE_TYPE_CUDA),
      input(input),
      pitch(pitch) {
    device = cuda_context;
  }

  nvenc_cuda::~nvenc_cuda() {
    // Unregister before the caller releases the converter's frame/context.
    destroy_encoder();
    nvenc.reset();
    if (library) {
      dlclose(library);
    }
  }

  bool nvenc_cuda::init_library() {
    if (!device || !input || !pitch) {
      return false;
    }
    if (!library) {
      library = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
    }
    if (!library) {
      BOOST_LOG(debug) << "NvEnc: native CUDA encoder library unavailable";
      return false;
    }
    auto create_instance = reinterpret_cast<decltype(&NvEncodeAPICreateInstance)>(
      dlsym(library, "NvEncodeAPICreateInstance"));
    if (!create_instance) {
      return false;
    }
    auto functions = std::make_shared<NV_ENCODE_API_FUNCTION_LIST>();
    functions->version = min_struct_version(NV_ENCODE_API_FUNCTION_LIST_VER);
    if (nvenc_failed(create_instance(functions.get()))) {
      BOOST_LOG(debug) << "NvEnc: native CUDA API unavailable: " << last_nvenc_error_string;
      return false;
    }
    nvenc = std::move(functions);
    return true;
  }

  bool nvenc_cuda::create_and_register_input_buffer() {
    if (!device || !input || !pitch || encoder_params.buffer_format != NV_ENC_BUFFER_FORMAT_NV12 ||
        pitch < encoder_params.width) {
      return false;
    }
    if (registered_input_buffer) {
      return true;
    }
    NV_ENC_REGISTER_RESOURCE resource = {min_struct_version(NV_ENC_REGISTER_RESOURCE_VER, 3, 4)};
    resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    resource.width = encoder_params.width;
    resource.height = encoder_params.height;
    resource.pitch = pitch;
    resource.resourceToRegister = input;
    resource.bufferFormat = encoder_params.buffer_format;
    resource.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (nvenc_failed(nvenc->nvEncRegisterResource(encoder, &resource))) {
      BOOST_LOG(debug) << "NvEnc: native CUDA surface registration failed: " << last_nvenc_error_string;
      return false;
    }
    registered_input_buffer = resource.registeredResource;
    return registered_input_buffer != nullptr;
  }

}  // namespace nvenc
#endif
