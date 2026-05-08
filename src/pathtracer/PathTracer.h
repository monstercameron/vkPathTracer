#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cpu/CpuFeatures.h"
#include "core/Logging.h"
#include "core/Types.h"
#include "core/contracts/Determinism.h"
#include "core/contracts/Result.h"
#include "scene/Scene.h"

namespace vkpt::pathtracer {

struct Vec2 {
  float u = 0.0f;
  float v = 0.0f;
};

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;

  Vec3& operator+=(const Vec3& other) {
    x += other.x;
    y += other.y;
    z += other.z;
    return *this;
  }
};

struct Quat4 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

struct Ray {
  Vec3 origin{};
  Vec3 direction{};
};

enum class ToneMapMode : std::uint8_t {
  Linear = 0,
  Reinhard,
  FilmicApprox,
  AcesApprox,
};

enum class OutputTransformMode : std::uint8_t {
  Gamma = 0,
  Linear,
};

struct FilmResolveSettings {
  float exposure = 1.0f;
  float white_balance_kelvin = 6500.0f;
  ToneMapMode tone_map = ToneMapMode::Linear;
  OutputTransformMode output_transform = OutputTransformMode::Gamma;
  float gamma = 2.2f;
  bool clamp_output = true;
};

struct IntegratorSettings {
  uint32_t max_depth = 6;
  bool enable_nee = false;
  bool enable_mis = false;
  uint32_t russian_roulette_start_depth = 3;
  float russian_roulette_min_survival = 0.1f;
  float russian_roulette_max_survival = 0.99f;
};

struct CameraSettings {
  float aperture_radius = 0.0f;
  float focus_distance = 0.0f;
};

struct FilmSettings {
  FilmResolveSettings resolve{};
  bool enable_denoiser = false;
  bool enable_temporal_aa = false;
};

struct PathTraceSettings {
  uint32_t width = 256;
  uint32_t height = 256;
  uint32_t spp = 8;
  uint64_t seed = 0x12345678ULL;
  bool deterministic = true;
  vkpt::core::FrameIndex determinism_frame_index = 0u;
  std::string determinism_scenario_id;
  IntegratorSettings integrator{};
  CameraSettings camera{};
  FilmSettings film{};

  void set_determinism(const vkpt::core::DeterminismContext& context) {
    const auto previous = determinism_context();
    deterministic = context.enabled;
    seed = context.base_seed;
    determinism_frame_index = context.frame_index;
    determinism_scenario_id = context.scenario_id;
    vkpt::core::EmitDeterminismChangedIfNeeded("pathtracer", previous, determinism_context());
  }

  vkpt::core::DeterminismContext determinism_context() const {
    return vkpt::core::MakeDeterminismContext(deterministic,
                                               seed,
                                               determinism_frame_index,
                                               determinism_scenario_id);
  }

  vkpt::core::DeterminismContext determinism_context(
      vkpt::core::FrameIndex frame_index,
      std::string_view scenario_id) const {
    return vkpt::core::MakeDeterminismContext(deterministic,
                                               seed,
                                               frame_index,
                                               scenario_id);
  }
};

struct RenderSettings {
  uint32_t width = 256;
  uint32_t height = 256;
  uint32_t spp = 8;
  uint32_t max_depth = 6;
  uint64_t seed = 0x12345678ULL;
  bool enable_nee = false;
  bool enable_mis = false;
  bool deterministic = true;
  vkpt::core::FrameIndex determinism_frame_index = 0u;
  std::string determinism_scenario_id;
  uint32_t russian_roulette_start_depth = 3;
  float russian_roulette_min_survival = 0.1f;
  float russian_roulette_max_survival = 0.99f;
  float camera_aperture_radius = 0.0f;
  float camera_focus_distance = 0.0f;
  bool enable_denoiser = false;
  bool enable_temporal_aa = false;
  FilmResolveSettings film_resolve{};

  void set_determinism(const vkpt::core::DeterminismContext& context) {
    const auto previous = determinism_context();
    deterministic = context.enabled;
    seed = context.base_seed;
    determinism_frame_index = context.frame_index;
    determinism_scenario_id = context.scenario_id;
    vkpt::core::EmitDeterminismChangedIfNeeded("render", previous, determinism_context());
  }

  vkpt::core::DeterminismContext determinism_context() const {
    return vkpt::core::MakeDeterminismContext(deterministic,
                                               seed,
                                               determinism_frame_index,
                                               determinism_scenario_id);
  }

  vkpt::core::DeterminismContext determinism_context(
      vkpt::core::FrameIndex frame_index,
      std::string_view scenario_id) const {
    return vkpt::core::MakeDeterminismContext(deterministic,
                                               seed,
                                               frame_index,
                                               scenario_id);
  }
};

PathTraceSettings MakePathTraceSettings(const RenderSettings& settings);
RenderSettings MakeRenderSettings(const PathTraceSettings& settings);
std::string SerializePathTraceSettings(const PathTraceSettings& settings);

struct NeeResult {
  Vec3 radiance{};
  bool valid = false;
};

struct RenderStats {
  Vec2 resolution{};
  uint32_t spp = 0;
  uint32_t max_depth = 0;
  uint64_t seed = 0;
};

/// Stable identity for one random-number stream.
///
/// The scalar and GPU tracers derive sampler state from this tuple instead of
/// mutable global RNG state. Keeping pixel, sample, frame, path, and dimension
/// explicit makes tiled/multithreaded rendering reproducible: any worker can
/// regenerate the same random sequence for the same path without depending on
/// scheduling order.
struct SampleKey {
  vkpt::core::FrameIndex frame_index = 0;
  vkpt::core::StableId pixel_index = 0;
  uint32_t sample_index = 0;
  uint32_t dimension = 0;
  uint32_t path_depth = 0;
  uint64_t path_id = 0;
  uint64_t seed = 0;
};

/// Per-render diagnostic counters accumulated by the active integrator.
///
/// These are intentionally low-level. Benchmarks and trace probes use them to
/// separate actual shading work from acceleration-structure quality issues
/// such as excessive node or leaf visits.
struct SampleCounters {
  uint64_t samples = 0;
  uint64_t rays = 0;
  uint64_t triangle_tests = 0;
  uint64_t sdf_tests = 0;
  uint64_t sdf_steps = 0;
  uint64_t triangle_hits = 0;
  uint64_t sdf_hits = 0;
  uint64_t sdf_misses = 0;
  uint64_t bvh_node_visits = 0;
  uint64_t bvh_leaf_visits = 0;
  uint64_t shadow_tests = 0;
};

enum class SdfShape : std::uint8_t {
  Sphere,
  Box,
  RoundedBox,
  Plane,
  Torus,
  Capsule,
  Unknown
};

struct RTMaterial {
  Vec3 albedo{1.0f, 1.0f, 1.0f};
  Vec3 emissive{0.0f, 0.0f, 0.0f};
  float roughness = 1.0f;
  float metallic = 0.0f;
  float ior = 1.5f;
  float transmission = 0.0f;
  float clearcoat = 0.0f;
  float sheen = 0.0f;
  float anisotropy = 0.0f;
  float alpha = 1.0f;
  uint32_t material_model = 0;
  uint32_t material_effect = 0;
  uint32_t material_flags = 0;
  uint32_t base_color_texture_index = 0xFFFFFFFFu;
  uint32_t normal_texture_index = 0xFFFFFFFFu;
  bool is_emissive() const {
    return emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f;
  }
};

