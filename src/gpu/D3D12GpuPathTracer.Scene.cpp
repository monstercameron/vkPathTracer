#ifdef PT_ENABLE_D3D12

#include "gpu/D3D12GpuPathTracerInternal.h"
#include "gpu/D3D12GpuPathTracer.SceneBvh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

namespace vkpt::gpu {

namespace {

constexpr uint32_t kGpuMaterialStrideFloats = 16u;
constexpr uint32_t kGpuLightStrideFloats = 16u;

std::array<float, kGpuMaterialStrideFloats> PackedMaterialValues(
    const vkpt::pathtracer::RTMaterial& material) {
  // The final float carries bit-packed material options consumed by HLSL as an
  // integer payload after float-to-uint conversion.
  uint32_t packedEffect = (material.material_effect & 1023u) |
                          ((material.material_flags & 1u) ? 1024u : 0u);
  if (material.base_color_texture_index != kInvalidTextureIndex &&
      material.base_color_texture_index < 8191u) {
    packedEffect |= ((material.base_color_texture_index + 1u) << 11u);
  }
  return {
      material.albedo.x,
      material.albedo.y,
      material.albedo.z,
      material.emissive.x,
      material.emissive.y,
      material.emissive.z,
      material.roughness,
      static_cast<float>(material.material_model),
      material.metallic,
      material.ior,
      material.transmission,
      material.clearcoat,
      material.sheen,
      material.normal_texture_index != kInvalidTextureIndex
          ? static_cast<float>(material.normal_texture_index + 1u)
          : 0.0f,
      material.alpha,
      static_cast<float>(packedEffect),
  };
}

void PackMaterial(const vkpt::pathtracer::RTMaterial& material,
                  std::vector<float>& out) {
  const auto packed = PackedMaterialValues(material);
  out.insert(out.end(), packed.begin(), packed.end());
}

bool StorePackedMaterialAt(std::vector<float>& gpuMaterials,
                           uint32_t materialIndex,
                           const vkpt::pathtracer::RTMaterial& material) {
  const std::size_t base = static_cast<std::size_t>(materialIndex) * kGpuMaterialStrideFloats;
  if (base + kGpuMaterialStrideFloats > gpuMaterials.size()) {
    return false;
  }
  const auto packed = PackedMaterialValues(material);
  std::copy(packed.begin(), packed.end(), gpuMaterials.begin() + static_cast<std::ptrdiff_t>(base));
  return true;
}

std::array<float, kGpuLightStrideFloats> PackedLightValues(
    const vkpt::pathtracer::RTHitLight& light) {
  return {
      light.position.x,
      light.position.y,
      light.position.z,
      light.color.x,
      light.color.y,
      light.color.z,
      light.intensity,
      std::max(0.0f, light.radius),
      light.direction.x,
      light.direction.y,
      light.direction.z,
      light.spot_inner_cos,
      light.spot_outer_cos,
      0.0f,
      0.0f,
      0.0f,
  };
}

void PackLight(const vkpt::pathtracer::RTHitLight& light,
               std::vector<float>& out) {
  const auto packed = PackedLightValues(light);
  out.insert(out.end(), packed.begin(), packed.end());
}

bool StorePackedLightAt(std::vector<float>& gpuLights,
                        uint32_t lightIndex,
                        const vkpt::pathtracer::RTHitLight& light) {
  const std::size_t base = static_cast<std::size_t>(lightIndex) * kGpuLightStrideFloats;
  if (base + kGpuLightStrideFloats > gpuLights.size()) {
    return false;
  }
  const auto packed = PackedLightValues(light);
  std::copy(packed.begin(), packed.end(), gpuLights.begin() + static_cast<std::ptrdiff_t>(base));
  return true;
}

bool MaterialDeltaRequiresFullRebuild(const vkpt::pathtracer::RTMaterial& before,
                                      const vkpt::pathtracer::RTMaterial& after) {
  // Double-sided state is baked into packed triangle data for the current
  // D3D12 path, so changing it needs the full triangle buffer repack.
  return before.material_flags != after.material_flags;
}

}  // namespace

bool D3D12GpuPathTracer::load_scene_snapshot(
    const vkpt::pathtracer::RTSceneData& scene) {
  if (!m_configured) {
    m_error = "load_scene_snapshot before configure";
    LogError("load_scene_snapshot rejected: " + m_error);
    return false;
  }
  m_sceneData      = scene;
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData));
  m_hasScene       = true;
  m_sceneUploaded  = false;
  m_temporalHistoryValid = false;
  std::ostringstream ss;
  ss << "scene snapshot loaded verts=" << scene.vertices.size()
     << " idx=" << scene.indices.size()
     << " mats=" << scene.materials.size()
     << " inst=" << scene.instances.size()
     << " tess=" << scene.tessellation_requests.size()
     << " lights=" << scene.lights.size();
  LogDebug(ss.str());
  return true;
}

bool D3D12GpuPathTracer::update_camera(
    const vkpt::pathtracer::Vec3& pos,
    const vkpt::pathtracer::Vec3& target,
    const vkpt::pathtracer::Vec3& up,
    float fov_deg) {
  if (!m_sceneUploaded) {
    // Geometry hasn't been uploaded yet — can't update camera independently.
    return false;
  }
  auto camera = vkpt::pathtracer::ExtractCameraState(m_sceneData);
  camera.position = pos;
  camera.target = target;
  camera.up = up;
  camera.fov_deg = fov_deg;
  const bool updated = update_camera_state(camera);
  std::ostringstream ss;
  ss << "update_camera pos=(" << pos.x << "," << pos.y << "," << pos.z
     << ") target=(" << target.x << "," << target.y << "," << target.z << ")";
  LogDebug(ss.str());
  return updated;
}

bool D3D12GpuPathTracer::update_camera_state(
    const vkpt::pathtracer::RTCameraState& camera) {
  if (!m_sceneUploaded) {
    return false;
  }
  vkpt::pathtracer::ApplyCameraState(m_sceneData, camera);
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData));
  m_temporalHistoryValid = false;
  std::ostringstream ss;
  ss << "update_camera_state pos=(" << camera.position.x << "," << camera.position.y
     << "," << camera.position.z << ") target=(" << camera.target.x << ","
     << camera.target.y << "," << camera.target.z << ")";
  LogDebug(ss.str());
  return true;
}

