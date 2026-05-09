#include "scene/SnapshotRing.h"

#include <algorithm>
#include <utility>

#include "core/Logging.h"
#include "core/log/Log.h"
#include "core/metrics/Metrics.h"

namespace vkpt::scene {

SnapshotRing::SnapshotRing() {
  m_stats.ring_capacity = kCapacity;
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        "snapshot",
        "snapshot.config",
        {{"ring_capacity", std::to_string(kCapacity)},
         {"cow_enabled", "true"}});
  } catch (...) {
  }
}

std::uint32_t SnapshotRing::register_reader(std::string_view name) {
  // One-shot subsystem registration at startup, not on any per-frame path.
  std::scoped_lock lock(m_mutex);
  if (m_readerCount >= kMaxReaders) {
    return kInvalidReader;
  }
  const std::uint32_t id = m_readerCount++;
  m_readers[id] = std::make_shared<ReaderState>(name);
  if (const auto snapshot = m_current.load(std::memory_order_acquire)) {
    m_readers[id]->last_observed_generation.store(snapshot->generation,
                                                  std::memory_order_release);
  }
  return id;
}

void SnapshotRing::publish(SnapshotPtr snapshot) {
  if (!snapshot) {
    return;
  }

  const auto previous = m_current.load(std::memory_order_acquire);
  const bool droppedPrevious =
      previous != nullptr && !was_observed_by_any_reader(previous->generation);
  SnapshotRingStats publishedStats;

  {
    // Publish runs once per sim tick (sim->render handoff), not on the renderer's
    // per-tile/per-sample hot path. Consumers read snapshots through the lock-free
    // m_current atomic<shared_ptr> below (the documented MSVC fast path per
    // SYSTEM.md "Risks & open questions"); this lock only guards the auxiliary
    // stats/slots bookkeeping that no hot-path consumer reads.
    std::scoped_lock lock(m_mutex);
    if (droppedPrevious) {
      ++m_stats.dropped_total;
      m_stats.latest_dropped_generation = previous->generation;
    }

    m_slots[m_nextSlot] = snapshot;
    m_nextSlot = (m_nextSlot + 1u) % kCapacity;

    ++m_stats.publish_total;
    m_stats.latest_generation = snapshot->generation;
    m_stats.current_flow_id = snapshot->generation;
    m_stats.latest_bytes_new = static_cast<std::uint64_t>(snapshot->build_stats.bytes_new);
    m_stats.latest_cow_reused_arrays = snapshot->build_stats.cow_reused_arrays;
    m_stats.latest_cow_total_arrays = snapshot->build_stats.cow_total_arrays;
    m_stats.latest_build_us = snapshot->build_stats.build_us;
    publishedStats = m_stats;
  }

  m_current.store(std::move(snapshot), std::memory_order_release);
  emit_reader_lag_warnings(publishedStats.latest_generation);

  VKP_METRIC_INC("vkp.snapshot.publish_total");
  VKP_METRIC_SET("vkp.snapshot.latest_generation", publishedStats.latest_generation);
  VKP_METRIC_SET("vkp.snapshot.latest_bytes_new", publishedStats.latest_bytes_new);
  VKP_METRIC_OBSERVE("vkp.snapshot.build_us", publishedStats.latest_build_us);
  VKP_METRIC_INC("vkp.scene.snapshot_published_total");
  VKP_METRIC_SET("vkp.scene.snapshot_latest_generation", publishedStats.latest_generation);
  VKP_METRIC_SET("vkp.scene.snapshot_latest_bytes_new", publishedStats.latest_bytes_new);
  VKP_METRIC_OBSERVE("vkp.scene.snapshot_build_us",
                     static_cast<std::uint64_t>(publishedStats.latest_build_us));
  const double cowReuseRatio = publishedStats.latest_cow_total_arrays == 0u
      ? 0.0
      : static_cast<double>(publishedStats.latest_cow_reused_arrays) /
            static_cast<double>(publishedStats.latest_cow_total_arrays);
  VKP_METRIC_SET("vkp.scene.snapshot_cow_reuse_ratio", cowReuseRatio);
  if (droppedPrevious) {
    VKP_METRIC_INC("vkp.snapshot.dropped_total");
    VKP_METRIC_SET("vkp.snapshot.latest_dropped_generation",
                   publishedStats.latest_dropped_generation);
    VKP_METRIC_INC("vkp.scene.snapshot_dropped_total");
    VKP_METRIC_SET("vkp.scene.snapshot_latest_dropped_generation",
                   publishedStats.latest_dropped_generation);
  }

  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Debug,
        "snapshot",
        "snapshot.published",
        {{"gen", std::to_string(publishedStats.latest_generation)},
         {"cow_reused_arrays", std::to_string(publishedStats.latest_cow_reused_arrays)},
         {"cow_total_arrays", std::to_string(publishedStats.latest_cow_total_arrays)},
         {"cow_reuse_ratio", std::to_string(cowReuseRatio)},
         {"bytes_new", std::to_string(publishedStats.latest_bytes_new)},
         {"build_us", std::to_string(publishedStats.latest_build_us)}}); 
    if (droppedPrevious) {
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Warning,
          "snapshot",
          "snapshot.dropped",
          {{"gen", std::to_string(previous->generation)},
           {"reason", "no_reader_caught_up"}});
    }
  } catch (...) {
  }
}

