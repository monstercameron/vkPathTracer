#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "pathtracer/PathTracer.h"

namespace vkpt::render {

struct DisplayFrame {
  std::vector<std::uint8_t> rgba8;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::uint64_t frame_id = 0u;
  std::uint64_t generation = 0u;
  std::uint32_t sample_count = 0u;
  vkpt::pathtracer::SampleCounters counters{};
};

struct FrameHandoffStats {
  std::uint64_t published = 0u;
  std::uint64_t acquired = 0u;
  std::uint64_t dropped = 0u;
  std::uint64_t latest_published_id = 0u;
  std::uint64_t latest_acquired_id = 0u;
  std::uint64_t latest_generation = 0u;
  std::uint32_t latest_sample_count = 0u;
  std::size_t latest_width = 0u;
  std::size_t latest_height = 0u;
};

class FrameHandoff {
 public:
  void publish(DisplayFrame frame);
  std::optional<DisplayFrame> acquire_latest();
  FrameHandoffStats stats() const;
  void clear();

 private:
  mutable std::mutex m_mutex;
  std::optional<DisplayFrame> m_pending;
  FrameHandoffStats m_stats{};
  std::uint64_t m_nextFrameId = 1u;
};

}  // namespace vkpt::render