bool D3D12GpuPathTracer::update_instance_transforms(
    const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) {
  if (!m_sceneUploaded || !m_instBuf || updates.empty()) {
    return false;
  }
  if (!m_dynamicInstanceTransformsEnabled) {
    return false;
  }
  if (m_gpuInsts.size() < m_sceneData.instances.size() * kGpuInstanceStrideU32) {
    return false;
  }
  const bool updateDxrTlas = (m_preferDxr || m_usingDxrDispatch);
  if (updateDxrTlas && (!m_dxrAccelReady || m_dxrInstanceDescs.empty())) {
    return false;
  }

  auto findInstanceIndex = [&](const vkpt::pathtracer::RTInstanceTransformUpdate& update) {
    if (update.instance_index < m_sceneData.instances.size()) {
      return update.instance_index;
    }
    if (update.entity_id != 0u) {
      for (std::size_t index = 0; index < m_sceneData.instances.size(); ++index) {
        if (m_sceneData.instances[index].entity_id == update.entity_id) {
          return static_cast<uint32_t>(index);
        }
      }
    }
    return vkpt::pathtracer::kInvalidRTInstanceIndex;
  };

  bool changed = false;
  for (const auto& update : updates) {
    const uint32_t instanceIndex = findInstanceIndex(update);
    if (instanceIndex >= m_sceneData.instances.size()) {
      continue;
    }
    auto& instance = m_sceneData.instances[instanceIndex];
    if (!instance.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform)) {
      continue;
    }
    instance.translation = update.translation;
    instance.rotation = update.rotation;
    instance.scale = update.scale;
    instance.flags |= update.flags;
    instance.transform_revision = update.transform_revision;

    const std::size_t base = static_cast<std::size_t>(instanceIndex) * kGpuInstanceStrideU32;
    m_gpuInsts[base + 3u] = instance.flags;
    m_gpuInsts[base + 4u] = FloatBits(instance.translation.x);
    m_gpuInsts[base + 5u] = FloatBits(instance.translation.y);
    m_gpuInsts[base + 6u] = FloatBits(instance.translation.z);
    m_gpuInsts[base + 8u] = FloatBits(instance.rotation.x);
    m_gpuInsts[base + 9u] = FloatBits(instance.rotation.y);
    m_gpuInsts[base + 10u] = FloatBits(instance.rotation.z);
    m_gpuInsts[base + 11u] = FloatBits(instance.rotation.w);
    m_gpuInsts[base + 12u] = FloatBits(instance.scale.x);
    m_gpuInsts[base + 13u] = FloatBits(instance.scale.y);
    m_gpuInsts[base + 14u] = FloatBits(instance.scale.z);
    changed = true;
  }

  if (!changed) {
    return false;
  }
  m_dynamicInstanceCount = BuildDynamicInstanceBvhFromPackedInstances(
      m_gpuInsts,
      static_cast<uint32_t>(m_sceneData.instances.size()),
      m_gpuDynamicBvh);
  const bool uploaded = updateDxrTlas ? update_dxr_instance_buffer_and_tlas() : upload_instance_buffer();
  if (uploaded) {
    m_temporalHistoryValid = false;
  }
  return uploaded;
}

bool D3D12GpuPathTracer::update_scene_delta(
    const vkpt::pathtracer::RTSceneDeltaUpdate& update) {
  if (!m_sceneUploaded) {
    return false;
  }
  const bool uploadMaterials = !update.materials.empty();
  const bool uploadLights = !update.lights.empty();
  if (uploadMaterials && !m_matBuf) {
    return false;
  }
  if (uploadLights && !m_ltBuf) {
    return false;
  }

  for (const auto& material : update.materials) {
    if (material.material_index >= m_sceneData.materials.size()) {
      return false;
    }
    if (MaterialDeltaRequiresFullRebuild(m_sceneData.materials[material.material_index],
                                         material.material)) {
      return false;
    }
  }
  for (const auto& light : update.lights) {
    if (light.light_index >= m_sceneData.lights.size()) {
      return false;
    }
  }

  // Validate and pack into CPU mirrors before touching GPU resources so a
  // failed delta leaves both scene state and buffers on the previous version.
  auto nextScene = m_sceneData;
  if (!vkpt::pathtracer::ApplySceneDeltaUpdate(nextScene, update)) {
    return false;
  }
  for (const auto& material : update.materials) {
    if (!StorePackedMaterialAt(m_gpuMats, material.material_index, material.material)) {
      return false;
    }
  }
  for (const auto& light : update.lights) {
    if (!StorePackedLightAt(m_gpuLights, light.light_index, light.light)) {
      return false;
    }
  }

  const bool uploaded = upload_material_light_buffers(uploadMaterials, uploadLights);
  if (!uploaded) {
    return false;
  }
  m_sceneData = std::move(nextScene);
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData));
  m_temporalHistoryValid = false;
  std::ostringstream ss;
  ss << "scene delta uploaded mats=" << update.materials.size()
     << " lights=" << update.lights.size()
     << " environment=" << (update.environment_color_changed ? "true" : "false");
  LogDebug(ss.str());
  return true;
}

