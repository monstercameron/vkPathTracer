#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/contracts/Result.h"
#include "pathtracer/PathTracer.h"

namespace vkpt::scene {

class SnapshotRing;

template <typename T>
class CowArray {
 public:
  CowArray() = default;
  CowArray(std::shared_ptr<const T[]> data, std::size_t size)
      : m_data(std::move(data)), m_size(size) {}

  const T* data() const { return m_data.get(); }
  std::size_t size() const { return m_size; }
  bool empty() const { return m_size == 0u; }
  std::span<const T> view() const { return {data(), size()}; }
  const T& operator[](std::size_t index) const { return data()[index]; }

  bool shares_storage_with(const CowArray& other) const {
    return data() == other.data() && size() == other.size();
  }

 private:
  std::shared_ptr<const T[]> m_data;
  std::size_t m_size = 0u;
};

struct RenderSceneSnapshotBuildStats {
  std::uint32_t cow_reused_arrays = 0u;
  std::uint32_t cow_total_arrays = 0u;
  std::size_t bytes_new = 0u;
  double build_us = 0.0;
  double acceleration_build_us = 0.0;
  bool acceleration_built = false;
  bool acceleration_reused = false;
  bool acceleration_refit_descriptor = false;
};

struct RenderSceneSnapshotRevisions {
  std::uint64_t generation = 1u;
  std::uint64_t topology_revision = 1u;
  std::uint64_t transform_revision = 1u;
  std::uint64_t camera_revision = 1u;
  std::uint64_t material_revision = 1u;
  std::uint64_t wall_time_ns = 0u;
};

struct RenderInstanceMotion {
  std::uint32_t entity_id = 0u;
  std::uint32_t instance_index = 0u;
  bool previous_valid = false;
  vkpt::pathtracer::RTInstance previous{};
  vkpt::pathtracer::RTInstance current{};
};

struct SnapshotAccelerationHandle {
  std::shared_ptr<const vkpt::pathtracer::IRayAccelerator> cpu_bvh;
  vkpt::pathtracer::RayAcceleratorBuildInfo cpu_bvh_info{};
  std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> refit_updates;
  bool reused_from_previous = false;
  bool transform_refit_descriptor = false;

  bool valid() const {
    return cpu_bvh != nullptr && cpu_bvh_info.built;
  }
};

struct RenderSceneSnapshot {
  using Ptr = std::shared_ptr<const RenderSceneSnapshot>;

  std::uint64_t generation = 0u;
  std::uint64_t topology_revision = 0u;
  std::uint64_t transform_revision = 0u;
  std::uint64_t camera_revision = 0u;
  std::uint64_t material_revision = 0u;
  std::uint64_t wall_time_ns = 0u;

  CowArray<vkpt::pathtracer::Vec3> vertices;
  CowArray<vkpt::pathtracer::Vec2> texcoords;
  CowArray<std::uint32_t> indices;
  CowArray<vkpt::pathtracer::Vec3> local_vertices;
  CowArray<std::uint32_t> local_indices;
  CowArray<vkpt::pathtracer::RTInstance> instances;
  CowArray<vkpt::pathtracer::RTTessellationRequest> tessellation_requests;
  CowArray<vkpt::pathtracer::RTSdfPrimitive> sdf_primitives;
  CowArray<vkpt::pathtracer::RTMaterial> materials;
  CowArray<std::string> textures;
  CowArray<vkpt::pathtracer::RTHitLight> lights;
  CowArray<vkpt::pathtracer::Vec3> environment_map;
  CowArray<RenderInstanceMotion> instance_motion;

  vkpt::pathtracer::Vec3 environment_color{0.0f, 0.0f, 0.0f};
  vkpt::pathtracer::Vec3 environment_map_scale{0.0f, 0.0f, 0.0f};
  std::uint32_t environment_map_width = 0u;
  std::uint32_t environment_map_height = 0u;
  vkpt::pathtracer::RTCameraState camera{};