SnapshotRing::SnapshotPtr SnapshotRing::current() const {
  return m_current.load(std::memory_order_acquire);
}

SnapshotRing::SnapshotPtr SnapshotRing::current(std::uint32_t reader_id) {
  auto snapshot = current();
  if (!snapshot) {
    return snapshot;
  }
  ReaderState* state = reader(reader_id);
  if (state == nullptr) {
    return snapshot;
  }

  state->last_observed_generation.store(snapshot->generation,
                                        std::memory_order_release);
  state->lag_warning_emitted.store(false, std::memory_order_release);
  return snapshot;
}

std::uint64_t SnapshotRing::current_flow_id() const noexcept {
  const auto snapshot = m_current.load(std::memory_order_acquire);
  return snapshot ? snapshot->generation : 0u;
}

void SnapshotRing::set_determinism(const vkpt::core::DeterminismContext& context) {
  // Determinism context update; happens at scenario boundaries, not per frame.
  vkpt::core::DeterminismContext previous;
  {
    std::scoped_lock lock(m_mutex);
    previous = m_determinismContext;
    m_determinismContext = context;
    m_stats.deterministic = context.enabled;
    m_stats.determinism_base_seed = context.base_seed;
    m_stats.determinism_frame_index = context.frame_index;
    m_stats.determinism_scenario_id = context.scenario_id;
  }
  vkpt::core::EmitDeterminismChangedIfNeeded("snapshot", previous, context);
}

vkpt::core::DeterminismContext SnapshotRing::determinism_context() const {
  // Diagnostic / determinism-replay accessor; not on per-frame hot path.
  std::scoped_lock lock(m_mutex);
  return m_determinismContext;
}

SnapshotRingStats SnapshotRing::stats() const {
  // Diagnostic accessor (UI/metrics scrape); not on per-frame hot path.
  std::scoped_lock lock(m_mutex);
  auto out = m_stats;
  out.current_flow_id = current_flow_id();
  return out;
}

SnapshotReaderStats SnapshotRing::reader_stats(std::uint32_t reader_id) const {
  SnapshotReaderStats out;
  const ReaderState* state = reader(reader_id);
  if (state == nullptr) {
    return out;
  }
  out.name = state->name;
  out.last_observed_generation =
      state->last_observed_generation.load(std::memory_order_acquire);
  const auto latest = m_current.load(std::memory_order_acquire);
  const std::uint64_t latestGeneration = latest ? latest->generation : 0u;
  out.lag = latestGeneration > out.last_observed_generation
      ? latestGeneration - out.last_observed_generation
      : 0u;
  out.lag_warning_emitted =
      state->lag_warning_emitted.load(std::memory_order_acquire);
  return out;
}

SnapshotRing::ReaderState* SnapshotRing::reader(std::uint32_t reader_id) const {
  if (reader_id >= kMaxReaders) {
    return nullptr;
  }
  const auto& state = m_readers[reader_id];
  return state.get();
}

bool SnapshotRing::was_observed_by_any_reader(std::uint64_t generation) const {
  for (const auto& state : m_readers) {
    if (!state) {
      continue;
    }
    if (state->last_observed_generation.load(std::memory_order_acquire) >= generation) {
      return true;
    }
  }
  return false;
}

void SnapshotRing::emit_reader_lag_warnings(std::uint64_t latest_generation) {
  std::array<std::shared_ptr<ReaderState>, kMaxReaders> readers;
  {
    // Snapshot the reader array under the lock so we don't hold it across the
    // logging loop. Same publish-cadence path (once per sim tick), not hot.
    std::scoped_lock lock(m_mutex);
    readers = m_readers;
  }

  for (const auto& state : readers) {
    if (!state) {
      continue;
    }

    const auto observed =
        state->last_observed_generation.load(std::memory_order_acquire);
    const auto lag = latest_generation > observed ? latest_generation - observed : 0u;
    if (lag < kCapacity) {
      continue;
    }
    const bool already_emitted =
        state->lag_warning_emitted.exchange(true, std::memory_order_acq_rel);
    if (already_emitted) {
      continue;
    }

    VKP_LOG(Warn,
            "scene",
            "lag_warning_emitted",
            "reader",
            state->name,
            "latest_generation",
            latest_generation,
            "last_observed_generation",
            observed,
            "lag",
            lag);
  }
}

}  // namespace vkpt::scene