bool D3D12GpuPathTracer::build_or_update_acceleration() {
  if (!m_hasScene) {
    m_error = "build_or_update_acceleration before snapshot";
    LogError("build_or_update_acceleration rejected: " + m_error);
    return false;
  }
  m_gpuMats.clear();
  m_gpuMats.reserve(std::max<std::size_t>(16u, m_sceneData.materials.size() * 16u));
  for (const auto& m : m_sceneData.materials) {
    PackMaterial(m, m_gpuMats);
  }
  if (m_gpuMats.empty()) m_gpuMats.assign(16, 0.0f);

  // CPU scene data is normalized into flat SoA buffers because the compute
  // shaders consume raw SRV buffers rather than structured C++ scene objects.
  struct PackedInstance {
    uint32_t first_triangle = 0u;
    uint32_t triangle_count = 0u;
    uint32_t material_index = 0u;
    uint32_t flags = 0u;
    uint32_t local_bvh_first_node = 0u;
    uint32_t local_bvh_node_count = 0u;
    vkpt::pathtracer::Vec3 translation{};
    vkpt::pathtracer::Quat4 rotation{};
    vkpt::pathtracer::Vec3 scale{1.0f, 1.0f, 1.0f};
    vkpt::pathtracer::Vec3 local_bounds_min{};
    vkpt::pathtracer::Vec3 local_bounds_max{};
  };

  m_gpuVerts.clear();
  m_gpuTexcoords.clear();
  m_gpuIdx.clear();
  m_gpuTriData.clear();
  m_gpuInsts.clear();
  m_gpuLocalBvh.clear();
  m_dynamicInstanceTransformsEnabled = false;
  m_staticTriangleCount = 0u;
  m_gpuVerts.reserve(std::max<std::size_t>(3u, m_sceneData.vertices.size() * 3u + m_sceneData.local_vertices.size() * 3u));
  m_gpuTexcoords.reserve(std::max<std::size_t>(2u, m_sceneData.vertices.size() * 2u + m_sceneData.local_vertices.size() * 2u));
  m_gpuIdx.reserve(std::max<std::size_t>(1u, m_sceneData.indices.size() + m_sceneData.local_indices.size()));
  std::vector<PackedInstance> packedInstances(m_sceneData.instances.size());
  std::vector<uint32_t> staticTriMat;
  staticTriMat.reserve(m_sceneData.indices.size() / 3u);
  const BvhBuildConfig bvhConfig{
      m_bvhLeafSize,
      m_bvhBucketCount,
      m_bvhSplitMode == "median"};

  auto append_vertex = [&](const vkpt::pathtracer::Vec3& vertex, uint32_t sourceIndex = kInvalidTextureIndex) {
    const uint32_t out = static_cast<uint32_t>(m_gpuVerts.size() / 3u);
    m_gpuVerts.push_back(vertex.x);
    m_gpuVerts.push_back(vertex.y);
    m_gpuVerts.push_back(vertex.z);
    if (sourceIndex < m_sceneData.texcoords.size()) {
      const auto& uv = m_sceneData.texcoords[sourceIndex];
      m_gpuTexcoords.push_back(uv.u);
      m_gpuTexcoords.push_back(uv.v);
    } else {
      m_gpuTexcoords.push_back(0.0f);
      m_gpuTexcoords.push_back(0.0f);
    }
    return out;
  };
  auto expand_bounds = [](vkpt::pathtracer::Vec3& bmin,
                          vkpt::pathtracer::Vec3& bmax,
                          const vkpt::pathtracer::Vec3& point,
                          bool& valid) {
    if (!valid) {
      bmin = point;
      bmax = point;
      valid = true;
      return;
    }
    bmin.x = std::min(bmin.x, point.x);
    bmin.y = std::min(bmin.y, point.y);
    bmin.z = std::min(bmin.z, point.z);
    bmax.x = std::max(bmax.x, point.x);
    bmax.y = std::max(bmax.y, point.y);
    bmax.z = std::max(bmax.z, point.z);
  };

  auto instance_has_compatible_triangles = [&](const vkpt::pathtracer::RTInstance& inst) {
    if (inst.triangle_count == 0u || inst.first_triangle > m_sceneData.indices.size() / 3u) {
      return false;
    }
    const uint64_t firstIndex = static_cast<uint64_t>(inst.first_triangle) * 3ull;
    const uint64_t indexCount = static_cast<uint64_t>(inst.triangle_count) * 3ull;
    const uint64_t indexSize = static_cast<uint64_t>(m_sceneData.indices.size());
    return firstIndex <= indexSize && indexCount <= indexSize - firstIndex;
  };

  auto instance_has_local_triangles = [&](const vkpt::pathtracer::RTInstance& inst) {
    const uint64_t firstIndex = inst.local_first_index;
    const uint64_t indexCount = inst.local_index_count;
    const uint64_t firstVertex = inst.local_first_vertex;
    const uint64_t vertexCount = inst.local_vertex_count;
    const uint64_t localIndexSize = static_cast<uint64_t>(m_sceneData.local_indices.size());
    const uint64_t localVertexSize = static_cast<uint64_t>(m_sceneData.local_vertices.size());
    return indexCount >= 3u &&
           indexCount % 3u == 0u &&
           firstIndex <= localIndexSize &&
           indexCount <= localIndexSize - firstIndex &&
           vertexCount > 0u &&
           firstVertex <= localVertexSize &&
           vertexCount <= localVertexSize - firstVertex;
  };

  const bool allowDynamicInstanceTransforms = m_dynamicInstanceTransformsAllowed;
  // Dynamic instances carry local geometry plus transform data. Static geometry
  // is baked into world-space triangle buffers for cheaper shader traversal.
  auto should_use_dynamic_instance = [&](const vkpt::pathtracer::RTInstance& inst) {
    return allowDynamicInstanceTransforms &&
           inst.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform) &&
           instance_has_local_triangles(inst);
  };

  auto make_base_packed_instance = [](const vkpt::pathtracer::RTInstance& inst) {
    PackedInstance packed{};
    packed.material_index = inst.material_index;
    packed.flags = inst.flags;
    packed.translation = inst.translation;
    packed.rotation = inst.rotation;
    packed.scale = inst.scale;
    return packed;
  };

  auto append_static_instance = [&](std::size_t instanceIndex) {
    const auto& inst = m_sceneData.instances[instanceIndex];
    PackedInstance packed = make_base_packed_instance(inst);
    packed.flags &= ~vkpt::pathtracer::kRTInstanceFlagDynamicTransform;
    packed.first_triangle = static_cast<uint32_t>(m_gpuIdx.size() / 3u);
    if (!instance_has_compatible_triangles(inst)) {
      packedInstances[instanceIndex] = packed;
      return;
    }

    const uint32_t firstIndex = inst.first_triangle * 3u;
    const uint32_t indexCount = inst.triangle_count * 3u;
    uint32_t minIndex = std::numeric_limits<uint32_t>::max();
    uint32_t maxIndex = 0u;
    for (uint32_t offset = 0u; offset < indexCount; ++offset) {
      const uint32_t index = m_sceneData.indices[firstIndex + offset];
      if (index >= m_sceneData.vertices.size()) {
        continue;
      }
      minIndex = std::min(minIndex, index);
      maxIndex = std::max(maxIndex, index);
    }
    if (minIndex == std::numeric_limits<uint32_t>::max() || maxIndex >= m_sceneData.vertices.size()) {
      packedInstances[instanceIndex] = packed;
      return;
    }

    const uint32_t baseVertex = static_cast<uint32_t>(m_gpuVerts.size() / 3u);
    for (uint32_t index = minIndex; index <= maxIndex; ++index) {
      append_vertex(m_sceneData.vertices[index], index);
    }
    for (uint32_t offset = 0u; offset < indexCount; offset += 3u) {
      const uint32_t i0 = m_sceneData.indices[firstIndex + offset + 0u];
      const uint32_t i1 = m_sceneData.indices[firstIndex + offset + 1u];
      const uint32_t i2 = m_sceneData.indices[firstIndex + offset + 2u];
      if (i0 < minIndex || i1 < minIndex || i2 < minIndex ||
          i0 > maxIndex || i1 > maxIndex || i2 > maxIndex) {
        continue;
      }
      m_gpuIdx.push_back(baseVertex + (i0 - minIndex));
      m_gpuIdx.push_back(baseVertex + (i1 - minIndex));
      m_gpuIdx.push_back(baseVertex + (i2 - minIndex));
      staticTriMat.push_back(inst.material_index);
    }
    packed.triangle_count = static_cast<uint32_t>(m_gpuIdx.size() / 3u) - packed.first_triangle;
    packedInstances[instanceIndex] = packed;
  };

  auto append_dynamic_instance = [&](std::size_t instanceIndex) {
    const auto& inst = m_sceneData.instances[instanceIndex];
    PackedInstance packed = make_base_packed_instance(inst);
    packed.flags |= vkpt::pathtracer::kRTInstanceFlagDynamicTransform;
    packed.first_triangle = static_cast<uint32_t>(m_gpuIdx.size() / 3u);

    const uint32_t baseVertex = static_cast<uint32_t>(m_gpuVerts.size() / 3u);
    bool boundsValid = false;
    for (uint32_t offset = 0u; offset < inst.local_vertex_count; ++offset) {
      const auto& vertex = m_sceneData.local_vertices[inst.local_first_vertex + offset];
      expand_bounds(packed.local_bounds_min, packed.local_bounds_max, vertex, boundsValid);
      append_vertex(vertex);
    }
    for (uint32_t offset = 0u; offset < inst.local_index_count; offset += 3u) {
      const uint32_t i0 = m_sceneData.local_indices[inst.local_first_index + offset + 0u];
      const uint32_t i1 = m_sceneData.local_indices[inst.local_first_index + offset + 1u];
      const uint32_t i2 = m_sceneData.local_indices[inst.local_first_index + offset + 2u];
      if (i0 >= inst.local_vertex_count || i1 >= inst.local_vertex_count || i2 >= inst.local_vertex_count) {
        continue;
      }
      m_gpuIdx.push_back(baseVertex + i0);
      m_gpuIdx.push_back(baseVertex + i1);
      m_gpuIdx.push_back(baseVertex + i2);
    }
    packed.triangle_count = static_cast<uint32_t>(m_gpuIdx.size() / 3u) - packed.first_triangle;
    if (!boundsValid) {
      packed.local_bounds_min = {-1.0f, -1.0f, -1.0f};
      packed.local_bounds_max = {1.0f, 1.0f, 1.0f};
    }
    if (packed.triangle_count > 0u) {
      const std::size_t indexOffset = static_cast<std::size_t>(packed.first_triangle) * 3u;
      const std::size_t indexCount = static_cast<std::size_t>(packed.triangle_count) * 3u;
      std::vector<BvhTriRef> refs(packed.triangle_count);
      for (uint32_t t = 0u; t < packed.triangle_count; ++t) {
        const uint32_t i0 = m_gpuIdx[indexOffset + static_cast<std::size_t>(t) * 3u + 0u];
        const uint32_t i1 = m_gpuIdx[indexOffset + static_cast<std::size_t>(t) * 3u + 1u];
        const uint32_t i2 = m_gpuIdx[indexOffset + static_cast<std::size_t>(t) * 3u + 2u];
        const float v0[3] = {m_gpuVerts[i0*3u], m_gpuVerts[i0*3u+1u], m_gpuVerts[i0*3u+2u]};
        const float v1[3] = {m_gpuVerts[i1*3u], m_gpuVerts[i1*3u+1u], m_gpuVerts[i1*3u+2u]};
        const float v2[3] = {m_gpuVerts[i2*3u], m_gpuVerts[i2*3u+1u], m_gpuVerts[i2*3u+2u]};
        refs[t].orig_idx = t;
        for (int k = 0; k < 3; ++k) {
          refs[t].bmin[k] = std::min({v0[k], v1[k], v2[k]});
          refs[t].bmax[k] = std::max({v0[k], v1[k], v2[k]});
          refs[t].centroid[k] = (refs[t].bmin[k] + refs[t].bmax[k]) * 0.5f;
        }
      }

      std::vector<uint32_t> localOrigIdx(
          m_gpuIdx.begin() + static_cast<std::ptrdiff_t>(indexOffset),
          m_gpuIdx.begin() + static_cast<std::ptrdiff_t>(indexOffset + indexCount));
      std::vector<uint32_t> localTriMat(packed.triangle_count, packed.material_index);
      std::vector<uint32_t> reorderedIdx;
      std::vector<uint32_t> reorderedTriMat;
      reorderedIdx.reserve(localOrigIdx.size());
      reorderedTriMat.reserve(packed.triangle_count);
      std::vector<float> localBvh(std::max(1u, 2u * packed.triangle_count - 1u) * 8u, 0.0f);
      uint32_t nodeCount = 0u;
      bvh_build(refs, 0u, packed.triangle_count, localOrigIdx, localTriMat,
                bvhConfig, localBvh, reorderedIdx, reorderedTriMat, nodeCount);
      localBvh.resize(static_cast<std::size_t>(nodeCount) * 8u);
      if (reorderedIdx.size() == indexCount && nodeCount > 0u) {
        std::copy(reorderedIdx.begin(), reorderedIdx.end(),
                  m_gpuIdx.begin() + static_cast<std::ptrdiff_t>(indexOffset));
        const uint32_t firstNode = static_cast<uint32_t>(m_gpuLocalBvh.size() / 8u);
        for (uint32_t node = 0u; node < nodeCount; ++node) {
          const std::size_t base = static_cast<std::size_t>(node) * 8u;
          const uint32_t lf = FloatBits(localBvh[base + 6u]);
          const uint32_t rc = FloatBits(localBvh[base + 7u]);
          if ((lf & 0x80000000u) != 0u) {
            const uint32_t firstTri = (lf & 0x7fffffffu) + packed.first_triangle;
            localBvh[base + 6u] = UintBitsToFloat(0x80000000u | firstTri);
          } else {
            localBvh[base + 6u] = UintBitsToFloat(lf + firstNode);
            localBvh[base + 7u] = UintBitsToFloat(rc + firstNode);
          }
        }
        packed.local_bvh_first_node = firstNode;
        packed.local_bvh_node_count = nodeCount;
        m_gpuLocalBvh.insert(m_gpuLocalBvh.end(), localBvh.begin(), localBvh.end());
      }
    }
    packedInstances[instanceIndex] = packed;
    m_dynamicInstanceTransformsEnabled = m_dynamicInstanceTransformsEnabled || packed.triangle_count > 0u;
  };

  for (std::size_t instanceIndex = 0; instanceIndex < m_sceneData.instances.size(); ++instanceIndex) {
    if (!should_use_dynamic_instance(m_sceneData.instances[instanceIndex])) {
      append_static_instance(instanceIndex);
    }
  }
  m_staticTriangleCount = static_cast<uint32_t>(staticTriMat.size());
  for (std::size_t instanceIndex = 0; instanceIndex < m_sceneData.instances.size(); ++instanceIndex) {
    if (should_use_dynamic_instance(m_sceneData.instances[instanceIndex])) {
      append_dynamic_instance(instanceIndex);
    }
  }

  if (m_gpuVerts.empty()) {
    m_gpuVerts.assign(3, 0.0f);
    m_gpuTexcoords.assign(2, 0.0f);
  }
  if (m_gpuIdx.empty()) {
    m_gpuIdx.push_back(0u);
  }

  m_gpuInsts.clear();
  m_gpuInsts.resize(packedInstances.size() * kGpuInstanceStrideU32);
  for (std::size_t instanceIndex = 0u; instanceIndex < packedInstances.size(); ++instanceIndex) {
    const auto& inst = packedInstances[instanceIndex];
    uint32_t* dst = m_gpuInsts.data() + instanceIndex * kGpuInstanceStrideU32;
    dst[0] = inst.first_triangle;
    dst[1] = inst.triangle_count;
    dst[2] = inst.material_index;
    dst[3] = inst.flags;
    dst[4] = FloatBits(inst.translation.x);
    dst[5] = FloatBits(inst.translation.y);
    dst[6] = FloatBits(inst.translation.z);
    dst[7] = inst.local_bvh_first_node;
    dst[8] = FloatBits(inst.rotation.x);
    dst[9] = FloatBits(inst.rotation.y);
    dst[10] = FloatBits(inst.rotation.z);
    dst[11] = FloatBits(inst.rotation.w);
    dst[12] = FloatBits(inst.scale.x);
    dst[13] = FloatBits(inst.scale.y);
    dst[14] = FloatBits(inst.scale.z);
    dst[15] = inst.local_bvh_node_count;
    dst[16] = FloatBits(inst.local_bounds_min.x);
    dst[17] = FloatBits(inst.local_bounds_min.y);
    dst[18] = FloatBits(inst.local_bounds_min.z);
    dst[19] = 0u;
    dst[20] = FloatBits(inst.local_bounds_max.x);
    dst[21] = FloatBits(inst.local_bounds_max.y);
    dst[22] = FloatBits(inst.local_bounds_max.z);
    dst[23] = 0u;
  }
  if (m_gpuInsts.empty()) m_gpuInsts.assign(kGpuInstanceStrideU32, 0u);
  if (m_gpuLocalBvh.empty()) StoreEmptyBvh(m_gpuLocalBvh);
  m_dynamicInstanceCount = BuildDynamicInstanceBvhFromPackedInstances(
      m_gpuInsts,
      static_cast<uint32_t>(packedInstances.size()),
      m_gpuDynamicBvh);

  m_gpuLights.clear();
  m_gpuLights.reserve(std::max<std::size_t>(16u, m_sceneData.lights.size() * 16u));
  for (const auto& lt : m_sceneData.lights) {
    PackLight(lt, m_gpuLights);
  }
  if (m_gpuLights.empty()) m_gpuLights.assign(16, 0.0f);

  m_gpuSdfs.clear();
  m_gpuSdfs.resize(m_sceneData.sdf_primitives.size() * kGpuSdfStrideFloats);
  for (std::size_t sdfIndex = 0u; sdfIndex < m_sceneData.sdf_primitives.size(); ++sdfIndex) {
    const auto& sdf = m_sceneData.sdf_primitives[sdfIndex];
    const uint32_t shape = static_cast<uint32_t>(sdf.shape);
    float* dst = m_gpuSdfs.data() + sdfIndex * kGpuSdfStrideFloats;
    dst[0] = static_cast<float>(shape);
    dst[1] = static_cast<float>(sdf.material_index);
    dst[2] = std::max(0.001f, sdf.radius);
    dst[3] = sdf.param_a;
    dst[4] = sdf.position.x;
    dst[5] = sdf.position.y;
    dst[6] = sdf.position.z;
    dst[7] = sdf.param_b;
    dst[8] = sdf.scale.x;
    dst[9] = sdf.scale.y;
    dst[10] = sdf.scale.z;
    dst[11] = 0.0f;
    dst[12] = sdf.rotation.x;
    dst[13] = sdf.rotation.y;
    dst[14] = sdf.rotation.z;
    dst[15] = 0.0f;
  }
  if (m_gpuSdfs.empty()) m_gpuSdfs.assign(kGpuSdfStrideFloats, 0.0f);
  if (!build_texture_buffers()) {
    return false;
  }
  m_gpuEnv.clear();
  m_gpuEnvMeta = {
      m_sceneData.environment_map_width,
      m_sceneData.environment_map_height,
      0u,
      0u};
  const uint64_t environmentPixelCount =
      static_cast<uint64_t>(m_sceneData.environment_map_width) *
      static_cast<uint64_t>(m_sceneData.environment_map_height);
  if (environmentPixelCount > 0u &&
      m_sceneData.environment_map.size() >= environmentPixelCount) {
    auto scale = m_sceneData.environment_map_scale;
    if (std::max({scale.x, scale.y, scale.z}) <= 1.0e-6f) {
      scale = {1.0f, 1.0f, 1.0f};
    }
    m_gpuEnv.reserve(static_cast<std::size_t>(environmentPixelCount) * 3u);
    for (std::size_t i = 0u; i < static_cast<std::size_t>(environmentPixelCount); ++i) {
      const auto& texel = m_sceneData.environment_map[i];
      m_gpuEnv.push_back(texel.x * scale.x);
      m_gpuEnv.push_back(texel.y * scale.y);
      m_gpuEnv.push_back(texel.z * scale.z);
    }
    m_gpuEnvMeta[2] = 1u;
  }
  if (m_gpuEnv.empty()) {
    m_gpuEnv.assign(3u, 0.0f);
    m_gpuEnvMeta = {0u, 0u, 0u, 0u};
  }

  std::ostringstream us;
  us << "packed scene verts=" << (m_gpuVerts.size() / 3u)
     << " idx=" << m_gpuIdx.size()
     << " mats=" << (m_gpuMats.size() / 16u)
     << " inst=" << (m_gpuInsts.size() / kGpuInstanceStrideU32)
     << " static_tris=" << m_staticTriangleCount
     << " dynamic_xform=" << (m_dynamicInstanceTransformsEnabled ? "on" : "off")
     << " dynamic_tlas_instances=" << m_dynamicInstanceCount
     << " dynamic_tlas_nodes=" << (m_gpuDynamicBvh.size() / 8u)
     << " local_bvh_nodes=" << (m_gpuLocalBvh.size() / 8u)
     << " sdfs=" << m_sceneData.sdf_primitives.size()
     << " tess=" << m_sceneData.tessellation_requests.size()
     << " lights=" << (m_gpuLights.size() / 16u)
     << " textures=" << (m_gpuTextureMeta.size() / 4u)
     << " env=" << m_gpuEnvMeta[0] << "x" << m_gpuEnvMeta[1]
     << " bytes="
     << (m_gpuVerts.size() * sizeof(float)) << ","
     << (m_gpuIdx.size() * sizeof(uint32_t)) << ","
     << (m_gpuMats.size() * sizeof(float)) << ","
     << (m_gpuInsts.size() * sizeof(uint32_t)) << ","
     << (m_gpuLocalBvh.size() * sizeof(float)) << ","
     << (m_gpuLights.size() * sizeof(float)) << ","
     << (m_gpuSdfs.size() * sizeof(float)) << ","
     << (m_gpuTexels.size() * sizeof(uint32_t)) << ","
     << (m_gpuTextureMeta.size() * sizeof(uint32_t)) << ","
     << (m_gpuEnv.size() * sizeof(float)) << ","
     << (m_gpuEnvMeta.size() * sizeof(uint32_t));
  LogDebug(us.str());

  if (!m_sceneData.tessellation_requests.empty()) {
    const auto& request = m_sceneData.tessellation_requests.front();
    std::ostringstream tessLog;
    tessLog << "cached GPU tessellation requested count=" << m_sceneData.tessellation_requests.size()
            << " first_geometry=" << request.geometry_id
            << " factor=" << request.factor
            << " generated_vertices=" << request.generated_vertex_count
            << " generated_indices=" << request.generated_index_count
            << " projection_mode=" << request.projection_mode
            << " cache_key=" << request.cache_key
            << " cache=" << (request.cache_generated_geometry ? "on" : "off");
    LogInfo(tessLog.str());
  }

  // ===== Build BVH =====
  m_gpuBvh.clear();
  m_gpuTriMat.clear();
  const uint32_t total_tris = m_staticTriangleCount;
  if (total_tris > 0u) {
    // Build per-triangle reference list with AABB and centroid
    std::vector<BvhTriRef> refs(total_tris);
    for (uint32_t t = 0u; t < total_tris; ++t) {
      const uint32_t i0 = m_gpuIdx[t*3u+0u], i1 = m_gpuIdx[t*3u+1u], i2 = m_gpuIdx[t*3u+2u];
      const float v0[3] = {m_gpuVerts[i0*3u], m_gpuVerts[i0*3u+1u], m_gpuVerts[i0*3u+2u]};
      const float v1[3] = {m_gpuVerts[i1*3u], m_gpuVerts[i1*3u+1u], m_gpuVerts[i1*3u+2u]};
      const float v2[3] = {m_gpuVerts[i2*3u], m_gpuVerts[i2*3u+1u], m_gpuVerts[i2*3u+2u]};
      refs[t].orig_idx = t;
      for (int k = 0; k < 3; ++k) {
        refs[t].bmin[k] = std::min({v0[k], v1[k], v2[k]});
        refs[t].bmax[k] = std::max({v0[k], v1[k], v2[k]});
        refs[t].centroid[k] = (refs[t].bmin[k] + refs[t].bmax[k]) * 0.5f;
      }
    }

    // Allocate GPU node buffer (max 2*total_tris nodes for binary BVH)
    const uint32_t max_nodes = std::max(1u, 2u * total_tris - 1u);
    m_gpuBvh.assign(static_cast<size_t>(max_nodes) * 8u, 0.0f);

    std::vector<uint32_t> orig_idx_copy = m_gpuIdx; // save before reorder
    std::vector<uint32_t> reord_idx;
    reord_idx.reserve(m_gpuIdx.size());
    uint32_t node_count = 0u;
    bvh_build(refs, 0u, total_tris, orig_idx_copy, staticTriMat,
              bvhConfig, m_gpuBvh, reord_idx, m_gpuTriMat, node_count);

    // Trim to actual node count, replace the static triangle segment with the
    // BVH order, then append dynamic triangles exactly as packed for instances.
    m_gpuBvh.resize(static_cast<size_t>(node_count) * 8u);
    const std::size_t dynamicIndexStart = static_cast<std::size_t>(total_tris) * 3u;
    if (dynamicIndexStart < m_gpuIdx.size()) {
      reord_idx.insert(reord_idx.end(), m_gpuIdx.begin() + static_cast<std::ptrdiff_t>(dynamicIndexStart), m_gpuIdx.end());
    }
    m_gpuIdx = std::move(reord_idx);

    std::ostringstream bvhLog;
    bvhLog << "BVH built nodes=" << node_count << " tris=" << total_tris
           << " reord_idx=" << m_gpuIdx.size()
           << " leaf_size=" << m_bvhLeafSize
           << " buckets=" << m_bvhBucketCount
           << " split=" << m_bvhSplitMode;
    LogDebug(bvhLog.str());
  }
  if (m_gpuBvh.empty())   StoreEmptyBvh(m_gpuBvh);   // dummy empty root leaf
  if (m_gpuTriMat.empty()) m_gpuTriMat.push_back(0u);   // dummy entry

  m_gpuTriData.clear();
  const uint32_t packedTriCount = static_cast<uint32_t>(m_gpuIdx.size() / 3u);
  const bool fullPackedTriDataRequired = m_preferDxr || m_packedTriangleBufferEnabled;
  if (fullPackedTriDataRequired) {
    std::vector<uint32_t> packedTriMat(packedTriCount, 0u);
    for (uint32_t tri = 0u; tri < m_staticTriangleCount && tri < m_gpuTriMat.size() && tri < packedTriCount; ++tri) {
      packedTriMat[tri] = m_gpuTriMat[tri];
    }
    for (const auto& inst : packedInstances) {
      if ((inst.flags & vkpt::pathtracer::kRTInstanceFlagDynamicTransform) == 0u) {
        continue;
      }
      const uint64_t begin = inst.first_triangle;
      const uint64_t end = begin + inst.triangle_count;
      if (begin >= packedTriMat.size()) {
        continue;
      }
      for (uint64_t tri = begin; tri < end && tri < packedTriMat.size(); ++tri) {
        packedTriMat[static_cast<std::size_t>(tri)] = inst.material_index;
      }
    }
    auto material_is_double_sided = [&](uint32_t matIndex) {
      const std::size_t effectOffset = static_cast<std::size_t>(matIndex) * 16u + 15u;
      if (effectOffset >= m_gpuMats.size()) {
        return false;
      }
      const uint32_t packedEffect = static_cast<uint32_t>(m_gpuMats[effectOffset] + 0.5f);
      return (packedEffect & 1024u) != 0u;
    };
    m_gpuTriData.resize(static_cast<std::size_t>(packedTriCount) * kGpuTriDataStrideFloats);
    auto write_packed_tri = [&](uint32_t triIndex) {
      const std::size_t ib = static_cast<std::size_t>(triIndex) * 3u;
      const uint32_t i0 = ib + 0u < m_gpuIdx.size() ? m_gpuIdx[ib + 0u] : 0u;
      const uint32_t i1 = ib + 1u < m_gpuIdx.size() ? m_gpuIdx[ib + 1u] : i0;
      const uint32_t i2 = ib + 2u < m_gpuIdx.size() ? m_gpuIdx[ib + 2u] : i0;
      auto vertex = [&](uint32_t index, int axis) {
        const std::size_t offset = static_cast<std::size_t>(index) * 3u + static_cast<std::size_t>(axis);
        return offset < m_gpuVerts.size() ? m_gpuVerts[offset] : 0.0f;
      };
      auto texcoord = [&](uint32_t index, int axis) {
        const std::size_t offset = static_cast<std::size_t>(index) * 2u + static_cast<std::size_t>(axis);
        return offset < m_gpuTexcoords.size() ? m_gpuTexcoords[offset] : 0.0f;
      };
      const float v0[3] = {vertex(i0, 0), vertex(i0, 1), vertex(i0, 2)};
      const float v1[3] = {vertex(i1, 0), vertex(i1, 1), vertex(i1, 2)};
      const float v2[3] = {vertex(i2, 0), vertex(i2, 1), vertex(i2, 2)};
      const float uv0[2] = {texcoord(i0, 0), texcoord(i0, 1)};
      const float uv1[2] = {texcoord(i1, 0), texcoord(i1, 1)};
      const float uv2[2] = {texcoord(i2, 0), texcoord(i2, 1)};
      const float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
      const float e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
      const uint32_t matIndex = triIndex < packedTriMat.size() ? packedTriMat[triIndex] : 0u;
      const float doubleSided = material_is_double_sided(matIndex) ? 1.0f : 0.0f;
      float* dst = m_gpuTriData.data() + static_cast<std::size_t>(triIndex) * kGpuTriDataStrideFloats;
      dst[0] = v0[0];
      dst[1] = v0[1];
      dst[2] = v0[2];
      dst[3] = static_cast<float>(matIndex);
      dst[4] = e1[0];
      dst[5] = e1[1];
      dst[6] = e1[2];
      dst[7] = doubleSided;
      dst[8] = e2[0];
      dst[9] = e2[1];
      dst[10] = e2[2];
      dst[11] = 0.0f;
      dst[12] = uv0[0];
      dst[13] = uv0[1];
      dst[14] = uv1[0];
      dst[15] = uv1[1];
      dst[16] = uv2[0];
      dst[17] = uv2[1];
    };
    for (uint32_t tri = 0u; tri < packedTriCount; ++tri) {
      write_packed_tri(tri);
    }
  }
  if (m_gpuTriData.empty()) {
    m_gpuTriData.assign(kGpuTriDataStrideFloats, 0.0f);
  }
  {
    std::ostringstream triLog;
    triLog << "packed triangle data tris=" << packedTriCount
           << " stride_floats=" << kGpuTriDataStrideFloats
           << " bytes=" << (m_gpuTriData.size() * sizeof(float))
           << " full_data=" << (fullPackedTriDataRequired ? "true" : "false")
           << " compute_enabled=" << (m_packedTriangleBufferEnabled ? "true" : "false")
           << " dxr_requested=" << (m_preferDxr ? "true" : "false");
    LogDebug(triLog.str());
  }

  destroy_scene_buffers();
  if (!upload_scene_buffers()) return false;
  m_sceneUploaded = true;

  // Build hardware acceleration structures for DXR if pipeline is ready
  if (m_preferDxr && m_dxrPipelineReady && m_dxrRuntimeObjectsReady) {
    m_dxrAccelReady = false;
    if (!build_dxr_acceleration_structures()) {
      LogError("build_or_update_acceleration: BLAS/TLAS build failed: " + m_error);
      // Non-fatal: fall back to compute path
    }
  }

  LogDebug("scene upload complete");
  return true;
}