  RenderSceneSnapshotBuildStats build_stats{};
  SnapshotAccelerationHandle acceleration{};
  mutable std::atomic<std::shared_ptr<const vkpt::pathtracer::PathTracerSceneSnapshot>>
      path_tracer_scene;

  bool geometry_storage_reused_from(const RenderSceneSnapshot& other) const;
  const vkpt::pathtracer::PathTracerSceneSnapshot& path_tracer_scene_snapshot() const;
};

enum class RenderSceneSnapshotChange : std::uint8_t {
  None = 0u,
  Topology = 1u << 0u,
  Transform = 1u << 1u,
  Camera = 1u << 2u,
  Material = 1u << 3u,
};

enum class SnapshotTransitionAction : std::uint8_t {
  Continue = 0u,
  ResetAccumulation,
  ReprojectCamera,
  ResetMovingPixels,
  InvalidateShading,
};

struct SnapshotTransitionCapabilities {
  bool camera_reprojection = false;
  bool transform_motion_vectors = false;
  bool material_reshade = false;
};

struct SnapshotTransitionDecision {
  RenderSceneSnapshotChange changes = RenderSceneSnapshotChange::None;
  SnapshotTransitionAction action = SnapshotTransitionAction::Continue;
  bool rebuild_tile_schedule = false;
  bool reset_accumulation = false;
  const char* reason = "none";
};

struct SimSnapshotTickRequest {
  SnapshotRing* snapshots = nullptr;
  vkpt::pathtracer::PathTracerSceneSnapshot* scene = nullptr;
  RenderSceneSnapshotRevisions revisions{};
  SnapshotTransitionCapabilities transition_capabilities{};
  std::function<vkpt::core::Status(const vkpt::pathtracer::PathTracerSceneSnapshot&)> validate_writes;
  std::function<void(vkpt::pathtracer::PathTracerSceneSnapshot&)> apply_writes;
};

struct SimSnapshotTickResult {
  vkpt::core::Status status = vkpt::core::Status::ok();
  RenderSceneSnapshot::Ptr previous;
  RenderSceneSnapshot::Ptr snapshot;
  SnapshotTransitionDecision transition{};
  RenderSceneSnapshotBuildStats build_stats{};
  bool published = false;
};

constexpr RenderSceneSnapshotChange operator|(RenderSceneSnapshotChange lhs,
                                              RenderSceneSnapshotChange rhs) {
  return static_cast<RenderSceneSnapshotChange>(
      static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

constexpr RenderSceneSnapshotChange& operator|=(RenderSceneSnapshotChange& lhs,
                                                RenderSceneSnapshotChange rhs) {
  lhs = lhs | rhs;
  return lhs;
}

constexpr bool HasChange(RenderSceneSnapshotChange value,
                         RenderSceneSnapshotChange flag) {
  return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0u;
}

const char* ToString(SnapshotTransitionAction action);
std::string SnapshotChangeReason(RenderSceneSnapshotChange changes);

std::uint64_t SnapshotWallTimeNowNs();

RenderSceneSnapshot::Ptr BuildRenderSceneSnapshot(
    const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
    const RenderSceneSnapshot* previous,
    RenderSceneSnapshotRevisions revisions,
    RenderSceneSnapshotBuildStats* stats = nullptr);

RenderSceneSnapshotChange CompareRenderSceneSnapshots(
    const RenderSceneSnapshot* previous,
    const RenderSceneSnapshot& current);

SnapshotTransitionDecision DecideSnapshotTransition(
    const RenderSceneSnapshot* previous,
    const RenderSceneSnapshot& current,
    SnapshotTransitionCapabilities capabilities = {});

SimSnapshotTickResult PublishSimTickSnapshot(SimSnapshotTickRequest request);

std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> DiffInstanceTransforms(
    const RenderSceneSnapshot& previous,
    const RenderSceneSnapshot& current);

}  // namespace vkpt::scene
