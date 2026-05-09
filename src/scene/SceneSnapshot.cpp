#include "scene/SceneSnapshot.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "core/metrics/Metrics.h"
#include "scene/SnapshotRing.h"

namespace vkpt::scene {
namespace {

using vkpt::pathtracer::Quat4;
using vkpt::pathtracer::RTHitLight;
using vkpt::pathtracer::RTInstance;
using vkpt::pathtracer::RTMaterial;
using vkpt::pathtracer::RTSdfPrimitive;
using vkpt::pathtracer::RTTessellationRequest;
using vkpt::pathtracer::Vec2;
using vkpt::pathtracer::Vec3;

bool SameVec2(const Vec2& lhs, const Vec2& rhs) {
  return lhs.u == rhs.u && lhs.v == rhs.v;
}

bool SameVec3(const Vec3& lhs, const Vec3& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool SameQuat(const Quat4& lhs, const Quat4& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
}

bool SameMaterial(const RTMaterial& lhs, const RTMaterial& rhs) {
  return SameVec3(lhs.albedo, rhs.albedo) &&
         SameVec3(lhs.emissive, rhs.emissive) &&
         lhs.roughness == rhs.roughness &&
         lhs.metallic == rhs.metallic &&
         lhs.ior == rhs.ior &&
         lhs.transmission == rhs.transmission &&
         lhs.clearcoat == rhs.clearcoat &&
         lhs.sheen == rhs.sheen &&
         lhs.anisotropy == rhs.anisotropy &&
         lhs.alpha == rhs.alpha &&
         lhs.material_model == rhs.material_model &&
         lhs.material_effect == rhs.material_effect &&
         lhs.material_flags == rhs.material_flags &&
         lhs.base_color_texture_index == rhs.base_color_texture_index &&
         lhs.normal_texture_index == rhs.normal_texture_index;
}

bool SameInstance(const RTInstance& lhs, const RTInstance& rhs) {
  return lhs.entity_id == rhs.entity_id &&
         lhs.geometry_id == rhs.geometry_id &&
         lhs.first_triangle == rhs.first_triangle &&
         lhs.triangle_count == rhs.triangle_count &&
         lhs.material_index == rhs.material_index &&
         lhs.flags == rhs.flags &&
         lhs.transform_revision == rhs.transform_revision &&
         lhs.local_first_vertex == rhs.local_first_vertex &&
         lhs.local_vertex_count == rhs.local_vertex_count &&
         lhs.local_first_index == rhs.local_first_index &&
         lhs.local_index_count == rhs.local_index_count &&
         SameVec3(lhs.translation, rhs.translation) &&
         SameQuat(lhs.rotation, rhs.rotation) &&
         SameVec3(lhs.scale, rhs.scale);
}

bool SameTessellationRequest(const RTTessellationRequest& lhs,
                             const RTTessellationRequest& rhs) {
  return lhs.geometry_id == rhs.geometry_id &&
         lhs.first_triangle == rhs.first_triangle &&
         lhs.source_triangle_count == rhs.source_triangle_count &&
         lhs.factor == rhs.factor &&
         lhs.generated_vertex_count == rhs.generated_vertex_count &&
         lhs.generated_index_count == rhs.generated_index_count &&
         lhs.cache_key == rhs.cache_key &&
         SameVec3(lhs.projection_center, rhs.projection_center) &&
         lhs.projection_radius == rhs.projection_radius &&
         lhs.projection_mode == rhs.projection_mode &&
         lhs.gpu_preferred == rhs.gpu_preferred &&
         lhs.cache_generated_geometry == rhs.cache_generated_geometry &&
         lhs.displacement == rhs.displacement;
}

bool SameSdfPrimitive(const RTSdfPrimitive& lhs, const RTSdfPrimitive& rhs) {
  return lhs.shape == rhs.shape &&
         SameVec3(lhs.position, rhs.position) &&
         SameVec3(lhs.scale, rhs.scale) &&
         SameVec3(lhs.rotation, rhs.rotation) &&
         lhs.material_index == rhs.material_index &&
         lhs.radius == rhs.radius &&
         lhs.param_a == rhs.param_a &&
         lhs.param_b == rhs.param_b;
}

bool SameLight(const RTHitLight& lhs, const RTHitLight& rhs) {
  return SameVec3(lhs.position, rhs.position) &&
         SameVec3(lhs.color, rhs.color) &&
         lhs.intensity == rhs.intensity &&
         lhs.radius == rhs.radius &&
         SameVec3(lhs.direction, rhs.direction) &&
         lhs.spot_inner_cos == rhs.spot_inner_cos &&
         lhs.spot_outer_cos == rhs.spot_outer_cos;
}

bool SameTransformState(const RTInstance& lhs, const RTInstance& rhs) {
  return lhs.entity_id == rhs.entity_id &&
         lhs.transform_revision == rhs.transform_revision &&
         SameVec3(lhs.translation, rhs.translation) &&
         SameQuat(lhs.rotation, rhs.rotation) &&
         SameVec3(lhs.scale, rhs.scale);
}

bool SameInstanceMotion(const RenderInstanceMotion& lhs,
                        const RenderInstanceMotion& rhs) {
  return lhs.entity_id == rhs.entity_id &&
         lhs.instance_index == rhs.instance_index &&
         lhs.previous_valid == rhs.previous_valid &&
         SameInstance(lhs.previous, rhs.previous) &&
         SameInstance(lhs.current, rhs.current);
}

template <typename T, typename Equal>
bool SameCowArrayAndVector(const CowArray<T>& lhs,
                           const std::vector<T>& rhs,
                           Equal equal) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  const T* lhsData = lhs.data();
  for (std::size_t i = 0u; i < rhs.size(); ++i) {
    if (!equal(lhsData[i], rhs[i])) {
      return false;
    }
  }
  return true;
}

template <typename T>
std::size_t ApproxBytes(const std::vector<T>& values) {
  return values.size() * sizeof(T);
}

template <>
std::size_t ApproxBytes<std::string>(const std::vector<std::string>& values) {
  std::size_t bytes = values.size() * sizeof(std::string);
  for (const auto& value : values) {
    bytes += value.size();
  }
  return bytes;
}

template <typename T, typename Equal>
CowArray<T> BuildCowArray(const std::vector<T>& source,
                          const CowArray<T>* previous,
                          Equal equal,
                          RenderSceneSnapshotBuildStats& stats) {
  ++stats.cow_total_arrays;
  if (previous != nullptr && SameCowArrayAndVector(*previous, source, equal)) {
    ++stats.cow_reused_arrays;
    return *previous;
  }
  if (source.empty()) {
    return {};
  }

  std::unique_ptr<T[]> storage(new T[source.size()]);
  std::copy(source.begin(), source.end(), storage.get());
  std::shared_ptr<const T[]> shared(storage.release(), std::default_delete<const T[]>{});
  stats.bytes_new += ApproxBytes(source);
  return CowArray<T>{std::move(shared), source.size()};
}

template <typename T>
std::vector<T> ToVector(const CowArray<T>& array) {
  if (array.empty()) {
    return {};
  }
  const T* first = array.data();
  return std::vector<T>(first, first + static_cast<std::ptrdiff_t>(array.size()));
}

vkpt::pathtracer::RTCameraState ExtractCamera(const vkpt::pathtracer::PathTracerSceneSnapshot& scene) {
  vkpt::pathtracer::RTCameraState camera;
  camera.position = scene.camera_position;
  camera.target = scene.camera_target;
  camera.up = scene.camera_up;
  camera.fov_deg = scene.camera_fov_deg;
  camera.focal_length_mm = scene.camera_focal_length_mm;
  camera.sensor_width_mm = scene.camera_sensor_width_mm;
  camera.sensor_height_mm = scene.camera_sensor_height_mm;
  camera.aperture_radius = scene.camera_aperture_radius;
  camera.focus_distance = scene.camera_focus_distance;
  camera.f_stop = scene.camera_f_stop;
  camera.shutter_seconds = scene.camera_shutter_seconds;
  camera.iso = scene.camera_iso;
  camera.exposure_compensation = scene.camera_exposure_compensation;
  camera.white_balance_kelvin = scene.camera_white_balance_kelvin;
  camera.iris_blade_count = scene.camera_iris_blade_count;
  camera.iris_rotation_degrees = scene.camera_iris_rotation_degrees;
  camera.iris_roundness = scene.camera_iris_roundness;
  camera.anamorphic_squeeze = scene.camera_anamorphic_squeeze;
  return camera;
}

void ApplyCamera(vkpt::pathtracer::PathTracerSceneSnapshot& scene,
                 const vkpt::pathtracer::RTCameraState& camera) {
  scene.camera_position = camera.position;
  scene.camera_target = camera.target;
  scene.camera_up = camera.up;
  scene.camera_fov_deg = camera.fov_deg;
  scene.camera_focal_length_mm = camera.focal_length_mm;
  scene.camera_sensor_width_mm = camera.sensor_width_mm;
  scene.camera_sensor_height_mm = camera.sensor_height_mm;
  scene.camera_aperture_radius = camera.aperture_radius;
  scene.camera_focus_distance = camera.focus_distance;
  scene.camera_f_stop = camera.f_stop;
  scene.camera_shutter_seconds = camera.shutter_seconds;
  scene.camera_iso = camera.iso;
  scene.camera_exposure_compensation = camera.exposure_compensation;
  scene.camera_white_balance_kelvin = camera.white_balance_kelvin;
  scene.camera_iris_blade_count = camera.iris_blade_count;
  scene.camera_iris_rotation_degrees = camera.iris_rotation_degrees;
  scene.camera_iris_roundness = camera.iris_roundness;
  scene.camera_anamorphic_squeeze = camera.anamorphic_squeeze;
}

const RTInstance* FindPreviousInstance(std::span<const RTInstance> previous,
                                       std::size_t current_index,
                                       const RTInstance& current) {
  if (current_index < previous.size() &&
      previous[current_index].entity_id == current.entity_id) {
    return &previous[current_index];
  }
  const auto it = std::find_if(previous.begin(),
                               previous.end(),
                               [&](const RTInstance& candidate) {
                                 return candidate.entity_id == current.entity_id;
                               });
  return it != previous.end() ? &*it : nullptr;
}

std::vector<RenderInstanceMotion> BuildInstanceMotion(
    const RenderSceneSnapshot* previous,
    const std::vector<RTInstance>& current) {
  std::vector<RenderInstanceMotion> motion;
  if (previous == nullptr || current.empty()) {
    return motion;
  }

  const auto previousInstances = previous->instances.view();
  motion.reserve(current.size());
  for (std::size_t index = 0u; index < current.size(); ++index) {
    const auto& instance = current[index];
    const RTInstance* previousInstance =
        FindPreviousInstance(previousInstances, index, instance);
    if (previousInstance == nullptr ||
        SameTransformState(*previousInstance, instance)) {
      continue;
    }

    RenderInstanceMotion pair;
    pair.entity_id = instance.entity_id;
    pair.instance_index = static_cast<std::uint32_t>(
        std::min<std::size_t>(index, std::numeric_limits<std::uint32_t>::max()));
    pair.previous_valid = true;
    pair.previous = *previousInstance;
    pair.current = instance;
    motion.push_back(pair);
  }
  return motion;
}

std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> BuildRefitUpdatesFromMotion(
    const RenderSceneSnapshot& snapshot) {
  std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
  const auto motion = snapshot.instance_motion.view();
  updates.reserve(motion.size());
  for (const auto& pair : motion) {
    vkpt::pathtracer::RTInstanceTransformUpdate update;
    update.entity_id = pair.entity_id;
    update.instance_index = pair.instance_index;
    update.flags = pair.current.flags | vkpt::pathtracer::kRTInstanceFlagTransformDirty;
    update.transform_revision = pair.current.transform_revision;
    update.translation = pair.current.translation;
    update.rotation = pair.current.rotation;
    update.scale = pair.current.scale;
    updates.push_back(update);
  }
  return updates;
}

std::shared_ptr<const vkpt::pathtracer::IRayAccelerator> FreezeAccelerator(
    std::unique_ptr<vkpt::pathtracer::IRayAccelerator> accelerator) {
  if (!accelerator) {
    return {};
  }
  std::shared_ptr<vkpt::pathtracer::IRayAccelerator> shared(std::move(accelerator));
  return shared;
}

SnapshotAccelerationHandle BuildSnapshotAcceleration(
    const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
    const RenderSceneSnapshot* previous,
    const RenderSceneSnapshot& current,
    RenderSceneSnapshotBuildStats& stats) {
  const auto start = std::chrono::steady_clock::now();
  SnapshotAccelerationHandle handle;
  const auto refitUpdates = BuildRefitUpdatesFromMotion(current);
  const bool canBuildViaRefit =
      previous != nullptr &&
      !refitUpdates.empty() &&
      current.geometry_storage_reused_from(*previous);
  const bool canReusePrevious =
      previous != nullptr &&
      previous->acceleration.valid() &&
      current.geometry_storage_reused_from(*previous) &&
      current.instances.shares_storage_with(previous->instances);
  if (canReusePrevious) {
    handle.cpu_bvh = previous->acceleration.cpu_bvh;
    handle.cpu_bvh_info = previous->acceleration.cpu_bvh_info;
    handle.reused_from_previous = true;
    stats.acceleration_built = handle.valid();
    stats.acceleration_reused = true;
    const auto end = std::chrono::steady_clock::now();
    stats.acceleration_build_us =
        std::chrono::duration<double, std::micro>(end - start).count();
    return handle;
  }

  constexpr std::size_t kSnapshotCpuBvhRefitTriangleBudget = 100'000u;
  const bool largeTopologyStableRefit =
      canBuildViaRefit &&
      scene.indices.size() / 3u > kSnapshotCpuBvhRefitTriangleBudget;
  if (largeTopologyStableRefit) {
    handle.cpu_bvh_info = previous->acceleration.cpu_bvh_info;
    handle.refit_updates = refitUpdates;
    handle.transform_refit_descriptor = true;
    stats.acceleration_refit_descriptor = true;
    const auto end = std::chrono::steady_clock::now();
    stats.acceleration_build_us =
        std::chrono::duration<double, std::micro>(end - start).count();
    return handle;
  }

  auto accelerator = vkpt::pathtracer::CreateCpuBvhAccelerator();
  if (!accelerator) {
    return handle;
  }

  bool built = false;
  if (canBuildViaRefit) {
    auto previousScene = previous->path_tracer_scene_snapshot();
    built = accelerator->build(previousScene, true);
    if (built) {
      vkpt::pathtracer::InstanceTransformUpdateOptions options;
      options.reason = vkpt::pathtracer::RenderUpdateReason::PhysicsMotion;
      options.fallback_policy = vkpt::pathtracer::TransformFallbackPolicy::AllowDynamicAcceleration;
      options.source_system = "snapshot";
      const auto result =
          accelerator->apply_instance_transform_update(previousScene, refitUpdates, options);
      if (result.applied()) {
        handle.refit_updates = refitUpdates;
        handle.transform_refit_descriptor = true;
      } else {
        accelerator = vkpt::pathtracer::CreateCpuBvhAccelerator();
        built = accelerator && accelerator->build(scene, true);
      }
    }
  } else {
    built = accelerator->build(scene, true);
  }

  if (built) {
    handle.cpu_bvh_info = accelerator->build_info();
    handle.cpu_bvh = FreezeAccelerator(std::move(accelerator));
  }
  stats.acceleration_built = handle.valid();
  stats.acceleration_refit_descriptor = handle.transform_refit_descriptor;
  const auto end = std::chrono::steady_clock::now();
  stats.acceleration_build_us =
      std::chrono::duration<double, std::micro>(end - start).count();
  return handle;
}

std::uint64_t CountTransformDirtyInstances(
    const vkpt::pathtracer::PathTracerSceneSnapshot& scene) {
  std::uint64_t count = 0u;
  for (const auto& instance : scene.instances) {
    if ((instance.flags & vkpt::pathtracer::kRTInstanceFlagTransformDirty) != 0u) {
      ++count;
    }
  }
  return count;
}

void RecordSceneTickMetrics(const vkpt::pathtracer::PathTracerSceneSnapshot& scene) {
  VKP_METRIC_SET("vkp.scene.entity_count", scene.instances.size());
  VKP_METRIC_SET("vkp.scene.transform_dirty_count",
                 CountTransformDirtyInstances(scene));
}

}  // namespace

std::uint64_t SnapshotWallTimeNowNs() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

bool RenderSceneSnapshot::geometry_storage_reused_from(
    const RenderSceneSnapshot& other) const {
  return vertices.shares_storage_with(other.vertices) &&
         texcoords.shares_storage_with(other.texcoords) &&
         indices.shares_storage_with(other.indices) &&
         local_vertices.shares_storage_with(other.local_vertices) &&
         local_indices.shares_storage_with(other.local_indices) &&
         tessellation_requests.shares_storage_with(other.tessellation_requests) &&
         sdf_primitives.shares_storage_with(other.sdf_primitives) &&
         environment_map.shares_storage_with(other.environment_map);
}

const vkpt::pathtracer::PathTracerSceneSnapshot&
RenderSceneSnapshot::path_tracer_scene_snapshot() const {
  static const vkpt::pathtracer::PathTracerSceneSnapshot kEmptyScene;
  if (auto cached = path_tracer_scene.load(std::memory_order_acquire)) {
    return *cached;
  }
  if (vertices.empty() && indices.empty() && instances.empty() &&
      materials.empty() && lights.empty()) {
    return kEmptyScene;
  }

  auto scene = std::make_shared<vkpt::pathtracer::PathTracerSceneSnapshot>();
  scene->vertices = ToVector(vertices);
  scene->texcoords = ToVector(texcoords);
  scene->indices = ToVector(indices);
  scene->local_vertices = ToVector(local_vertices);
  scene->local_indices = ToVector(local_indices);
  scene->instances = ToVector(instances);
  scene->tessellation_requests = ToVector(tessellation_requests);
  scene->sdf_primitives = ToVector(sdf_primitives);
  scene->materials = ToVector(materials);
  scene->textures = ToVector(textures);
  scene->lights = ToVector(lights);
  scene->environment_color = environment_color;
  scene->environment_map = ToVector(environment_map);
  scene->environment_map_scale = environment_map_scale;
  scene->environment_map_width = environment_map_width;
  scene->environment_map_height = environment_map_height;
  ApplyCamera(*scene, camera);

  // CAS so concurrent first readers settle on a single shared_ptr. With a plain
  // store, the loser's local was the only owner of its build; on function return
  // the local destructed and the returned const& dangled, crashing whoever held it
  // (UI picking thread + render thread racing on first ADS frame).
  std::shared_ptr<const vkpt::pathtracer::PathTracerSceneSnapshot> expected{};
  if (path_tracer_scene.compare_exchange_strong(
          expected,
          std::shared_ptr<const vkpt::pathtracer::PathTracerSceneSnapshot>(scene),
          std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return *scene;
  }
  return *expected;
}

const char* ToString(SnapshotTransitionAction action) {
  switch (action) {
    case SnapshotTransitionAction::Continue:
      return "continue";
    case SnapshotTransitionAction::ResetAccumulation:
      return "reset";
    case SnapshotTransitionAction::ReprojectCamera:
      return "reproject";
    case SnapshotTransitionAction::ResetMovingPixels:
      return "per_pixel";
    case SnapshotTransitionAction::InvalidateShading:
      return "reshade";
  }
  return "reset";
}

std::string SnapshotChangeReason(RenderSceneSnapshotChange changes) {
  if (changes == RenderSceneSnapshotChange::None) {
    return "none";
  }
  std::string out;
  const auto append = [&](const char* value) {
    if (!out.empty()) {
      out.push_back('+');
    }
    out.append(value);
  };
  if (HasChange(changes, RenderSceneSnapshotChange::Topology)) {
    append("topology");
  }
  if (HasChange(changes, RenderSceneSnapshotChange::Transform)) {
    append("transform");
  }
  if (HasChange(changes, RenderSceneSnapshotChange::Camera)) {
    append("camera");
  }
  if (HasChange(changes, RenderSceneSnapshotChange::Material)) {
    append("material");
  }
  return out;
}

RenderSceneSnapshot::Ptr BuildRenderSceneSnapshot(
    const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
    const RenderSceneSnapshot* previous,
    RenderSceneSnapshotRevisions revisions,
    RenderSceneSnapshotBuildStats* stats) {
  const auto start = std::chrono::steady_clock::now();
  RenderSceneSnapshotBuildStats localStats;
  auto out = std::make_shared<RenderSceneSnapshot>();
  if (revisions.wall_time_ns == 0u) {
    revisions.wall_time_ns = SnapshotWallTimeNowNs();
  }

  out->generation = revisions.generation;
  out->topology_revision = revisions.topology_revision;
  out->transform_revision = revisions.transform_revision;
  out->camera_revision = revisions.camera_revision;
  out->material_revision = revisions.material_revision;
  out->wall_time_ns = revisions.wall_time_ns;

  const bool topologySame =
      previous != nullptr &&
      revisions.topology_revision == previous->topology_revision;
  const bool transformSame =
      previous != nullptr &&
      revisions.transform_revision == previous->transform_revision;
  const bool materialSame =
      previous != nullptr &&
      revisions.material_revision == previous->material_revision;
  const bool cameraSame =
      previous != nullptr &&
      revisions.camera_revision == previous->camera_revision;
  if (topologySame) {
    auto reuseArray = [&](const auto& array) {
      ++localStats.cow_total_arrays;
      ++localStats.cow_reused_arrays;
      return array;
    };

    out->vertices = reuseArray(previous->vertices);
    out->texcoords = reuseArray(previous->texcoords);
    out->indices = reuseArray(previous->indices);
    out->local_vertices = reuseArray(previous->local_vertices);
    out->local_indices = reuseArray(previous->local_indices);
    out->tessellation_requests = reuseArray(previous->tessellation_requests);
    out->sdf_primitives = reuseArray(previous->sdf_primitives);

    // S6: when materialSame, all material-dependent arrays reuse previous
    // storage in a single block (clarity refactor).
    if (materialSame) {
      out->environment_map = reuseArray(previous->environment_map);
      out->materials = reuseArray(previous->materials);
      out->textures = reuseArray(previous->textures);
      out->lights = reuseArray(previous->lights);
    } else {
      out->environment_map = BuildCowArray(scene.environment_map,
                                           &previous->environment_map,
                                           SameVec3,
                                           localStats);
      out->materials = BuildCowArray(scene.materials,
                                     &previous->materials,
                                     SameMaterial,
                                     localStats);
      out->textures = BuildCowArray(scene.textures,
                                    &previous->textures,
                                    [](const std::string& lhs, const std::string& rhs) {
                                      return lhs == rhs;
                                    },
                                    localStats);
      out->lights = BuildCowArray(scene.lights,
                                  &previous->lights,
                                  SameLight,
                                  localStats);
    }

    out->instances = (transformSame && materialSame)
        ? reuseArray(previous->instances)
        : BuildCowArray(scene.instances,
                        &previous->instances,
                        SameInstance,
                        localStats);
    if (transformSame) {
      // S3 trivial case: topologySame && transformSame implies no per-instance
      // motion is possible; short-circuit to an empty buffer.
      ++localStats.cow_total_arrays;
      if (previous->instance_motion.empty()) {
        ++localStats.cow_reused_arrays;
      }
      out->instance_motion = {};
    } else if (out->instances.shares_storage_with(previous->instances)) {
      // S3: BuildCowArray reused previous storage (every instance compared
      // equal), so by construction no transform changed and motion is empty.
      // Skip BuildInstanceMotion's O(n) walk.
      ++localStats.cow_total_arrays;
      if (previous->instance_motion.empty()) {
        ++localStats.cow_reused_arrays;
      }
      out->instance_motion = {};
    } else {
      const auto instanceMotion = BuildInstanceMotion(previous, scene.instances);
      out->instance_motion = BuildCowArray(instanceMotion,
                                           &previous->instance_motion,
                                           SameInstanceMotion,
                                           localStats);
    }

    out->environment_color = scene.environment_color;
    out->environment_map_scale = scene.environment_map_scale;
    out->environment_map_width = scene.environment_map_width;
    out->environment_map_height = scene.environment_map_height;
    out->camera = ExtractCamera(scene);
    if (materialSame && cameraSame) {
      out->path_tracer_scene.store(
          previous->path_tracer_scene.load(std::memory_order_acquire),
          std::memory_order_release);
    }
    if (transformSame) {
      out->acceleration = previous->acceleration;
      out->acceleration.reused_from_previous = out->acceleration.valid();
      out->acceleration.transform_refit_descriptor = false;
      out->acceleration.refit_updates.clear();
      localStats.acceleration_built = out->acceleration.valid();
      localStats.acceleration_reused = out->acceleration.valid();
    } else {
      out->acceleration = BuildSnapshotAcceleration(scene, previous, *out, localStats);
    }

    const auto end = std::chrono::steady_clock::now();
    localStats.build_us = std::chrono::duration<double, std::micro>(end - start).count();
    out->build_stats = localStats;
    if (stats != nullptr) {
      *stats = localStats;
    }
    return out;
  }

  out->vertices = BuildCowArray(scene.vertices,
                                previous ? &previous->vertices : nullptr,
                                SameVec3,
                                localStats);
  out->texcoords = BuildCowArray(scene.texcoords,
                                 previous ? &previous->texcoords : nullptr,
                                 SameVec2,
                                 localStats);
  out->indices = BuildCowArray(scene.indices,
                               previous ? &previous->indices : nullptr,
                               [](std::uint32_t lhs, std::uint32_t rhs) { return lhs == rhs; },
                               localStats);
  out->local_vertices = BuildCowArray(scene.local_vertices,
                                      previous ? &previous->local_vertices : nullptr,
                                      SameVec3,
                                      localStats);
  out->local_indices = BuildCowArray(scene.local_indices,
                                     previous ? &previous->local_indices : nullptr,
                                     [](std::uint32_t lhs, std::uint32_t rhs) { return lhs == rhs; },
                                     localStats);
  out->instances = BuildCowArray(scene.instances,
                                 previous ? &previous->instances : nullptr,
                                 SameInstance,
                                 localStats);
  out->tessellation_requests = BuildCowArray(scene.tessellation_requests,
                                             previous ? &previous->tessellation_requests : nullptr,
                                             SameTessellationRequest,
                                             localStats);
  out->sdf_primitives = BuildCowArray(scene.sdf_primitives,
                                      previous ? &previous->sdf_primitives : nullptr,
                                      SameSdfPrimitive,
                                      localStats);
  out->materials = BuildCowArray(scene.materials,
                                 previous ? &previous->materials : nullptr,
                                 SameMaterial,
                                 localStats);
  out->textures = BuildCowArray(scene.textures,
                                previous ? &previous->textures : nullptr,
                                [](const std::string& lhs, const std::string& rhs) {
                                  return lhs == rhs;
                                },
                                localStats);
  out->lights = BuildCowArray(scene.lights,
                              previous ? &previous->lights : nullptr,
                              SameLight,
                              localStats);
  out->environment_map = BuildCowArray(scene.environment_map,
                                       previous ? &previous->environment_map : nullptr,
                                       SameVec3,
                                       localStats);
  const auto instanceMotion = BuildInstanceMotion(previous, scene.instances);
  out->instance_motion = BuildCowArray(instanceMotion,
                                       previous ? &previous->instance_motion : nullptr,
                                       SameInstanceMotion,
                                       localStats);

  out->environment_color = scene.environment_color;
  out->environment_map_scale = scene.environment_map_scale;
  out->environment_map_width = scene.environment_map_width;
  out->environment_map_height = scene.environment_map_height;
  out->camera = ExtractCamera(scene);
  out->path_tracer_scene.store(
      std::make_shared<vkpt::pathtracer::PathTracerSceneSnapshot>(scene),
      std::memory_order_release);
  out->acceleration = BuildSnapshotAcceleration(scene, previous, *out, localStats);

  const auto end = std::chrono::steady_clock::now();
  localStats.build_us = std::chrono::duration<double, std::micro>(end - start).count();
  out->build_stats = localStats;
  if (stats != nullptr) {
    *stats = localStats;
  }
  return out;
}

RenderSceneSnapshotChange CompareRenderSceneSnapshots(
    const RenderSceneSnapshot* previous,
    const RenderSceneSnapshot& current) {
  if (previous == nullptr) {
    return RenderSceneSnapshotChange::Topology |
           RenderSceneSnapshotChange::Transform |
           RenderSceneSnapshotChange::Camera |
           RenderSceneSnapshotChange::Material;
  }

  RenderSceneSnapshotChange change = RenderSceneSnapshotChange::None;
  if (previous->topology_revision != current.topology_revision) {
    change |= RenderSceneSnapshotChange::Topology;
  }
  if (previous->transform_revision != current.transform_revision) {
    change |= RenderSceneSnapshotChange::Transform;
  }
  if (previous->camera_revision != current.camera_revision) {
    change |= RenderSceneSnapshotChange::Camera;
  }
  if (previous->material_revision != current.material_revision) {
    change |= RenderSceneSnapshotChange::Material;
  }
  return change;
}

SnapshotTransitionDecision DecideSnapshotTransition(
    const RenderSceneSnapshot* previous,
    const RenderSceneSnapshot& current,
    SnapshotTransitionCapabilities capabilities) {
  SnapshotTransitionDecision decision;
  decision.changes = CompareRenderSceneSnapshots(previous, current);
  if (decision.changes == RenderSceneSnapshotChange::None) {
    return decision;
  }

  decision.rebuild_tile_schedule = true;

  if (HasChange(decision.changes, RenderSceneSnapshotChange::Topology)) {
    decision.action = SnapshotTransitionAction::ResetAccumulation;
    decision.reset_accumulation = true;
    decision.reason = "topology";
    return decision;
  }

  const bool transformChanged =
      HasChange(decision.changes, RenderSceneSnapshotChange::Transform);
  const bool cameraChanged =
      HasChange(decision.changes, RenderSceneSnapshotChange::Camera);
  const bool materialChanged =
      HasChange(decision.changes, RenderSceneSnapshotChange::Material);

  if (cameraChanged && !transformChanged && !materialChanged &&
      capabilities.camera_reprojection) {
    decision.action = SnapshotTransitionAction::ReprojectCamera;
    decision.reason = "camera";
    return decision;
  }

  if (transformChanged && !cameraChanged && !materialChanged &&
      capabilities.transform_motion_vectors &&
      !current.instance_motion.empty()) {
    decision.action = SnapshotTransitionAction::ResetMovingPixels;
    decision.reason = "transform";
    return decision;
  }

  if (materialChanged && !transformChanged && !cameraChanged &&
      capabilities.material_reshade) {
    decision.action = SnapshotTransitionAction::InvalidateShading;
    decision.reason = "material";
    return decision;
  }

  decision.action = SnapshotTransitionAction::ResetAccumulation;
  decision.reset_accumulation = true;
  decision.reason = transformChanged
      ? "transform"
      : (cameraChanged ? "camera" : "material");
  return decision;
}

vkpt::core::Status SnapshotRing::validate_sim_tick(
    const SimSnapshotTickRequest& request) const {
  if (request.snapshots != nullptr && request.snapshots != this) {
    return vkpt::core::Status::error(
        vkpt::core::StatusCode::InvalidArgument,
        "sim tick request targets a different SnapshotRing");
  }
  if (request.scene == nullptr) {
    return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument,
                                     "sim tick request has no scene");
  }
  if (request.validate_writes) {
    return request.validate_writes(*request.scene);
  }
  return vkpt::core::Status::ok();
}

SnapshotRing::SnapshotPtr SnapshotRing::apply_sim_tick(
    SimSnapshotTickRequest request,
    const RenderSceneSnapshot* previous,
    RenderSceneSnapshotBuildStats* stats) const {
  if (request.apply_writes) {
    request.apply_writes(*request.scene);
  }
  RecordSceneTickMetrics(*request.scene);
  return BuildRenderSceneSnapshot(*request.scene,
                                  previous,
                                  request.revisions,
                                  stats);
}

SimSnapshotTickResult SnapshotRing::publish_sim_tick(
    SimSnapshotTickRequest request) {
  SimSnapshotTickResult result;
  result.status = validate_sim_tick(request);
  if (result.status.is_error()) {
    return result;
  }

  result.previous = current();
  result.snapshot = apply_sim_tick(request, result.previous.get(), &result.build_stats);
  if (!result.snapshot) {
    result.status = vkpt::core::Status::error(
        vkpt::core::StatusCode::InternalError,
        "sim tick snapshot build failed");
    return result;
  }

  result.transition = DecideSnapshotTransition(result.previous.get(),
                                               *result.snapshot,
                                               request.transition_capabilities);
  publish(result.snapshot);
  result.published = true;
  return result;
}

SimSnapshotTickResult PublishSimTickSnapshot(SimSnapshotTickRequest request) {
  SimSnapshotTickResult result;
  if (request.snapshots == nullptr) {
    result.status = vkpt::core::Status::error(
        vkpt::core::StatusCode::InvalidArgument,
        "sim tick request has no SnapshotRing");
    return result;
  }
  return request.snapshots->publish_sim_tick(std::move(request));
}

std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> DiffInstanceTransforms(
    const RenderSceneSnapshot& previous,
    const RenderSceneSnapshot& current) {
  std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
  const auto prevInstances = previous.instances.view();
  const auto currInstances = current.instances.view();
  updates.reserve(currInstances.size());

  for (std::size_t index = 0u; index < currInstances.size(); ++index) {
    const auto& instance = currInstances[index];
    const RTInstance* previousInstance =
        FindPreviousInstance(prevInstances, index, instance);

    if (previousInstance != nullptr && SameTransformState(*previousInstance, instance)) {
      continue;
    }

    vkpt::pathtracer::RTInstanceTransformUpdate update;
    update.entity_id = instance.entity_id;
    update.instance_index = static_cast<std::uint32_t>(
        std::min<std::size_t>(index, std::numeric_limits<std::uint32_t>::max()));
    update.flags = instance.flags | vkpt::pathtracer::kRTInstanceFlagTransformDirty;
    update.transform_revision = instance.transform_revision;
    update.translation = instance.translation;
    update.rotation = instance.rotation;
    update.scale = instance.scale;
    updates.push_back(update);
  }

  return updates;
}

}  // namespace vkpt::scene