bool D3D12GpuPathTracer::build_texture_buffers() {
  m_gpuTexels.clear();
  m_gpuTextureMeta.clear();

  auto appendTexture = [&](const LoadedTextureRgba8& texture) -> bool {
    if (m_gpuTexels.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
      m_error = "texture buffer offset overflow";
      LogError("build_texture_buffers: " + m_error);
      return false;
    }
    const uint32_t offset = static_cast<uint32_t>(m_gpuTexels.size());
    m_gpuTexels.insert(m_gpuTexels.end(), texture.texels.begin(), texture.texels.end());
    m_gpuTextureMeta.push_back(offset);
    m_gpuTextureMeta.push_back(std::max(1u, texture.width));
    m_gpuTextureMeta.push_back(std::max(1u, texture.height));
    m_gpuTextureMeta.push_back(1u);
    return true;
  };

  if (m_sceneData.textures.empty()) {
    return appendTexture(LoadedTextureRgba8{});
  }

  for (const auto& uri : m_sceneData.textures) {
    LoadedTextureRgba8 texture;
    std::string loadError;
    if (!LoadTextureRgba8(uri, kMaxTextureDimension, texture, &loadError)) {
      LogError("texture load failed uri=" + uri + " error=" + loadError);
      texture = LoadedTextureRgba8{};
    }
    if (!appendTexture(texture)) {
      return false;
    }
  }

  std::ostringstream log;
  log << "texture buffers built count=" << (m_gpuTextureMeta.size() / 4u)
      << " texels=" << m_gpuTexels.size()
      << " bytes=" << (m_gpuTexels.size() * sizeof(uint32_t));
  LogInfo(log.str());
  return true;
}