inline constexpr uint32_t kInvalidRTInstanceIndex = 0xFFFFFFFFu;

enum RTInstanceFlags : uint32_t {
  kRTInstanceFlagNone = 0u,
  kRTInstanceFlagDynamicTransform = 1u << 0u,
  kRTInstanceFlagPhysicsControlled = 1u << 1u,
  kRTInstanceFlagTransformDirty = 1u << 2u,
};

struct RTInstance {
  vkpt::core::StableId entity_id = 0;
  uint32_t geometry_id = 0;
  uint32_t first_triangle = 0;
  uint32_t triangle_count = 0;
  uint32_t material_index = 0;
  uint32_t flags = kRTInstanceFlagNone;
  uint32_t transform_revision = 0;
  uint32_t local_first_vertex = 0;
  uint32_t local_vertex_count = 0;
  uint32_t local_first_index = 0;
  uint32_t local_index_count = 0;
  Vec3 translation{};
  Quat4 rotation{};
  Vec3 scale{1.0f, 1.0f, 1.0f};

  bool has_flag(uint32_t flag) const {
    return (flags & flag) != 0u;
  }
};

struct RTInstanceTransformUpdate {
  vkpt::core::StableId entity_id = 0;
  uint32_t instance_index = kInvalidRTInstanceIndex;
  uint32_t flags = kRTInstanceFlagNone;
  uint32_t transform_revision = 0;
  Vec3 translation{};
  Quat4 rotation{};
  Vec3 scale{1.0f, 1.0f, 1.0f};
};

struct RTTessellationRequest {
  uint32_t geometry_id = 0;
  uint32_t first_triangle = 0;
  uint32_t source_triangle_count = 0;
  uint32_t factor = 1;
  uint32_t generated_vertex_count = 0;
  uint32_t generated_index_count = 0;
  uint64_t cache_key = 0;
  Vec3 projection_center{};
  float projection_radius = 0.0f;
  uint32_t projection_mode = 0;
  bool gpu_preferred = true;
  bool cache_generated_geometry = true;
  bool displacement = false;
};

struct RTTriangle {
  uint32_t i0 = 0;
  uint32_t i1 = 0;
  uint32_t i2 = 0;
};

struct RTSdfPrimitive {
  SdfShape shape = SdfShape::Unknown;
  Vec3 position{};
  Vec3 scale{1.0f, 1.0f, 1.0f};
  Vec3 rotation{};
  uint32_t material_index = 0;
  float radius = 0.5f;
  float param_a = 0.0f;
  float param_b = 0.0f;
};

struct RTHitLight {
  Vec3 position{};
  Vec3 color{1.0f, 1.0f, 1.0f};
  float intensity = 0.0f;
  float radius = 0.0f;
  Vec3 direction{0.0f, -1.0f, 0.0f};
  float spot_inner_cos = -1.0f;
  float spot_outer_cos = -1.0f;
};

/// Flattened path-tracer scene snapshot consumed by CPU, Vulkan, and D3D12 backends.
///
/// Scene extraction resolves the editor/ECS representation into packed arrays:
/// triangle instances point into global index streams, dynamic instances also
/// retain local ranges and transform state, and SDF/light/material tables are
/// laid out so backends can upload them directly. Treat this as a snapshot; live
/// ECS mutation should produce transform/material deltas or a new snapshot.
struct PathTracerSceneSnapshot {
  std::vector<Vec3> vertices;
  std::vector<Vec2> texcoords;
  std::vector<uint32_t> indices;  // triangle index stream in triples
  // Local mesh data for dynamic instances. Compatibility renderers can keep
  // using baked vertices/indices; dynamic-aware backends use these ranges plus
  // RTInstance transforms to avoid changing vertex/index buffers per physics step.
  std::vector<Vec3> local_vertices;
  std::vector<uint32_t> local_indices;
  std::vector<RTInstance> instances;
  std::vector<RTTessellationRequest> tessellation_requests;
  std::vector<RTSdfPrimitive> sdf_primitives;
  std::vector<RTMaterial> materials;
  std::vector<std::string> textures;
  std::vector<RTHitLight> lights;
  Vec3 environment_color{0.0f, 0.0f, 0.0f};
  std::vector<Vec3> environment_map;
  Vec3 environment_map_scale{0.0f, 0.0f, 0.0f};
  uint32_t environment_map_width = 0u;
  uint32_t environment_map_height = 0u;
  Vec3 camera_position{};
  Vec3 camera_target{0.0f, 0.0f, -1.0f};
  Vec3 camera_up{0.0f, 1.0f, 0.0f};
  float camera_fov_deg = 60.0f;
  float camera_focal_length_mm = 35.0f;
  float camera_sensor_width_mm = 36.0f;
  float camera_sensor_height_mm = 24.0f;
  float camera_aperture_radius = 0.0f;
  float camera_focus_distance = 0.0f;
  float camera_f_stop = 0.0f;
  float camera_shutter_seconds = 0.0166666675f;
  float camera_iso = 100.0f;
  float camera_exposure_compensation = 0.0f;
  float camera_white_balance_kelvin = 6500.0f;
  uint32_t camera_iris_blade_count = 0u;
  float camera_iris_rotation_degrees = 0.0f;
  float camera_iris_roundness = 1.0f;
  float camera_anamorphic_squeeze = 1.0f;
};

struct SceneParticleFrameState {
  vkpt::core::FrameIndex frame = 0;
  float seconds = 0.0f;
  float delta_seconds = 1.0f / 24.0f;
  bool advance_emitters = false;
};

struct RTCameraState {
  Vec3 position{};
  Vec3 target{0.0f, 0.0f, -1.0f};
  Vec3 up{0.0f, 1.0f, 0.0f};
  float fov_deg = 60.0f;
  float focal_length_mm = 35.0f;
  float sensor_width_mm = 36.0f;
  float sensor_height_mm = 24.0f;
  float aperture_radius = 0.0f;
  float focus_distance = 0.0f;
  float f_stop = 0.0f;
  float shutter_seconds = 0.0166666675f;
  float iso = 100.0f;
  float exposure_compensation = 0.0f;
  float white_balance_kelvin = 6500.0f;
  uint32_t iris_blade_count = 0u;
  float iris_rotation_degrees = 0.0f;
  float iris_roundness = 1.0f;
  float anamorphic_squeeze = 1.0f;
};

struct RTMaterialUpdate {
  uint32_t material_index = 0u;
  RTMaterial material{};
};

struct RTLightUpdate {
  uint32_t light_index = 0u;
  RTHitLight light{};
};

struct RTInstanceMaterialUpdate {
  vkpt::core::StableId entity_id = 0;
  uint32_t instance_index = kInvalidRTInstanceIndex;
  uint32_t material_index = 0u;
};

struct RTSceneDeltaUpdate {
  std::vector<RTMaterialUpdate> materials;
  std::vector<RTLightUpdate> lights;
  std::vector<RTInstanceMaterialUpdate> instance_materials;
  bool environment_color_changed = false;
  Vec3 environment_color{};
};

