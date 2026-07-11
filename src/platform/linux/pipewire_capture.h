/**
 * @file src/platform/linux/pipewire_capture.h
 * @brief PipeWire capture format, copy, and frame metadata policy.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "src/platform/common.h"

namespace pipewire_capture {
  std::optional<std::uint32_t> drm_format_for_spa(std::uint32_t spa_format);

  std::size_t safe_row_copy_bytes(
    std::size_t width_bytes,
    std::int32_t source_stride,
    std::size_t destination_stride
  );

  platf::frame_metadata_t cpu_frame_metadata();
  platf::frame_metadata_t dmabuf_frame_metadata(std::string render_node);
}  // namespace pipewire_capture
