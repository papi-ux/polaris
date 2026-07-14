/**
 * @file src/platform/linux/vaapi.h
 * @brief Declarations for VA-API hardware accelerated capture.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

// local includes
#include "misc.h"
#include "src/platform/common.h"

namespace egl {
  struct surface_descriptor_t;
}

namespace va {
  enum class dmabuf_surface_action_e {
    blank,
    reuse,
    import,
    unavailable,
  };

  struct dmabuf_surface_decision_t {
    dmabuf_surface_action_e action;
    std::optional<std::size_t> slot;
  };

  /**
   * @brief Tracks which imported DMA-BUF surface may be reused without
   *        destroying the surface currently referenced by the GL pipeline.
   *
   * Planning is side-effect free so a failed EGL import leaves the previous
   * active surface and next import slot unchanged.
   */
  class dmabuf_surface_cache_policy_t {
  public:
    static constexpr std::size_t capacity = 2;

    dmabuf_surface_decision_t plan(
      std::uint64_t frame_sequence,
      std::uint64_t buffer_key,
      const std::array<bool, capacity> &valid_slots
    ) const {
      if (frame_sequence == 0) {
        return {dmabuf_surface_action_e::blank, std::nullopt};
      }

      if (frame_sequence <= sequence_) {
        if (active_slot_ && valid_slots[*active_slot_]) {
          return {dmabuf_surface_action_e::reuse, active_slot_};
        }
        return {dmabuf_surface_action_e::unavailable, std::nullopt};
      }

      if (buffer_key != 0) {
        for (std::size_t slot = 0; slot < capacity; ++slot) {
          if (valid_slots[slot] && buffer_keys_[slot] == buffer_key) {
            return {dmabuf_surface_action_e::reuse, slot};
          }
        }
      }

      return {dmabuf_surface_action_e::import, next_import_slot_};
    }

    void commit_blank() {
      active_slot_.reset();
    }

    void commit_live(std::uint64_t frame_sequence, std::uint64_t buffer_key, std::size_t slot) {
      sequence_ = frame_sequence;
      buffer_keys_[slot] = buffer_key;
      active_slot_ = slot;
      if (next_import_slot_ == slot) {
        next_import_slot_ = (next_import_slot_ + 1) % capacity;
      }
    }

  private:
    std::uint64_t sequence_ {0};
    std::array<std::uint64_t, capacity> buffer_keys_ {};
    std::optional<std::size_t> active_slot_;
    std::size_t next_import_slot_ {0};
  };

  /**
   * Width --> Width of the image
   * Height --> Height of the image
   * offset_x --> Horizontal offset of the image in the texture
   * offset_y --> Vertical offset of the image in the texture
   * file_t card --> The file descriptor of the render device used for encoding
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, bool vram);
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, int offset_x, int offset_y, bool vram);
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, file_t &&card, int offset_x, int offset_y, bool vram);

  // Ensure the render device pointed to by fd is capable of encoding h264 with the hevc_mode configured
  bool validate(int fd);
}  // namespace va