bool D3D12GpuPathTracer::upload_scene_buffers() {
  if (!wait_for_gpu()) {
    m_error = "wait_for_gpu before scene upload";
    LogError("upload_scene_buffers: " + m_error);
    return false;
  }
  if (FAILED(m_cmdAllocator->Reset())) {
    m_error = "upload cmd allocator reset";
    LogError("upload_scene_buffers: " + m_error);
    return false;
  }
  if (FAILED(m_cmdList->Reset(m_cmdAllocator.Get(), nullptr))) {
    m_error = "upload cmd list reset";
    LogError("upload_scene_buffers: " + m_error);
    return false;
  }

  UINT64 offset = 0;
  auto align = [](UINT64 x) { return (x + 255ull) & ~255ull; };

  // Each packed array is copied into one default-heap buffer. Offsets in the
  // persistent upload heap are 256-byte aligned to satisfy D3D12 copy rules.
  auto stage = [&](const char* name, const void* data, UINT64 size, ID3D12Resource** dst) {
    const bool shouldLogUpload = g_d3d12SceneUploadCalls < 8u;
    if (shouldLogUpload) {
      std::ostringstream stageInfo;
      stageInfo << "upload_scene stage begin name=" << name << " bytes=" << size;
      LogDebug(stageInfo.str());
    }
    if (offset + size > m_uploadSize) {
      m_error = std::string("upload_scene_buffers overflow for ") + name;
      LogError("upload_scene_buffers: " + m_error);
      return false;
    }
    if (!data || size == 0u) {
      m_error = std::string("upload_scene_buffers invalid data for ") + name;
      LogError("upload_scene_buffers: " + m_error);
      return false;
    }
    const UINT64 stagedOffset = offset;
    std::memcpy(static_cast<uint8_t*>(m_uploadPtr) + offset, data, static_cast<size_t>(size));

    const D3D12_HEAP_PROPERTIES hp = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC  rd{};
    rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width=size; rd.Height=1;
    rd.DepthOrArraySize=1; rd.MipLevels=1; rd.Format=DXGI_FORMAT_UNKNOWN;
    rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.SampleDesc.Count=1;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(dst)))) {
      m_error = "upload_stage create resource failed";
      LogError("upload_scene_buffers: " + m_error);
      return false;
    }
    ++g_d3d12SceneUploadCalls;
    if (shouldLogUpload) {
      std::ostringstream st;
      st << "upload_scene stage " << name << " offset=" << stagedOffset
         << " bytes=" << size;
      if (size > 0u && std::strcmp(name, "verts") == 0) {
        st << " first=" << FormatFirstN(reinterpret_cast<const float*>(data), size / sizeof(float), 8u);
      } else if (size > 0u && (std::strcmp(name, "mats") == 0 || std::strcmp(name, "lights") == 0)) {
        st << " first=" << FormatFirstN(reinterpret_cast<const float*>(data), size / sizeof(float), 8u);
      } else if (size > 0u && (std::strcmp(name, "idx") == 0 || std::strcmp(name, "inst") == 0)) {
        st << " first=" << FormatFirstN(reinterpret_cast<const uint32_t*>(data), size / sizeof(uint32_t), 8u);
      }
      LogDebug(st.str());
    }
    m_cmdList->CopyBufferRegion(*dst, 0, m_uploadBuf.Get(), offset, size);
    offset = align(offset + size);
    return true;
  };

  if (!stage("verts", m_gpuVerts.data(),  m_gpuVerts.size()  * sizeof(float), &m_vertBuf)) return false;
  if (!stage("idx", m_gpuIdx.data(),      m_gpuIdx.size()    * sizeof(uint32_t), &m_idxBuf)) return false;
  if (!stage("mats", m_gpuMats.data(),   m_gpuMats.size()   * sizeof(float), &m_matBuf)) return false;
  if (!stage("inst", m_gpuInsts.data(),  m_gpuInsts.size()  * sizeof(uint32_t), &m_instBuf)) return false;
  if (!stage("lights", m_gpuLights.data(), m_gpuLights.size() * sizeof(float), &m_ltBuf)) return false;

  if (!stage("bvh",    m_gpuBvh.data(),    m_gpuBvh.size()    * sizeof(float),    &m_bvhBuf))    return false;
  if (!stage("trimat", m_gpuTriMat.data(), m_gpuTriMat.size() * sizeof(uint32_t), &m_triMatBuf)) return false;
  if (!stage("tridata", m_gpuTriData.data(), m_gpuTriData.size() * sizeof(float), &m_triDataBuf)) return false;
  if (!stage("dynamic_bvh", m_gpuDynamicBvh.data(), m_gpuDynamicBvh.size() * sizeof(float), &m_dynamicBvhBuf)) return false;
  if (!stage("local_bvh", m_gpuLocalBvh.data(), m_gpuLocalBvh.size() * sizeof(float), &m_localBvhBuf)) return false;
  if (!stage("sdf", m_gpuSdfs.data(), m_gpuSdfs.size() * sizeof(float), &m_sdfBuf)) return false;
  if (!stage("texels", m_gpuTexels.data(), m_gpuTexels.size() * sizeof(uint32_t), &m_texelBuf)) return false;
  if (!stage("texmeta", m_gpuTextureMeta.data(), m_gpuTextureMeta.size() * sizeof(uint32_t), &m_texMetaBuf)) return false;
  if (!stage("env", m_gpuEnv.data(), m_gpuEnv.size() * sizeof(float), &m_envBuf)) return false;
  if (!stage("envmeta", m_gpuEnvMeta.data(), m_gpuEnvMeta.size() * sizeof(uint32_t), &m_envMetaBuf)) return false;

  // Transition once after all copies so the buffers can be read by compute and
  // DXR shaders until a later delta upload transitions selected buffers back.
  D3D12_RESOURCE_BARRIER barriers[15]{};
  ID3D12Resource* bufs[] = {m_vertBuf.Get(), m_idxBuf.Get(), m_matBuf.Get(),
                            m_instBuf.Get(), m_ltBuf.Get(), m_bvhBuf.Get(), m_triMatBuf.Get(),
                            m_triDataBuf.Get(), m_dynamicBvhBuf.Get(), m_localBvhBuf.Get(), m_sdfBuf.Get(),
                            m_texelBuf.Get(), m_texMetaBuf.Get(), m_envBuf.Get(), m_envMetaBuf.Get()};
  for (int i = 0; i < 15; ++i) {
    barriers[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[i].Transition.pResource=bufs[i];
    barriers[i].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[i].Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[i].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  }
  m_cmdList->ResourceBarrier(15, barriers);
  const auto closeRes = m_cmdList->Close();
  if (FAILED(closeRes)) {
    m_error = "cmd list close";
    LogError("upload_scene_buffers: " + m_error);
    return false;
  }
  ID3D12CommandList* lists[] = {m_cmdList.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    m_error = "wait_for_gpu failed";
    LogError("upload_scene_buffers: " + m_error);
    return false;
  }
  const auto removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during upload_scene_buffers hr=" + FormatHr(removeHr);
    LogError("upload_scene_buffers: " + m_error);
    return false;
  }
  LogDebug("upload_scene_buffers complete");
  return true;
}

