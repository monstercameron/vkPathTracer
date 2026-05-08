#include "render/FrameHandoff.h"

#include <utility>

namespace vkpt::render {

const char* FrameDropReasonName(FrameDropReason reason) noexcept {
  switch (reason) {
    case FrameDropReason::None:
      return "none";
    case FrameDropReason::RingFull:
      return "framering_full";
    case FrameDropReason::Cancelled:
      return "cancelled";
    case FrameDropReason::ResolveFailed:
      return "resolve_failed";
    case FrameDropReason::AccumulationReset:
      return "accumulation_reset";
  }
  return "none";
}

void FrameHandoff::set_observer(Observer observer) {
  std::scoped_lock lock(m_mutex);
  m_observer = std::move(observer);
}

void FrameHandoff::publish(DisplayFrame frame) {
  std::optional<FrameHandoffEvent> droppedEvent;
  FrameHandoffEvent publishedEvent;
  Observer observer;

  {
    std::scoped_lock lock(m_mutex);
    // Mailbox semantics: keep only the newest producer frame and account for the
    // overwritten one as dropped.
    if (m_pending.has_value()) {
      ++m_stats.dropped;
      m_stats.latest_dropped_id = m_pending->frame_id;
      m_stats.latest_dropped_generation = m_pending->generation;
      m_stats.latest_drop_reason = FrameDropReason::RingFull;
      droppedEvent = FrameHandoffEvent{
          FrameHandoffEvent::Type::Dropped,
          FrameDropReason::RingFull,
          m_pending->frame_id,
          m_pending->generation,
          m_pending->sample_count,
          m_pending->width,
          m_pending->height};
    }

    frame.frame_id = m_nextFrameId++;
    m_stats.latest_published_id = frame.frame_id;
    m_stats.latest_generation = frame.generation;
    m_stats.latest_sample_count = frame.sample_count;
    m_stats.latest_width = frame.width;
    m_stats.latest_height = frame.height;
    ++m_stats.published;
    publishedEvent = FrameHandoffEvent{
        FrameHandoffEvent::Type::Published,
        FrameDropReason::None,
        frame.frame_id,
        frame.generation,
        frame.sample_count,
        frame.width,
        frame.height};
    m_pending = std::move(frame);
    observer = m_observer;
  }

  if (observer) {
    if (droppedEvent.has_value()) {
      observer(*droppedEvent);
    }
    observer(publishedEvent);
  }
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

void FrameHandoff::clear(FrameDropReason reason) {
  std::optional<FrameHandoffEvent> droppedEvent;
  Observer observer;

  {
    std::scoped_lock lock(m_mutex);
    if (m_pending.has_value()) {
      ++m_stats.dropped;
      m_stats.latest_dropped_id = m_pending->frame_id;
      m_stats.latest_dropped_generation = m_pending->generation;
      m_stats.latest_drop_reason = reason;
      droppedEvent = FrameHandoffEvent{
          FrameHandoffEvent::Type::Dropped,
          reason,
          m_pending->frame_id,
          m_pending->generation,
          m_pending->sample_count,
          m_pending->width,
          m_pending->height};
    }
    m_pending.reset();
    observer = m_observer;
  }

  if (observer && droppedEvent.has_value()) {
    observer(*droppedEvent);
  }
}

}  // namespace vkpt::render