enum class RenderUpdateReason : std::uint8_t {
  Unknown,
  PhysicsMotion,
  EditorGizmoMotion,
  ScriptTransformMotion,
  CameraMotion,
  MaterialEdit,
  LightEdit,
  StructuralSceneEdit,
  SceneLoad,
  ExplicitUserReload,
  LegacyUnknown,
};

enum class PathTracerLifecycle : std::uint8_t {
  Uninitialized,
  Configured,
  SceneLoaded,
  Ready,
  Failed,
};

inline const char* ToString(PathTracerLifecycle lifecycle) {
  switch (lifecycle) {
    case PathTracerLifecycle::Uninitialized:
      return "uninitialized";
    case PathTracerLifecycle::Configured:
      return "configured";
    case PathTracerLifecycle::SceneLoaded:
      return "scene_loaded";
    case PathTracerLifecycle::Ready:
      return "ready";
    case PathTracerLifecycle::Failed:
      return "failed";
  }
  return "unknown";
}

enum class PathTracerAccumulationResetReason : std::uint8_t {
  Topology,
  Transform,
  Camera,
  Material,
  External,
};

inline const char* ToString(PathTracerAccumulationResetReason reason) {
  switch (reason) {
    case PathTracerAccumulationResetReason::Topology:
      return "topology";
    case PathTracerAccumulationResetReason::Transform:
      return "transform";
    case PathTracerAccumulationResetReason::Camera:
      return "camera";
    case PathTracerAccumulationResetReason::Material:
      return "material";
    case PathTracerAccumulationResetReason::External:
      return "external";
  }
  return "external";
}

inline PathTracerAccumulationResetReason AccumulationResetReasonFromUpdateReason(
    RenderUpdateReason reason) {
  switch (reason) {
    case RenderUpdateReason::PhysicsMotion:
    case RenderUpdateReason::EditorGizmoMotion:
    case RenderUpdateReason::ScriptTransformMotion:
      return PathTracerAccumulationResetReason::Transform;
    case RenderUpdateReason::CameraMotion:
      return PathTracerAccumulationResetReason::Camera;
    case RenderUpdateReason::MaterialEdit:
    case RenderUpdateReason::LightEdit:
      return PathTracerAccumulationResetReason::Material;
    case RenderUpdateReason::StructuralSceneEdit:
    case RenderUpdateReason::SceneLoad:
    case RenderUpdateReason::ExplicitUserReload:
      return PathTracerAccumulationResetReason::Topology;
    case RenderUpdateReason::Unknown:
    case RenderUpdateReason::LegacyUnknown:
      return PathTracerAccumulationResetReason::External;
  }
  return PathTracerAccumulationResetReason::External;
}

enum class TransformFallbackPolicy : std::uint8_t {
  NoFallback,
  AllowDynamicAcceleration,
  AllowFullStaticAccelerationBuild,
  AllowFullSceneReload,
};

struct InstanceTransformUpdateOptions {
  RenderUpdateReason reason = RenderUpdateReason::Unknown;
  TransformFallbackPolicy fallback_policy = TransformFallbackPolicy::NoFallback;
  bool reset_accumulation = true;
  bool coalesce = false;
  bool allow_partial = false;
  vkpt::core::FrameIndex source_frame = 0;
  const char* source_system = nullptr;
};

enum class InstanceTransformUpdateStatus : std::uint8_t {
  AppliedMetadataOnly,
  AppliedInstanceBufferOnly,
  AppliedDynamicAccelUpdate,
  AppliedFullStaticAccelRebuild,
  AppliedFullSceneReload,
  BlockedNeedsFullStaticAccelRebuild,
  BlockedNeedsFullSceneReload,
  Unsupported,
  Failed,
};

struct InstanceTransformPlan {
  InstanceTransformUpdateStatus status = InstanceTransformUpdateStatus::Unsupported;
  std::uint32_t requested_count = 0u;
  std::uint32_t matched_count = 0u;
  const char* message = nullptr;

  bool can_apply_without_full_fallback() const {
    return status == InstanceTransformUpdateStatus::AppliedMetadataOnly ||
           status == InstanceTransformUpdateStatus::AppliedInstanceBufferOnly ||
           status == InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate;
  }
};

inline std::string_view TransformPlanMessage(
    const InstanceTransformPlan& plan) noexcept {
  return plan.message != nullptr ? std::string_view(plan.message) : std::string_view{};
}

inline bool EquivalentInstanceTransformPlans(
    const InstanceTransformPlan& lhs,
    const InstanceTransformPlan& rhs) noexcept {
  return lhs.status == rhs.status &&
         lhs.requested_count == rhs.requested_count &&
         lhs.matched_count == rhs.matched_count &&
         TransformPlanMessage(lhs) == TransformPlanMessage(rhs);
}

struct InstanceTransformUpdateResult {
  InstanceTransformUpdateStatus status = InstanceTransformUpdateStatus::Unsupported;
  std::uint32_t requested_count = 0u;
  std::uint32_t applied_count = 0u;
  double validate_ms = 0.0;
  double upload_ms = 0.0;
  double dynamic_accel_ms = 0.0;
  double full_rebuild_ms = 0.0;
  const char* message = nullptr;

  bool applied() const {
    return status == InstanceTransformUpdateStatus::AppliedMetadataOnly ||
           status == InstanceTransformUpdateStatus::AppliedInstanceBufferOnly ||
           status == InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate ||
           status == InstanceTransformUpdateStatus::AppliedFullStaticAccelRebuild ||
           status == InstanceTransformUpdateStatus::AppliedFullSceneReload;
  }
};

inline TransformFallbackPolicy DefaultTransformFallbackPolicy(RenderUpdateReason reason) {
  switch (reason) {
    case RenderUpdateReason::PhysicsMotion:
    case RenderUpdateReason::EditorGizmoMotion:
    case RenderUpdateReason::ScriptTransformMotion:
      return TransformFallbackPolicy::AllowDynamicAcceleration;
    case RenderUpdateReason::StructuralSceneEdit:
    case RenderUpdateReason::SceneLoad:
    case RenderUpdateReason::ExplicitUserReload:
      return TransformFallbackPolicy::AllowFullSceneReload;
    case RenderUpdateReason::Unknown:
    case RenderUpdateReason::CameraMotion:
    case RenderUpdateReason::MaterialEdit:
    case RenderUpdateReason::LightEdit:
    case RenderUpdateReason::LegacyUnknown:
      return TransformFallbackPolicy::NoFallback;
  }
  return TransformFallbackPolicy::NoFallback;
}

inline bool IsContinuousTransformMotion(RenderUpdateReason reason) {
  return reason == RenderUpdateReason::PhysicsMotion ||
         reason == RenderUpdateReason::EditorGizmoMotion ||
         reason == RenderUpdateReason::ScriptTransformMotion;
}

