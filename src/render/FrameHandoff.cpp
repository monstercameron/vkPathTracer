#include "render/FrameHandoff.h"

#include <utility>

namespace vkpt::render {

void FrameHandoff::publish(DisplayFrame frame) {
  std::scoped_lock lock(m_mutex);
  // Mailbox semantics: keep only the newest producer frame and account for the
  // overwritten one as dropped.
  if (m_pending.has_value()) {
    ++m_stats.dropped;
  }

  frame.frame_id = m_nextFrameId++;
  m_stats.latest_published_id = frame.frame_id;
  m_stats.latest_generation = frame.generation;
  m_stats.latest_sample_count = frame.sample_count;
  m_stats.latest_width = frame.width;
  m_stats.latest_height = frame.height;
  ++m_stats.published;
  m_pending = std::move(frame);
}

std::optional<DisplayFrame> FrameHandoff::acquire_latest() {
  std::scoped_lock lock(m_mutex);
  if (!m_pending.has_value()) {
    return std::nullopt;
  }

  // Move ownership to the caller so large pixel buffers are not copied.
  auto frame = std::move(m_pending);
  m_pending.reset();
  ++m_stats.acquired;
  m_stats.latest_acquired_id = frame->frame_id;
  return frame;
}

FrameHandoffStats FrameHandoff::stats() const {
  std::scoped_lock lock(m_mutex);
  return m_stats;
}

void FrameHandoff::clear() {
  std::scoped_lock lock(m_mutex);
  if (m_pending.has_value()) {
    ++m_stats.dropped;
  }
  m_pending.reset();
}

}  // namespace vkpt::render
