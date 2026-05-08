#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "core/contracts/Determinism.h"
#include "core/contracts/IFlowSource.h"
#include "core/Types.h"
#include "scene/SceneSnapshot.h"

namespace vkpt::scene {

struct SnapshotRingStats {
  std::uint64_t publish_total = 0u;
  std::uint64_t dropped_total = 0u;
  std::uint64_t latest_generation = 0u;
  std::uint64_t latest_dropped_generation = 0u;
  std::uint64_t latest_bytes_new = 0u;
  std::uint32_t latest_cow_reused_arrays = 0u;
  std::uint32_t latest_cow_total_arrays = 0u;
  double latest_build_us = 0.0;
  std::uint32_t ring_capacity = 3u;
  bool deterministic = false;
  std::uint64_t determinism_base_seed = 0u;
  vkpt::core::FrameIndex determinism_frame_index = 0u;
  std::string determinism_scenario_id;
  std::uint64_t current_flow_id = 0u;
};

struct SnapshotReaderStats {
  std::string name;
  std::uint64_t last_observed_generation = 0u;
  std::uint64_t lag = 0u;
  bool lag_warning_emitted = false;
};

class SnapshotRing final : public vkpt::core::contracts::IFlowSource {
 public:
  using SnapshotPtr = RenderSceneSnapshot::Ptr;
  static constexpr std::uint32_t kCapacity = 3u;
  static constexpr std::uint32_t kMaxReaders = 8u;
  static constexpr std::uint32_t kInvalidReader = UINT32_MAX;

  SnapshotRing();

  // SnapshotRing lifecycle contract:
  //
  // state\method       register_reader validate_sim_tick apply_sim_tick publish current stats set_determinism
  // Empty              ok              status-only       ok*            ->Ready null    ok    ok
  // Ready              ok              status-only       ok*            ok     latest  ok    ok
  // Degraded(lag/drop) ok              status-only       ok*            ok     latest  ok    ok
  //
  // `validate_sim_tick` never mutates scene data. `apply_sim_tick` may mutate
  // the request scene only after validation has succeeded at the caller. Publish
  // order is the single flow source; DeterminismContext is retained in stats so
  // readers can correlate snapshot generations with deterministic replay input.
  std::uint32_t register_reader(std::string_view name);
  void publish(SnapshotPtr snapshot);
  vkpt::core::Status validate_sim_tick(const SimSnapshotTickRequest& request) const;
  SnapshotPtr apply_sim_tick(SimSnapshotTickRequest request,
                             const RenderSceneSnapshot* previous,
                             RenderSceneSnapshotBuildStats* stats = nullptr) const;
  SimSnapshotTickResult publish_sim_tick(SimSnapshotTickRequest request);
  SnapshotPtr current() const;
  SnapshotPtr current(std::uint32_t reader_id);
  std::uint64_t current_flow_id() const noexcept override;
  void set_determinism(const vkpt::core::DeterminismContext& context);
  vkpt::core::DeterminismContext determinism_context() const;
  SnapshotRingStats stats() const;
  SnapshotReaderStats reader_stats(std::uint32_t reader_id) const;

 private:
  struct ReaderState {
    explicit ReaderState(std::string_view reader_name) : name(reader_name) {}
    std::string name;
    std::atomic<std::uint64_t> last_observed_generation{0u};
    std::atomic_bool lag_warning_emitted{false};
  };

  ReaderState* reader(std::uint32_t reader_id) const;
  bool was_observed_by_any_reader(std::uint64_t generation) const;
  void emit_reader_lag_warnings(std::uint64_t latest_generation);

  std::atomic<SnapshotPtr> m_current;
  std::array<SnapshotPtr, kCapacity> m_slots{};
  std::uint32_t m_nextSlot = 0u;

  mutable std::mutex m_mutex;
  SnapshotRingStats m_stats{};
  vkpt::core::DeterminismContext m_determinismContext{};
  std::array<std::shared_ptr<ReaderState>, kMaxReaders> m_readers{};
  std::uint32_t m_readerCount = 0u;
};

}  // namespace vkpt::scene