inline bool TransformUpdateStatusAllowedByPolicy(InstanceTransformUpdateStatus status,
                                                 TransformFallbackPolicy policy) {
  switch (status) {
    case InstanceTransformUpdateStatus::AppliedMetadataOnly:
    case InstanceTransformUpdateStatus::AppliedInstanceBufferOnly:
      return true;
    case InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate:
      return policy >= TransformFallbackPolicy::AllowDynamicAcceleration;
    case InstanceTransformUpdateStatus::AppliedFullStaticAccelRebuild:
    case InstanceTransformUpdateStatus::BlockedNeedsFullStaticAccelRebuild:
      return policy >= TransformFallbackPolicy::AllowFullStaticAccelerationBuild;
    case InstanceTransformUpdateStatus::AppliedFullSceneReload:
    case InstanceTransformUpdateStatus::BlockedNeedsFullSceneReload:
      return policy >= TransformFallbackPolicy::AllowFullSceneReload;
    case InstanceTransformUpdateStatus::Unsupported:
    case InstanceTransformUpdateStatus::Failed:
      return false;
  }
  return false;
}

inline const char* ToString(RenderUpdateReason reason) {
  switch (reason) {
    case RenderUpdateReason::Unknown:
      return "unknown";
    case RenderUpdateReason::PhysicsMotion:
      return "physics_motion";
    case RenderUpdateReason::EditorGizmoMotion:
      return "editor_gizmo_motion";
    case RenderUpdateReason::ScriptTransformMotion:
      return "script_transform_motion";
    case RenderUpdateReason::CameraMotion:
      return "camera_motion";
    case RenderUpdateReason::MaterialEdit:
      return "material_edit";
    case RenderUpdateReason::LightEdit:
      return "light_edit";
    case RenderUpdateReason::StructuralSceneEdit:
      return "structural_scene_edit";
    case RenderUpdateReason::SceneLoad:
      return "scene_load";
    case RenderUpdateReason::ExplicitUserReload:
      return "explicit_user_reload";
    case RenderUpdateReason::LegacyUnknown:
      return "legacy_unknown";
  }
  return "unknown";
}

inline const char* ToString(TransformFallbackPolicy policy) {
  switch (policy) {
    case TransformFallbackPolicy::NoFallback:
      return "no_fallback";
    case TransformFallbackPolicy::AllowDynamicAcceleration:
      return "allow_dynamic_acceleration";
    case TransformFallbackPolicy::AllowFullStaticAccelerationBuild:
      return "allow_full_static_acceleration_build";
    case TransformFallbackPolicy::AllowFullSceneReload:
      return "allow_full_scene_reload";
  }
  return "unknown";
}

inline const char* ToString(InstanceTransformUpdateStatus status) {
  switch (status) {
    case InstanceTransformUpdateStatus::AppliedMetadataOnly:
      return "applied_metadata_only";
    case InstanceTransformUpdateStatus::AppliedInstanceBufferOnly:
      return "applied_instance_buffer_only";
    case InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate:
      return "applied_dynamic_accel_update";
    case InstanceTransformUpdateStatus::AppliedFullStaticAccelRebuild:
      return "applied_full_static_accel_rebuild";
    case InstanceTransformUpdateStatus::AppliedFullSceneReload:
      return "applied_full_scene_reload";
    case InstanceTransformUpdateStatus::BlockedNeedsFullStaticAccelRebuild:
      return "blocked_needs_full_static_accel_rebuild";
    case InstanceTransformUpdateStatus::BlockedNeedsFullSceneReload:
      return "blocked_needs_full_scene_reload";
    case InstanceTransformUpdateStatus::Unsupported:
      return "unsupported";
    case InstanceTransformUpdateStatus::Failed:
      return "failed";
  }
  return "unknown";
}

inline bool IsAppliedTransformUpdateStatus(InstanceTransformUpdateStatus status) {
  return status == InstanceTransformUpdateStatus::AppliedMetadataOnly ||
         status == InstanceTransformUpdateStatus::AppliedInstanceBufferOnly ||
         status == InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate ||
         status == InstanceTransformUpdateStatus::AppliedFullStaticAccelRebuild ||
         status == InstanceTransformUpdateStatus::AppliedFullSceneReload;
}

enum class RTInstanceTransformApplyMode {
  MetadataOnly,
  RebakeCpuVertices,
};

RTCameraState ExtractCameraState(const PathTracerSceneSnapshot& scene);
void ApplyCameraState(PathTracerSceneSnapshot& scene, const RTCameraState& camera);
bool ApplyInstanceTransformUpdates(PathTracerSceneSnapshot& scene,
                                   const std::vector<RTInstanceTransformUpdate>& updates,
                                   RTInstanceTransformApplyMode mode =
                                       RTInstanceTransformApplyMode::RebakeCpuVertices);
bool ApplySceneDeltaUpdate(PathTracerSceneSnapshot& scene, const RTSceneDeltaUpdate& update);
void MergeSceneDeltaUpdates(RTSceneDeltaUpdate& dst, const RTSceneDeltaUpdate& src);
std::optional<RTSceneDeltaUpdate> BuildSceneDeltaUpdate(const PathTracerSceneSnapshot& before,
                                                        const PathTracerSceneSnapshot& after);

struct GpuLayoutField {
  std::string struct_name;
  std::string field;
  std::size_t cpu_offset = 0u;
  std::size_t cpu_size = 0u;
  std::size_t cpu_alignment = 0u;
  std::size_t gpu_offset = 0u;
  std::size_t gpu_size = 0u;
  std::size_t gpu_alignment = 0u;
};

struct RTSceneLayoutManifest {
  std::string schema_version = "1.0";
  std::size_t total_cpu_bytes = 0u;
  std::size_t total_gpu_bytes = 0u;
  std::vector<GpuLayoutField> fields;
};

struct GpuSceneBufferLayoutContract {
  std::uint32_t material_stride_floats = 16u;
  std::uint32_t instance_stride_u32 = 24u;
  std::uint32_t sdf_stride_floats = 16u;
  std::uint32_t packed_triangle_stride_floats = 18u;
  std::uint32_t bvh_node_stride_floats = 8u;
  std::uint32_t light_stride_floats = 16u;
  std::uint32_t film_pixel_stride_floats = 4u;
};

inline constexpr GpuSceneBufferLayoutContract kStandardGpuSceneBufferLayout{};

struct PathTracerLifecycleContract {
  bool configure_before_scene_load = true;
  bool load_scene_before_acceleration_build = true;
  bool reset_clears_accumulation_and_counters = true;
  // reset_accumulation() is a film-only epoch reset. It must not invalidate
  // scene buffers or a valid acceleration structure.
  bool reset_preserves_acceleration = true;
  bool camera_update_preserves_scene_buffers = true;
  bool scene_delta_preserves_acceleration = true;
  bool resolve_is_available_after_render = true;
};

struct PathTracerTransformUpdateContract {
  TransformFallbackPolicy physics_default_fallback = TransformFallbackPolicy::AllowDynamicAcceleration;
  TransformFallbackPolicy editor_default_fallback = TransformFallbackPolicy::AllowDynamicAcceleration;
  TransformFallbackPolicy script_default_fallback = TransformFallbackPolicy::AllowDynamicAcceleration;
  TransformFallbackPolicy structural_default_fallback = TransformFallbackPolicy::AllowFullSceneReload;
  // plan_instance_transform_update() is a pure planning query. With the same
  // tracer state, scene snapshot, updates, and options, repeated calls must
  // return equivalent status/count/message plans and must not commit scene,
  // film, acceleration, or counter changes.
  bool plan_is_stable_for_same_inputs = true;
  bool plan_is_commit_free = true;
  bool plan_before_apply = true;
  bool failed_apply_must_not_commit = true;
  bool applied_update_resets_accumulation_by_default = true;
};

