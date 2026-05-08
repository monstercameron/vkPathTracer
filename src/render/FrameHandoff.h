#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "pathtracer/PathTracer.h"

namespace vkpt::render {

enum class FrameDropReason : std::uint8_t {
  None = 0,
  RingFull,
  Cancelled,
  ResolveFailed,
  AccumulationReset,
  FrameRingFull = RingFull,
};

const char* FrameDropReasonName(FrameDropReason reason) noexcept;

/// CPU-displayable frame published by the render worker and consumed by the UI.
struct DisplayFrame {
  std::vector<std::uint8_t> rgba8;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::uint64_t frame_id = 0u;
  std::uint64_t generation = 0u;
  std::uint32_t sample_count = 0u;
  vkpt::pathtracer::SampleCounters counters{};
};

/// Counters describing latest-frame mailbox behavior.
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
  std::uint64_t latest_dropped_id = 0u;
  std::uint64_t latest_dropped_generation = 0u;
  FrameDropReason latest_drop_reason = FrameDropReason::None;
};

struct FrameHandoffEvent {
  enum class Type : std::uint8_t {
    Published,
    Dropped,
  };

  Type type = Type::Published;
  FrameDropReason drop_reason = FrameDropReason::None;
  std::uint64_t frame_id = 0u;
  std::uint64_t generation = 0u;
  std::uint32_t sample_count = 0u;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
};

/// Single-slot latest-frame handoff between producer and consumer threads.
///
/// Publishing replaces any unacquired frame and counts it as dropped. Consumers
/// acquire by move, so the producer never blocks on display-side ownership.
///
/// FrameHandoff state machine contract:
///
/// state\method  set_observer  publish     acquire_latest  stats  clear
/// Empty         ok            ->Pending   noop            ok     noop
/// Pending       ok            ->Pending   ->Empty         ok     ->Empty
///
/// "Pending" means one display frame is waiting for the consumer. publish() is
/// always latest-wins and reports the overwritten pending frame as RingFull.
class FrameHandoff {
 public:
  using Observer = std::function<void(const FrameHandoffEvent&)>;

  /// Attach an optional observer for publish/drop events.
  void set_observer(Observer observer);
  /// Publish a new latest frame, assigning a monotonically increasing frame id.
  void publish(DisplayFrame frame);
  /// Move out the latest pending frame, if one exists.
  std::optional<DisplayFrame> acquire_latest();
  /// Return a stable snapshot of mailbox counters.
  FrameHandoffStats stats() const;
  /// Drop any pending frame while preserving historical counters.
  void clear(FrameDropReason reason = FrameDropReason::Cancelled);

 private:
  mutable std::mutex m_mutex;
  std::optional<DisplayFrame> m_pending;
  FrameHandoffStats m_stats{};
  std::uint64_t m_nextFrameId = 1u;
  Observer m_observer;
};

}  // namespace vkpt::render