bool D3D12GpuPathTracer::upload_material_light_buffers(bool uploadMaterials,
                                                       bool uploadLights) {
  if (!uploadMaterials && !uploadLights) {
    return true;
  }
  if (uploadMaterials && (!m_matBuf || m_gpuMats.empty())) {
    m_error = "material delta upload missing material buffer";
    LogError("upload_material_light_buffers: " + m_error);
    return false;
  }
  if (uploadLights && (!m_ltBuf || m_gpuLights.empty())) {
    m_error = "light delta upload missing light buffer";
    LogError("upload_material_light_buffers: " + m_error);
    return false;
  }

  const UINT64 matSize = uploadMaterials
      ? static_cast<UINT64>(m_gpuMats.size()) * sizeof(float)
      : 0u;
  const UINT64 lightSize = uploadLights
      ? static_cast<UINT64>(m_gpuLights.size()) * sizeof(float)
      : 0u;
  const auto align = [](UINT64 x) { return (x + 255ull) & ~255ull; };
  const UINT64 lightOffset = align(matSize);
  const UINT64 requiredSize = lightOffset + lightSize;
  if (requiredSize > m_uploadSize) {
    m_error = "upload_material_light_buffers overflow";
    LogError("upload_material_light_buffers: " + m_error);
    return false;
  }
  if (!wait_for_gpu()) {
    LogError("upload_material_light_buffers: " + m_error);
    return false;
  }
  if (FAILED(m_cmdAllocator->Reset()) ||
      FAILED(m_cmdList->Reset(m_cmdAllocator.Get(), nullptr))) {
    m_error = "material/light upload command reset failed";
    LogError("upload_material_light_buffers: " + m_error);
    return false;
  }

  // Delta uploads rewrite whole material/light buffers. That keeps descriptor
  // bindings stable and avoids per-element copy bookkeeping.
  D3D12_RESOURCE_BARRIER toCopy[2]{};
  UINT barrierCount = 0u;
  auto addToCopy = [&](ID3D12Resource* resource) {
    auto& barrier = toCopy[barrierCount++];
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  };

  if (uploadMaterials) {
    std::memcpy(m_uploadPtr, m_gpuMats.data(), static_cast<std::size_t>(matSize));
    addToCopy(m_matBuf.Get());
  }
  if (uploadLights) {
    std::memcpy(static_cast<uint8_t*>(m_uploadPtr) + lightOffset,
                m_gpuLights.data(),
                static_cast<std::size_t>(lightSize));
    addToCopy(m_ltBuf.Get());
  }
  m_cmdList->ResourceBarrier(barrierCount, toCopy);
  if (uploadMaterials) {
    m_cmdList->CopyBufferRegion(m_matBuf.Get(), 0, m_uploadBuf.Get(), 0, matSize);
  }
  if (uploadLights) {
    m_cmdList->CopyBufferRegion(m_ltBuf.Get(), 0, m_uploadBuf.Get(), lightOffset, lightSize);
  }

  D3D12_RESOURCE_BARRIER toSrv[2]{};
  for (UINT i = 0u; i < barrierCount; ++i) {
    toSrv[i] = toCopy[i];
    toSrv[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toSrv[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  }
  m_cmdList->ResourceBarrier(barrierCount, toSrv);

  if (FAILED(m_cmdList->Close())) {
    m_error = "material/light upload command close failed";
    LogError("upload_material_light_buffers: " + m_error);
    return false;
  }
  ID3D12CommandList* lists[] = {m_cmdList.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    LogError("upload_material_light_buffers: " + m_error);
    return false;
  }
  const auto removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during upload_material_light_buffers hr=" + FormatHr(removeHr);
    LogError("upload_material_light_buffers: " + m_error);
    return false;
  }
  return true;
}

bool D3D12GpuPathTracer::upload_instance_buffer() {
  if (!m_instBuf || !m_dynamicBvhBuf || m_gpuInsts.empty() || m_gpuDynamicBvh.empty()) {
    return false;
  }
  const UINT64 instSize = static_cast<UINT64>(m_gpuInsts.size()) * sizeof(uint32_t);
  const UINT64 tlasSize = static_cast<UINT64>(m_gpuDynamicBvh.size()) * sizeof(float);
  const auto align = [](UINT64 x) { return (x + 255ull) & ~255ull; };
  const UINT64 tlasOffset = align(instSize);
  if (tlasOffset + tlasSize > m_uploadSize) {
    m_error = "upload_instance_buffer overflow";
    LogError("upload_instance_buffer: " + m_error);
    return false;
  }
  if (!wait_for_gpu()) {
    LogError("upload_instance_buffer: " + m_error);
    return false;
  }
  if (FAILED(m_cmdAllocator->Reset()) ||
      FAILED(m_cmdList->Reset(m_cmdAllocator.Get(), nullptr))) {
    m_error = "instance upload command reset failed";
    LogError("upload_instance_buffer: " + m_error);
    return false;
  }

  // Instance transforms and the dynamic-instance BVH must update together so
  // shader traversal never observes mismatched transform and bounds data.
  std::memcpy(m_uploadPtr, m_gpuInsts.data(), static_cast<std::size_t>(instSize));
  std::memcpy(static_cast<uint8_t*>(m_uploadPtr) + tlasOffset,
              m_gpuDynamicBvh.data(),
              static_cast<std::size_t>(tlasSize));
  D3D12_RESOURCE_BARRIER toCopy[2]{};
  ID3D12Resource* resources[2] = {m_instBuf.Get(), m_dynamicBvhBuf.Get()};
  for (int i = 0; i < 2; ++i) {
    toCopy[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy[i].Transition.pResource = resources[i];
    toCopy[i].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toCopy[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    toCopy[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  }
  m_cmdList->ResourceBarrier(2, toCopy);
  m_cmdList->CopyBufferRegion(m_instBuf.Get(), 0, m_uploadBuf.Get(), 0, instSize);
  m_cmdList->CopyBufferRegion(m_dynamicBvhBuf.Get(), 0, m_uploadBuf.Get(), tlasOffset, tlasSize);

  D3D12_RESOURCE_BARRIER toSrv[2] = {toCopy[0], toCopy[1]};
  for (auto& barrier : toSrv) {
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  }
  m_cmdList->ResourceBarrier(2, toSrv);

  if (FAILED(m_cmdList->Close())) {
    m_error = "instance upload command close failed";
    LogError("upload_instance_buffer: " + m_error);
    return false;
  }
  ID3D12CommandList* lists[] = {m_cmdList.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    LogError("upload_instance_buffer: " + m_error);
    return false;
  }
  const auto removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during upload_instance_buffer hr=" + FormatHr(removeHr);
    LogError("upload_instance_buffer: " + m_error);
    return false;
  }
  return true;
}

void D3D12GpuPathTracer::destroy_scene_buffers() {
  m_vertBuf.Reset(); m_idxBuf.Reset(); m_matBuf.Reset();
  m_instBuf.Reset(); m_ltBuf.Reset();
  m_bvhBuf.Reset();  m_triMatBuf.Reset();
  m_triDataBuf.Reset();
  m_dynamicBvhBuf.Reset();
  m_localBvhBuf.Reset();
  m_sdfBuf.Reset();
  m_texelBuf.Reset();
  m_texMetaBuf.Reset();
  m_envBuf.Reset();
  m_envMetaBuf.Reset();
  m_srvUavHeap.Reset();
  m_sceneUploaded = false;
}

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