struct PathTracerStateTransitionContract {
  PathTracerLifecycle from = PathTracerLifecycle::Uninitialized;
  const char* operation = "";
  PathTracerLifecycle to = PathTracerLifecycle::Uninitialized;
  const char* postcondition = "";
};

struct PathTracerStandardContract {
  std::string schema_version = "pathtracer.contract.v1";
  PathTracerLifecycleContract lifecycle{};
  PathTracerTransformUpdateContract transforms{};
  GpuSceneBufferLayoutContract gpu_layout{};
  std::array<PathTracerStateTransitionContract, 5> state_machine{{
      {PathTracerLifecycle::Uninitialized,
       "configure",
       PathTracerLifecycle::Configured,
       "settings and film storage are initialized; no scene or acceleration is valid"},
      {PathTracerLifecycle::Configured,
       "load_scene_snapshot",
       PathTracerLifecycle::SceneLoaded,
       "render-time scene data is loaded; acceleration must be rebuilt before rendering"},
      {PathTracerLifecycle::SceneLoaded,
       "build_or_update_acceleration",
       PathTracerLifecycle::Ready,
       "acceleration is valid and the tracer is ready to render"},
      {PathTracerLifecycle::Ready,
       "reset_accumulation",
       PathTracerLifecycle::Ready,
       "film accumulation and counters are cleared; acceleration remains valid"},
      {PathTracerLifecycle::Ready,
       "update_scene_delta",
       PathTracerLifecycle::Ready,
       "material/light/transform deltas preserve acceleration unless the call fails"},
  }};
};

struct PathTracerStatus {
  std::string backend;
  PathTracerLifecycle lifecycle = PathTracerLifecycle::Uninitialized;
  bool scene_loaded = false;
  bool accel_valid = false;
  bool ready_to_render = false;
  std::uint32_t current_sample = 0u;
  std::uint64_t last_sample_us = 0u;
  std::uint64_t total_samples = 0u;
  std::uint64_t accumulation_gen = 0u;
  std::string last_error;
};

inline vkpt::core::Status PathTracerOperationStatus(
    const char* operation,
    bool succeeded,
    const PathTracerStatus& after,
    vkpt::core::Status::Code error_code = vkpt::core::Status::Code::InternalError) {
  if (succeeded && after.last_error.empty()) {
    return vkpt::core::Status::ok(std::string(operation) + " succeeded");
  }
  std::string message = after.last_error;
  if (message.empty()) {
    message = std::string(operation) + " failed";
  }
  return vkpt::core::Status::error(error_code, std::move(message));
}

struct FilmLdr {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba8;
};

struct FilmHdr {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<float> rgbf;
};

struct FilmReadbackToken {
  std::uint64_t id = 0u;
  std::uint64_t fence_value = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;

  explicit operator bool() const noexcept { return id != 0u; }
};

enum class FilmReadbackState : std::uint8_t {
  Invalid = 0,
  Pending,
  Ready,
  Failed,
  Unsupported,
};

struct FilmReadbackResult {
  FilmReadbackState state = FilmReadbackState::Invalid;
  FilmLdr ldr{};
  std::string error;

  bool ready() const noexcept { return state == FilmReadbackState::Ready; }
};

struct RenderTile {
  uint32_t x = 0u;
  uint32_t y = 0u;
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint32_t sample_index = 0u;
  uint32_t tile_id = 0u;
  uint32_t gpu_id = 0u;
};

// Apply resolve pipeline: exposure, tone map, gamma, clamp, LDR output.
FilmLdr ApplyFilmResolve(const FilmHdr& hdr, const FilmResolveSettings& settings);
FilmResolveSettings CameraAdjustedFilmResolveSettings(const FilmResolveSettings& base,
                                                       const PathTracerSceneSnapshot& scene);
Vec3 SampleSceneEnvironment(const PathTracerSceneSnapshot& scene, const Vec3& direction);
Vec3 WhiteBalanceScale(float kelvin);
std::string SerializeFilmResolveSettings(const FilmResolveSettings& settings);

class FilmBuffer {
 public:
  FilmBuffer() = default;
  explicit FilmBuffer(uint32_t width, uint32_t height);

  void resize(uint32_t width, uint32_t height);
  void clear();
  void add_sample(uint32_t x, uint32_t y, const Vec3& color);
  void set_pixel_raw(uint32_t x,
                     uint32_t y,
                     const Vec3& accumulation,
                     uint32_t sample_count,
                     float invalid_samples = 0.0f);
  void reset_pixel(uint32_t x, uint32_t y);
  bool copy_from(const FilmBuffer& src);
  // Copy raw accumulation and sample counts from src for rows [start_y, end_y).
  // Overwrites any existing data in that row range.
  void import_tile(const FilmBuffer& src, uint32_t start_y, uint32_t end_y);
  FilmLdr resolve_ldr() const;
  FilmLdr resolve_ldr(const FilmResolveSettings& settings) const;
  FilmHdr resolve_hdr() const;
  void set_resolve_settings(const FilmResolveSettings& settings) { m_resolveSettings = settings; }
  const FilmResolveSettings& resolve_settings() const { return m_resolveSettings; }

  uint32_t width() const { return m_width; }
  uint32_t height() const { return m_height; }
  const std::vector<Vec3>& raw() const { return m_accumulation; }
  const std::vector<uint32_t>& sample_counts() const { return m_sampleCounts; }
  const std::vector<float>& invalid_samples() const { return m_invalidSamples; }

 private:
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  std::vector<Vec3> m_accumulation;
  std::vector<uint32_t> m_sampleCounts;
  std::vector<float> m_invalidSamples;
  FilmResolveSettings m_resolveSettings{};
};

struct RayQueryHit {
  bool hit = false;
  float t = 0.0f;
  Vec3 position{};
  Vec3 normal{};
  uint32_t material_index = 0;
  uint32_t primitive_index = 0;
};

struct RayQueryStats {
  uint64_t triangle_tests = 0;
  uint64_t triangle_hits = 0;
  uint64_t bvh_node_visits = 0;
  uint64_t bvh_leaf_visits = 0;
};

struct RayAcceleratorBuildInfo {
  bool built = false;
  std::size_t primitive_count = 0;
  std::size_t node_count = 0;
  std::size_t leaf_count = 0;
  double build_ms = 0.0;
  bool deterministic = false;
};

class IRayAccelerator {
 public:
  virtual ~IRayAccelerator() = default;

