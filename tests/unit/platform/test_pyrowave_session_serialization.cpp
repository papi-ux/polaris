/**
 * @file tests/unit/platform/test_pyrowave_session_serialization.cpp
 * @brief Keep codec lifecycle operations out of another session's borrowed command buffer.
 */
#include "../../tests_common.h"
#include "src/platform/linux/pyrowave_encode.h"
#include <vulkan/vulkan.h>
#include "pyrowave.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <vector>

namespace {
  enum class operation_t { cpu_encode, create, destroy };

  struct probe_t {
    std::mutex mutex;
    std::condition_variable changed;
    bool armed = false;
    bool gpu_entered = false;
    bool release_gpu = false;
    bool gpu_finished = false;
    bool overlapping_entry = false;
    operation_t operation = operation_t::cpu_encode;
  } probe;

  // The wrappers pause at the real codec boundary, after Polaris has installed its borrowed
  // command buffer. Unsafe peer calls are intercepted before they can corrupt GPU work.
  bool observe_peer_entry(operation_t operation, bool wait_for_release) {
    std::unique_lock lock {probe.mutex};
    if (!probe.armed || probe.release_gpu || probe.operation != operation) {
      return false;
    }
    probe.overlapping_entry = true;
    probe.changed.notify_all();
    if (wait_for_release) {
      probe.changed.wait(lock, [] { return probe.gpu_finished; });
    }
    return true;
  }
}

extern "C" {
  pyrowave_result __real_pyrowave_encoder_encode_gpu_scaled_synchronous(
    pyrowave_encoder, const pyrowave_gpu_sync_operation *, const pyrowave_gpu_sync_operation *,
    const pyrowave_scaled_encode_info *, const pyrowave_rate_control *);
  pyrowave_result __wrap_pyrowave_encoder_encode_gpu_scaled_synchronous(
    pyrowave_encoder encoder, const pyrowave_gpu_sync_operation *acquire,
    const pyrowave_gpu_sync_operation *release, const pyrowave_scaled_encode_info *info,
    const pyrowave_rate_control *rate) {
    {
      std::unique_lock lock {probe.mutex};
      if (probe.armed) {
        probe.gpu_entered = true;
        probe.changed.notify_all();
        probe.changed.wait(lock, [] { return probe.release_gpu; });
      }
    }
    return __real_pyrowave_encoder_encode_gpu_scaled_synchronous(encoder, acquire, release, info, rate);
  }

  pyrowave_result __real_pyrowave_encoder_encode_cpu_synchronous(
    pyrowave_encoder, const pyrowave_cpu_buffer *, const pyrowave_rate_control *);
  pyrowave_result __wrap_pyrowave_encoder_encode_cpu_synchronous(
    pyrowave_encoder encoder, const pyrowave_cpu_buffer *buffer, const pyrowave_rate_control *rate) {
    if (observe_peer_entry(operation_t::cpu_encode, false)) {
      return PYROWAVE_ERROR_GENERIC;
    }
    return __real_pyrowave_encoder_encode_cpu_synchronous(encoder, buffer, rate);
  }

  pyrowave_result __real_pyrowave_encoder_create(
    const pyrowave_encoder_create_info *, pyrowave_encoder *);
  pyrowave_result __wrap_pyrowave_encoder_create(
    const pyrowave_encoder_create_info *info, pyrowave_encoder *encoder) {
    observe_peer_entry(operation_t::create, true);
    return __real_pyrowave_encoder_create(info, encoder);
  }

  void __real_pyrowave_encoder_destroy(pyrowave_encoder);
  void __wrap_pyrowave_encoder_destroy(pyrowave_encoder encoder) {
    observe_peer_entry(operation_t::destroy, true);
    __real_pyrowave_encoder_destroy(encoder);
  }
}

namespace {
  void check_peer_operation(operation_t operation) {
    using namespace std::chrono_literals;
    if (!pyrowave_encode::available()) {
      GTEST_SKIP() << "no Vulkan device this codec can use";
    }
    constexpr int width = 256, height = 144;
    constexpr std::size_t budget = 128 * 1024;
    const auto chroma = pyrowave_encode::chroma_e::yuv444;
    auto gpu = pyrowave_encode::make_session(width, height, chroma);
    auto peer = pyrowave_encode::make_session(width, height, chroma);
    ASSERT_NE(gpu, nullptr);
    ASSERT_NE(peer, nullptr);
    std::vector<uint8_t> bgra(width * height * 4, 127);
    std::vector<uint8_t> plane(width * height, 127);

    {
      std::lock_guard lock {probe.mutex};
      probe.armed = true;
      probe.gpu_entered = false;
      probe.release_gpu = false;
      probe.gpu_finished = false;
      probe.overlapping_entry = false;
      probe.operation = operation;
    }
    auto gpu_result = std::async(std::launch::async, [&] {
      const bool result = gpu->encode_packed(bgra.data(), width, height, width * 4, budget);
      {
        std::lock_guard lock {probe.mutex};
        probe.gpu_finished = true;
      }
      probe.changed.notify_all();
      return result;
    });
    bool entered;
    {
      std::unique_lock lock {probe.mutex};
      entered = probe.changed.wait_for(lock, 5s, [] { return probe.gpu_entered; });
    }

    std::promise<void> peer_started;
    auto started = peer_started.get_future();
    auto peer_result = std::async(std::launch::async, [&] {
      peer_started.set_value();
      switch (operation) {
        case operation_t::cpu_encode:
          return peer->encode(plane.data(), plane.data(), plane.data(), budget);
        case operation_t::create:
          return pyrowave_encode::make_session(width, height, chroma) != nullptr;
        case operation_t::destroy:
          peer.reset();
          return true;
      }
      return false;
    });
    started.wait();
    bool overlapped;
    {
      std::unique_lock lock {probe.mutex};
      probe.changed.wait_for(lock, 100ms, [] { return probe.overlapping_entry; });
      overlapped = probe.overlapping_entry;
      probe.release_gpu = true;
    }
    probe.changed.notify_all();
    // Join both workers before asserting, including on a failed admission check.
    const bool peer_ok = peer_result.get();
    const bool gpu_ok = gpu_result.get();
    {
      std::lock_guard lock {probe.mutex};
      probe.armed = false;
    }
    EXPECT_TRUE(entered) << "the GPU-input path must reach the borrowed-command-buffer boundary";
    EXPECT_FALSE(overlapped) << "a peer entered the codec while another session owned its command buffer";
    EXPECT_TRUE(gpu_ok);
    EXPECT_TRUE(peer_ok);
  }
}

TEST(PyroWaveSessionSerializationTests, CpuEncodingWaitsForBorrowedGpuCommands) {
  check_peer_operation(operation_t::cpu_encode);
}

TEST(PyroWaveSessionSerializationTests, EncoderCreationWaitsForBorrowedGpuCommands) {
  check_peer_operation(operation_t::create);
}

TEST(PyroWaveSessionSerializationTests, EncoderDestructionWaitsForBorrowedGpuCommands) {
  check_peer_operation(operation_t::destroy);
}