  virtual bool build(const PathTracerSceneSnapshot& scene, bool deterministic) = 0;
  // Pure planning query. For the same built accelerator state, scene snapshot,
  // updates, and options, repeated calls must return equivalent plans and must
  // not mutate acceleration data.
  virtual InstanceTransformPlan plan_instance_transform_update(
      const PathTracerSceneSnapshot& /*scene*/,
      std::span<const RTInstanceTransformUpdate> updates,
      const InstanceTransformUpdateOptions& /*options*/) const {
    return {
        InstanceTransformUpdateStatus::Unsupported,
        static_cast<std::uint32_t>(updates.size()),
        0u,
        "accelerator does not support instance transform updates"};
  }
  virtual InstanceTransformUpdateResult apply_instance_transform_update(
      const PathTracerSceneSnapshot& /*scene*/,
      std::span<const RTInstanceTransformUpdate> updates,
      const InstanceTransformUpdateOptions& /*options*/) {
    return {
        InstanceTransformUpdateStatus::Unsupported,
        static_cast<std::uint32_t>(updates.size()),
        0u,
        0.0,
        0.0,
        0.0,
        0.0,
        "accelerator does not support instance transform updates"};
  }
  virtual bool intersect(const Ray& ray, RayQueryHit& out, RayQueryStats* stats = nullptr) const = 0;
  virtual RayAcceleratorBuildInfo build_info() const = 0;
  virtual void reset() = 0;
};

class ICpuRayKernel {
 public:
  virtual ~ICpuRayKernel() = default;

  virtual std::string_view name() const = 0;
  virtual bool set_accelerator(IRayAccelerator* accelerator) = 0;
  virtual bool render_sample_batch(uint32_t start_y,
                                   uint32_t end_y,
                                   uint32_t sample_index,
                                   uint32_t frame_index) = 0;
};

std::unique_ptr<IRayAccelerator> CreateCpuBvhAccelerator();

class IPathTracer {
 public:
  virtual ~IPathTracer() = default;

  // State contract:
  // Uninitialized -> configure() -> Configured
  // Configured -> load_scene_snapshot() -> SceneLoaded
  // SceneLoaded -> build_or_update_acceleration() -> Ready
  // Ready -> reset_accumulation() -> Ready
  //
  // reset_accumulation() is film-only: it clears accumulated radiance and
  // counters, advances accumulation_gen, and preserves scene_loaded/accel_valid.
  // BuildStandardPathTracerContract() exposes the same transition table for
  // tests, scripting, and future backend conformance checks.
  virtual vkpt::core::Status configure(const RenderSettings& settings) = 0;
  virtual vkpt::core::Status configure(const PathTraceSettings& settings) {
    return configure(MakeRenderSettings(settings));
  }
  vkpt::core::Status configure_status(const RenderSettings& settings) {
    return configure(settings);
  }
  vkpt::core::Status configure_status(const PathTraceSettings& settings) {
    return configure_status(MakeRenderSettings(settings));
  }
  // Applies render settings without invalidating uploaded scene data when a
  // backend can do so. Callers should fall back to configure/load/build when
  // this returns false.
  virtual bool update_render_settings(const RenderSettings& /*settings*/) { return false; }
  virtual vkpt::core::Status load_scene_snapshot(const PathTracerSceneSnapshot& scene) = 0;
  vkpt::core::Status load_scene_snapshot_status(const PathTracerSceneSnapshot& scene) {
    return load_scene_snapshot(scene);
  }
  virtual vkpt::core::Status build_or_update_acceleration() = 0;
  vkpt::core::Status build_or_update_acceleration_status() {
    return build_or_update_acceleration();
  }
  virtual bool reset_accumulation() = 0;
  // Replace accumulated film history without changing scene or acceleration data.
  // CPU backends use this for camera reprojection and per-pixel history resets.
  virtual bool replace_film_history(const FilmBuffer& /*film*/) { return false; }
  // Update only camera pose without re-uploading geometry. Returns false if
  // not supported (caller should fall back to load_scene_snapshot).
  virtual bool update_camera(const Vec3& /*pos*/, const Vec3& /*target*/,
                             const Vec3& /*up*/, float /*fov_deg*/) { return false; }
  // Update camera pose, lens, and exposure state without rebuilding geometry.
  virtual bool update_camera_state(const RTCameraState& /*camera*/) { return false; }
  // Update only dynamic instance transforms. Returns false if the backend still
  // needs a full scene snapshot/acceleration rebuild for moving objects.
  virtual bool update_instance_transforms(
      const std::vector<RTInstanceTransformUpdate>& /*updates*/) { return false; }
  // Pure planning query. With the same tracer state, scene snapshot, updates,
  // and options, repeated calls must return EquivalentInstanceTransformPlans().
  // The call must not mutate scene data, acceleration state, film contents, or
  // sample/accumulation counters. apply_instance_transform_update() still
  // revalidates and returns the authoritative commit result.
  virtual InstanceTransformPlan plan_instance_transform_update(
      std::span<const RTInstanceTransformUpdate> updates,
      const InstanceTransformUpdateOptions& /*options*/) const {
    return {
        InstanceTransformUpdateStatus::Unsupported,
        static_cast<std::uint32_t>(updates.size()),
        0u,
        "backend does not support transactional instance transform updates"};
  }
  virtual InstanceTransformUpdateResult apply_instance_transform_update(
      std::span<const RTInstanceTransformUpdate> updates,
      const InstanceTransformUpdateOptions& options) {
    const auto plan = plan_instance_transform_update(updates, options);
    return {
        plan.status,
        plan.requested_count,
        0u,
        0.0,
        0.0,
        0.0,
        0.0,
        plan.message};
  }
  // Update material/light/environment records without rebuilding geometry or
  // acceleration structures. Returns false when the backend requires a full
  // scene upload for this delta.
  virtual bool update_scene_delta(const RTSceneDeltaUpdate& /*update*/) { return false; }
  virtual bool render_tile(const RenderTile& tile, uint32_t frame_index) = 0;
  virtual bool supports_tile_rendering() const { return false; }
  virtual bool render_tile_cancellable(const RenderTile& tile,
                                       uint32_t frame_index,
                                       std::stop_token stop) {
    if (stop.stop_requested()) {
      return false;
    }
    return render_tile(tile, frame_index);
  }
  virtual FilmLdr resolve_ldr() const = 0;
  virtual FilmHdr resolve_hdr() const = 0;
  virtual FilmReadbackToken request_film_readback() { return {}; }
  virtual FilmReadbackResult poll_film(FilmReadbackToken /*token*/) {
    return {
        FilmReadbackState::Unsupported,
        {},
        "backend does not support split film readback"};
  }
  virtual SampleCounters read_counters() const = 0;
  virtual PathTracerStatus status() const { return {}; }
  virtual void shutdown() = 0;
  virtual const FilmBuffer& film() const = 0;
};

float MisWeight(float pdf_a, float pdf_b);

class NullPathTracer final : public IPathTracer {
 public:
  using IPathTracer::configure;

  vkpt::core::Status configure(const RenderSettings& settings) override;
  vkpt::core::Status load_scene_snapshot(const PathTracerSceneSnapshot& scene) override;
  vkpt::core::Status build_or_update_acceleration() override;
  bool reset_accumulation() override;
  bool replace_film_history(const FilmBuffer& film) override;
  bool update_camera(const Vec3& pos, const Vec3& target, const Vec3& up, float fov_deg) override;
  bool update_camera_state(const RTCameraState& camera) override;
  bool update_instance_transforms(const std::vector<RTInstanceTransformUpdate>& updates) override;
  InstanceTransformPlan plan_instance_transform_update(
      std::span<const RTInstanceTransformUpdate> updates,
      const InstanceTransformUpdateOptions& options) const override;
  InstanceTransformUpdateResult apply_instance_transform_update(
      std::span<const RTInstanceTransformUpdate> updates,
      const InstanceTransformUpdateOptions& options) override;
  bool update_scene_delta(const RTSceneDeltaUpdate& update) override;
  bool render_tile(const RenderTile& tile, uint32_t frame_index) override;
  bool supports_tile_rendering() const override { return true; }
  FilmLdr resolve_ldr() const override { return m_film.resolve_ldr(); }
  FilmHdr resolve_hdr() const override { return m_film.resolve_hdr(); }
  SampleCounters read_counters() const override { return m_counters; }
  PathTracerStatus status() const override;
  void shutdown() override;
  const FilmBuffer& film() const override { return m_film; }

 private:
  RenderSettings m_settings{};
  PathTracerSceneSnapshot m_scene{};
  FilmBuffer m_film{};
  SampleCounters m_counters{};
  bool m_configured = false;
  bool m_has_scene = false;
  bool m_accel_valid = false;
  std::uint32_t m_current_sample = 0u;
  std::uint64_t m_last_sample_us = 0u;
  std::uint64_t m_accumulation_gen = 0u;
  PathTracerAccumulationResetReason m_next_reset_reason =
      PathTracerAccumulationResetReason::External;
  std::string m_last_error;
};

class ScalarCpuPathTracer final : public IPathTracer, public ICpuRayKernel {
 public:
  using IPathTracer::configure;

  ~ScalarCpuPathTracer() override;

  vkpt::core::Status configure(const RenderSettings& settings) override;
  vkpt::core::Status load_scene_snapshot(const PathTracerSceneSnapshot& scene) override;
  vkpt::core::Status build_or_update_acceleration() override;
  bool reset_accumulation() override;
  bool replace_film_history(const FilmBuffer& film) override;
  bool update_camera(const Vec3& pos, const Vec3& target, const Vec3& up, float fov_deg) override;
  bool update_camera_state(const RTCameraState& camera) override;
  bool update_instance_transforms(const std::vector<RTInstanceTransformUpdate>& updates) override;
  InstanceTransformPlan plan_instance_transform_update(
      std::span<const RTInstanceTransformUpdate> updates,
      const InstanceTransformUpdateOptions& options) const override;
  InstanceTransformUpdateResult apply_instance_transform_update(
      std::span<const RTInstanceTransformUpdate> updates,
      const InstanceTransformUpdateOptions& options) override;
  bool update_scene_delta(const RTSceneDeltaUpdate& update) override;
  bool render_sample_batch(uint32_t start_y, uint32_t end_y, uint32_t sample_index, uint32_t frame_index) override;
  bool render_tile(const RenderTile& tile, uint32_t frame_index) override;
  bool render_tile_cancellable(const RenderTile& tile,
                               uint32_t frame_index,
                               std::stop_token stop) override;
  bool supports_tile_rendering() const override { return true; }
  std::string_view name() const override { return "scalar-cpu"; }
  bool set_accelerator(IRayAccelerator* accelerator) override;
  bool render_sample_pixels(const uint32_t* pixel_indices,
                            uint32_t pixel_count,
                            uint32_t sample_index,
                            uint32_t frame_index);
  FilmLdr resolve_ldr() const override { return m_film.resolve_ldr(); }
  FilmHdr resolve_hdr() const override { return m_film.resolve_hdr(); }
  SampleCounters read_counters() const override;
  PathTracerStatus status() const override;
  const FilmBuffer& film() const override { return m_film; }
  void shutdown() override;

 private:
  struct alignas(64) RenderBatchAccum {
    float lum_sum = 0.0f;
    float sample_max = 0.0f;
    std::uint64_t sample_count = 0u;
    std::uint64_t ray_count = 0u;
    uint32_t min_pixel = std::numeric_limits<uint32_t>::max();
    uint32_t max_pixel = 0u;
    SampleCounters counters{};
  };

  struct Hit {
    bool hit = false;
    float t = 0.0f;
    Vec3 position{};
    Vec3 normal{};
    uint32_t material_index = 0;
  };

  struct Rng {
    explicit Rng(const SampleKey& key);
    float next01();
    Vec3 unit_sphere();

   private:
    uint64_t m_state = 0;
  };

  Vec3 make_unit(const Vec3& value) const;
  Vec3 sample_hemisphere(Rng& rng, const Vec3& normal) const;
  // Sample Phong lobe around refl_dir; falls back to hemisphere if sample is
  // below the shading surface. exponent=0 → diffuse, exponent→∞ → mirror.
  Vec3 sample_phong_lobe(Rng& rng, const Vec3& refl_dir,
                         float exponent, const Vec3& normal) const;
  Vec3 trace(const Ray& ray,
             uint32_t sample_index,
             uint32_t frame_index,
             uint32_t path_id,
             uint32_t path_depth,
             uint64_t& ray_counter,
             SampleCounters& counters,
             Rng& rng);
  bool intersect_triangle(const RTTriangle& tri,
                          const Ray& ray,
                          float& t,
                          float& u,
                          float& v,
                          SampleCounters& counters) const;
  bool intersect_box(const RTSdfPrimitive& primitive,
                     const Ray& ray,
                     float& t,
                     Vec3& normal,
                     SampleCounters& counters) const;
  bool intersect_sphere(const RTSdfPrimitive& primitive,
                        const Ray& ray,
                        float& t,
                        Vec3& normal,
                        SampleCounters& counters) const;
  bool intersect_rounded_box(const RTSdfPrimitive& primitive,
                             const Ray& ray,
                             float& t,
                             Vec3& normal,
                             SampleCounters& counters) const;
  bool intersect_plane(const RTSdfPrimitive& primitive,
                       const Ray& ray,
                       float& t,
                       Vec3& normal,
                       SampleCounters& counters) const;
  bool intersect_torus(const RTSdfPrimitive& primitive,
                       const Ray& ray,
                       float& t,
                       Vec3& normal,
                       SampleCounters& counters) const;
  bool intersect_capsule(const RTSdfPrimitive& primitive,
                         const Ray& ray,
                         float& t,
                         Vec3& normal,
                         SampleCounters& counters) const;
  bool intersect_scene(const Ray& ray, Hit& out, SampleCounters& counters) const;
  NeeResult sample_direct_light(const Hit& hit,
                                const Vec3& view_dir,
                                Rng& rng,
                                SampleCounters& counters) const;
  float light_pdf_for_direction(const Vec3& position, const Vec3& direction) const;
  Ray camera_rays(uint32_t x,
                  uint32_t y,
                  uint32_t sample_index,
                  uint32_t frame_index,
                  uint32_t path_id,
                  uint64_t& sample_seed);
  void shade_pixel(uint32_t pixel,
                   uint32_t sample_index,
                   uint32_t frame_index,
                   RenderBatchAccum& accum);
  bool render_sample_contiguous_pixels(uint32_t first_pixel,
                                       uint32_t pixel_count,
                                       uint32_t sample_index,
                                       uint32_t frame_index);
  bool finish_render_batch(const std::vector<RenderBatchAccum>& locals,
                           uint32_t sample_index,
                           uint32_t frame_index);
  static Vec3 evaluate_bsdf(const RTMaterial& material, const Vec3& normal, const Vec3& in_dir, const Vec3& out_dir, float& pdf);

  RenderSettings m_settings;
  PathTraceSettings m_trace_settings;
  PathTracerSceneSnapshot m_scene;
  FilmBuffer m_film;
  mutable SampleCounters m_counters{};
  std::unique_ptr<IRayAccelerator> m_accelerator;
  IRayAccelerator* m_external_accelerator = nullptr;
  RayAcceleratorBuildInfo m_accel_info{};
  bool m_configured = false;
  bool m_has_scene = false;
  bool m_accel_valid = false;
  std::uint32_t m_current_sample = 0u;
  std::uint64_t m_last_sample_us = 0u;
  std::uint64_t m_accumulation_gen = 0u;
  PathTracerAccumulationResetReason m_next_reset_reason =
      PathTracerAccumulationResetReason::External;
  std::string m_last_error;
  uint32_t m_worker_count = 1;
  vkpt::cpu::SimdDispatchInfo m_simd_dispatch{};
  Vec3 m_camera_right{};
  Vec3 m_camera_up{};
  Vec3 m_camera_forward{};
};

vkpt::core::Result<PathTracerSceneSnapshot> BuildSceneDataFromDocument(const vkpt::scene::SceneDocument& doc);
vkpt::core::Result<PathTracerSceneSnapshot> BuildSceneDataFromDocumentAtFrame(
    const vkpt::scene::SceneDocument& doc,
    const SceneParticleFrameState& frame_state);
bool SavePngCompat(const std::string& path, const FilmLdr& image, std::string* error = nullptr);
bool SaveExrCompat(const std::string& path, const FilmHdr& image, std::string* error = nullptr);
vkpt::core::Result<RTSceneLayoutManifest> BuildPathTracerSceneSnapshotLayoutManifest(std::vector<std::string>* diagnostics = nullptr);
std::string SerializePathTracerSceneSnapshotLayoutManifest(const RTSceneLayoutManifest& manifest);
inline PathTracerStandardContract BuildStandardPathTracerContract() {
  return {};
}

inline bool ValidateStandardPathTracerContract(const PathTracerStandardContract& contract,
                                               std::vector<std::string>* diagnostics = nullptr) {
  if (diagnostics) {
    diagnostics->clear();
  }
  bool ok = true;
  auto require = [&](bool condition, const char* message) {
    if (!condition) {
      ok = false;
      if (diagnostics) {
        diagnostics->push_back(message);
      }
    }
  };

  require(contract.schema_version == "pathtracer.contract.v1",
          "unexpected path tracer contract schema version");
  require(contract.lifecycle.configure_before_scene_load,
          "path tracers must be configured before scene snapshots are loaded");
  require(contract.lifecycle.load_scene_before_acceleration_build,
          "path tracers must load a scene snapshot before acceleration build/update");
  require(contract.lifecycle.reset_clears_accumulation_and_counters,
          "reset_accumulation must clear film accumulation and counters");
  require(contract.lifecycle.reset_preserves_acceleration,
          "reset_accumulation must preserve valid acceleration structures");
  require(contract.lifecycle.resolve_is_available_after_render,
          "resolve APIs must be available after render_sample_batch");
  require(contract.state_machine.size() == 5u,
          "path tracer state machine must publish five standard transitions");
  require(contract.state_machine[0].from == PathTracerLifecycle::Uninitialized &&
              std::string_view(contract.state_machine[0].operation) == "configure" &&
              contract.state_machine[0].to == PathTracerLifecycle::Configured,
          "path tracer state machine missing configure transition");
  require(contract.state_machine[1].from == PathTracerLifecycle::Configured &&
              std::string_view(contract.state_machine[1].operation) == "load_scene_snapshot" &&
              contract.state_machine[1].to == PathTracerLifecycle::SceneLoaded,
          "path tracer state machine missing scene load transition");
  require(contract.state_machine[2].from == PathTracerLifecycle::SceneLoaded &&
              std::string_view(contract.state_machine[2].operation) == "build_or_update_acceleration" &&
              contract.state_machine[2].to == PathTracerLifecycle::Ready,
          "path tracer state machine missing acceleration build transition");
  require(contract.state_machine[3].from == PathTracerLifecycle::Ready &&
              std::string_view(contract.state_machine[3].operation) == "reset_accumulation" &&
              contract.state_machine[3].to == PathTracerLifecycle::Ready,
          "path tracer state machine missing film-only reset transition");
  require(contract.transforms.plan_before_apply,
          "transform updates must be planned before transactional apply");
  require(contract.transforms.plan_is_stable_for_same_inputs,
          "transform update planning must be stable for unchanged inputs");
  require(contract.transforms.plan_is_commit_free,
          "transform update planning must not commit backend state");
  require(contract.transforms.failed_apply_must_not_commit,
          "failed transform applies must not commit partial backend state");
  require(contract.transforms.applied_update_resets_accumulation_by_default,
          "applied transform updates should reset accumulation by default");
  require(contract.transforms.physics_default_fallback ==
              DefaultTransformFallbackPolicy(RenderUpdateReason::PhysicsMotion),
          "physics transform fallback policy differs from standard default");
  require(contract.transforms.editor_default_fallback ==
              DefaultTransformFallbackPolicy(RenderUpdateReason::EditorGizmoMotion),
          "editor transform fallback policy differs from standard default");
  require(contract.transforms.script_default_fallback ==
              DefaultTransformFallbackPolicy(RenderUpdateReason::ScriptTransformMotion),
          "script transform fallback policy differs from standard default");
  require(contract.transforms.structural_default_fallback ==
              DefaultTransformFallbackPolicy(RenderUpdateReason::StructuralSceneEdit),
          "structural transform fallback policy differs from standard default");
  require(contract.gpu_layout.material_stride_floats == 16u,
          "material stride must remain 16 floats");
  require(contract.gpu_layout.instance_stride_u32 == 24u,
          "instance stride must remain 24 uint32 values");
  require(contract.gpu_layout.sdf_stride_floats == 16u,
          "SDF stride must remain 16 floats");
  require(contract.gpu_layout.packed_triangle_stride_floats == 18u,
          "packed triangle stride must remain 18 floats");
  require(contract.gpu_layout.bvh_node_stride_floats == 8u,
          "BVH node stride must remain 8 floats");
  require(contract.gpu_layout.light_stride_floats == 16u,
          "light stride must remain 16 floats");
  require(contract.gpu_layout.film_pixel_stride_floats == 4u,
          "film pixel stride must remain 4 floats");

  if (ok && diagnostics) {
    diagnostics->push_back("standard path tracer contract is valid");
  }
  return ok;
}

inline InstanceTransformUpdateOptions MakeStandardTransformUpdateOptions(
    RenderUpdateReason reason,
    vkpt::core::FrameIndex source_frame = 0,
    const char* source_system = nullptr) {
  InstanceTransformUpdateOptions options{};
  options.reason = reason;
  options.fallback_policy = DefaultTransformFallbackPolicy(reason);
  options.reset_accumulation = true;
  options.coalesce = !IsContinuousTransformMotion(reason);
  options.allow_partial = false;
  options.source_frame = source_frame;
  options.source_system = source_system;
  return options;
}

}  // namespace vkpt::pathtracer
