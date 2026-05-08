#ifdef PT_ENABLE_VULKAN

#include "gpu/VulkanGpuPathTracer.h"
#include "core/Logging.h"
#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "pathtracer/PathTracerObservability.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <vector>

// Convenience macro: check VkResult and log+return-false on failure
#define VK_CHK(expr)                                                            \
  do {                                                                           \
    VkResult _r = (expr);                                                       \
    if (_r != VK_SUCCESS) {                                                      \
      std::ostringstream _ss;                                                    \
      _ss << #expr << " failed: VkResult=" << static_cast<int>(_r);             \
      m_error = _ss.str();                                                       \
      vkpt::log::Logger::instance().log(                                         \
          vkpt::log::Severity::Error, "vulkan", m_error);                        \
      return false;                                                              \
    }                                                                            \
  } while (0)

#define VK_STATUS_CHK(expr)                                                     \
  do {                                                                          \
    VkResult _r = (expr);                                                       \
    if (_r != VK_SUCCESS) {                                                     \
      std::ostringstream _ss;                                                   \
      _ss << #expr << " failed: VkResult=" << static_cast<int>(_r);            \
      m_error = _ss.str();                                                      \
      vkpt::log::Logger::instance().log(                                        \
          vkpt::log::Severity::Error, "vulkan", m_error);                      \
      return vkpt::core::Status::error(                                         \
          vkpt::core::StatusCode::InternalError, m_error);                      \
    }                                                                           \
  } while (0)

namespace vkpt::gpu {

namespace {

constexpr uint64_t kTelemetrySamplePeriodNs = 1000000000ull;
constexpr std::size_t kVulkanMaterialStrideFloats =
    vkpt::pathtracer::kStandardGpuSceneBufferLayout.material_stride_floats;
constexpr std::size_t kVulkanInstanceStrideU32 =
    vkpt::pathtracer::kStandardGpuSceneBufferLayout.instance_stride_u32;
constexpr std::size_t kVulkanLightStrideFloats =
    vkpt::pathtracer::kStandardGpuSceneBufferLayout.light_stride_floats;
constexpr std::size_t kVulkanSdfStrideFloats =
    vkpt::pathtracer::kStandardGpuSceneBufferLayout.sdf_stride_floats;
constexpr uint32_t kVulkanWorkgroupSizeX = 8u;
constexpr uint32_t kVulkanWorkgroupSizeY = 8u;
constexpr uint32_t kVulkanCommandBufferCount = 3u;
constexpr uint32_t kVulkanTileHeightRows = 16u;
static_assert(kVulkanTileHeightRows % kVulkanWorkgroupSizeY == 0u,
              "Vulkan row tiles must align to compute workgroups");
static_assert(kVulkanCommandBufferCount >= 2u,
              "Vulkan submission must keep multiple command buffers available");

uint32_t VkFloatBits(float value) {
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

uint64_t ElapsedUs(std::chrono::steady_clock::time_point start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
}

vkpt::pathtracer::PathTracerLifecycle LifecycleFromGpuState(bool valid,
                                                            bool configured,
                                                            bool scene_loaded,
                                                            bool accel_valid,
                                                            const std::string& error) {
  if (!valid && !error.empty()) {
    return vkpt::pathtracer::PathTracerLifecycle::Failed;
  }
  if (!configured) {
    return vkpt::pathtracer::PathTracerLifecycle::Uninitialized;
  }
  if (!scene_loaded) {
    return vkpt::pathtracer::PathTracerLifecycle::Configured;
  }
  if (!accel_valid) {
    return vkpt::pathtracer::PathTracerLifecycle::SceneLoaded;
  }
  return vkpt::pathtracer::PathTracerLifecycle::Ready;
}

}  // namespace

// ============================================================================
// Construction / destruction
// ============================================================================

VulkanGpuPathTracer::VulkanGpuPathTracer(std::string spv_path)
    : m_spvPath(std::move(spv_path)) {
  vkpt::core::Status initStatus = init_device();
  if (initStatus) {
    initStatus = create_cmd_pool();
  }
  if (initStatus) {
    initStatus = create_pipeline();
  }
  m_valid = initStatus.is_ok();
  if (!m_valid && m_error.empty()) {
    m_error = initStatus.message;
  }
  if (!m_valid) {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Error, "vulkan",
        "VulkanGpuPathTracer init failed: " + m_error);
    EmitGpuBackendAnomaly("vulkan",
                          "initialize",
                          m_error.empty() ? "initialization failed" : m_error,
                          introspect());
  } else {
    EmitGpuBackendStarted("vulkan", introspect());
  }
}

VulkanGpuPathTracer::~VulkanGpuPathTracer() {
  shutdown();
}

VulkanGpuSubmissionContract VulkanGpuPathTracer::submission_contract() {
  return VulkanGpuSubmissionContract{
      kVulkanCommandBufferCount,
      kVulkanCommandBufferCount,
      kVulkanWorkgroupSizeX,
      kVulkanWorkgroupSizeY,
      kVulkanTileHeightRows,
      true,
      true,
      true,
      true,
      true,
      true,
      true,
      true,
      true,
      true,
      true,
      true};
}

GpuBackendIntrospection VulkanGpuPathTracer::introspect() const {
  GpuBackendIntrospection info;
  info.adapter_name = m_gpuName;
  info.vram_bytes_used = m_deviceMemoryBytes;
  info.vram_bytes_total = m_deviceMemoryLimitBytes;
  info.pending_dispatches = m_pendingDispatches;
  info.last_present_us = 0u;
  info.last_fence_wait_us = m_lastFenceWaitUs;
  info.timeline_value = m_completedTimelineValue;
  info.device_lost_recent = m_recentDeviceLost;
  info.fence_timeout_recent = m_recentFenceTimeout;
  info.lifecycle = vkpt::pathtracer::observability::ToComponentLifecycle(
      LifecycleFromGpuState(m_valid,
                            m_configured,
                            m_hasScene,
                            m_sceneUploaded,
                            m_error));
  info.current_flow_id = m_accumulationGeneration;
  info.set_determinism(m_settings.determinism_context());
  info.last_error = m_error;
  return info;
}

vkpt::core::health::Report VulkanGpuPathTracer::health_report() const {
  return GpuHealthReportFromIntrospection(introspect());
}

// ============================================================================
// IPathTracer
// ============================================================================

vkpt::core::Status VulkanGpuPathTracer::configure(const vkpt::pathtracer::RenderSettings& s) {
  if (!m_valid) {
    EmitGpuBackendAnomaly("vulkan",
                          "configure",
                          m_error.empty() ? "Vulkan backend is not initialized" : m_error,
                          introspect());
    return vkpt::core::Status::error(vkpt::core::StatusCode::NotReady,
                                     m_error.empty() ? "Vulkan backend is not initialized" : m_error);
  }
  const auto previousDeterminism = m_settings.determinism_context();
  m_settings    = s;
  vkpt::core::EmitDeterminismChangedIfNeeded("gpu",
                                             previousDeterminism,
                                             m_settings.determinism_context());
  m_configured  = true;
  const uint64_t filmPixels = static_cast<uint64_t>(s.width) * s.height;
  if (filmPixels > std::numeric_limits<uint32_t>::max()) {
    m_error = "film dimensions exceed Vulkan backend limits";
    EmitGpuBackendAnomaly("vulkan", "configure", m_error, introspect());
    return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument, m_error);
  }
  m_filmPixels  = static_cast<uint32_t>(filmPixels);

  // The film buffer is host-visible and coherent, so resolve paths can read
  // accumulated RGBA32F data without staging copies or explicit invalidation.
  destroy_film_buffers();
  m_latestFilmReadbackToken = {};
  const VkDeviceSize filmBytes =
      static_cast<VkDeviceSize>(m_filmPixels) * 4u * sizeof(float);
  if (!make_buffer(filmBytes,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   m_filmBuf, m_filmMem)) {
    return vkpt::core::Status::error(vkpt::core::StatusCode::AllocFailed, m_error);
  }
  VK_STATUS_CHK(vkMapMemory(m_device, m_filmMem, 0, filmBytes, 0, &m_filmPtr));
  std::memset(m_filmPtr, 0, static_cast<std::size_t>(filmBytes));

  m_film.resize(s.width, s.height);
  m_film.set_resolve_settings(s.film_resolve);
  m_film.clear();
  m_cpuFilmDirty = false;
  m_counters      = {};
  m_hasScene      = false;
  m_sceneUploaded = false;
  m_currentSample = 0u;
  ++m_accumulationGeneration;
  publish_device_memory_telemetry();

  // Descriptors depend on the film buffer — reset them
  if (m_ds != VK_NULL_HANDLE) {
    vkFreeDescriptorSets(m_device, m_dsPool, 1, &m_ds);
    m_ds = VK_NULL_HANDLE;
  }
  if (!create_descriptors()) {
    EmitGpuBackendAnomaly("vulkan", "configure", m_error, introspect());
    return vkpt::core::Status::error(vkpt::core::StatusCode::InternalError, m_error);
  }
  EmitGpuBackendConfig("vulkan", introspect());
  debug_check_state_contract("configure");
  return vkpt::core::Status::ok("Vulkan path tracer configured");
}

vkpt::core::Status VulkanGpuPathTracer::update_render_settings_status(
    const vkpt::pathtracer::RenderSettings& s) {
  (void)s;
  return vkpt::core::Status::error(
      vkpt::core::StatusCode::Unsupported,
      "Vulkan backend requires configure/load/build for render setting changes");
}

vkpt::core::Status VulkanGpuPathTracer::load_scene_snapshot(
    const vkpt::pathtracer::PathTracerSceneSnapshot& scene) {
  if (!m_configured) {
    m_error = "load_scene_snapshot before configure";
    EmitGpuBackendAnomaly("vulkan", "load_scene_snapshot", m_error, introspect());
    return vkpt::core::Status::error(vkpt::core::StatusCode::NotReady, m_error);
  }
  m_sceneData     = scene;
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData));
  m_cpuFilmDirty = false;
  m_hasScene      = true;
  m_sceneUploaded = false;
  debug_check_state_contract("load_scene_snapshot");
  return vkpt::core::Status::ok("Vulkan scene loaded");
}

vkpt::core::Status VulkanGpuPathTracer::build_or_update_acceleration() {
  if (!m_hasScene) {
    m_error = "build_or_update_acceleration before load_scene_snapshot";
    EmitGpuBackendAnomaly("vulkan", "build_or_update_acceleration", m_error, introspect());
    return vkpt::core::Status::error(vkpt::core::StatusCode::NotReady, m_error);
  }
  if (!recreate_scene_buffers_from_snapshot()) {
    EmitGpuBackendAnomaly("vulkan", "build_or_update_acceleration", m_error, introspect());
    return vkpt::core::Status::error(vkpt::core::StatusCode::InternalError, m_error);
  }
  EmitGpuBackendStarted("vulkan", introspect());
  debug_check_state_contract("build_or_update_acceleration");
  return vkpt::core::Status::ok("Vulkan scene buffers ready");
}

bool VulkanGpuPathTracer::recreate_scene_buffers_from_snapshot() {
  // Pack PathTracerSceneSnapshot into flat arrays matching pathtrace.comp storage-buffer
  // contracts. Dummy entries keep descriptor ranges non-empty for empty scenes.
  m_gpuVerts.clear();
  if (m_sceneData.vertices.empty()) {
    m_gpuVerts.assign(3u, 0.0f);
  } else {
    m_gpuVerts.resize(m_sceneData.vertices.size() * 3u);
    for (std::size_t i = 0u; i < m_sceneData.vertices.size(); ++i) {
      const auto& v = m_sceneData.vertices[i];
      const std::size_t base = i * 3u;
      m_gpuVerts[base + 0u] = v.x;
      m_gpuVerts[base + 1u] = v.y;
      m_gpuVerts[base + 2u] = v.z;
    }
  }

  m_gpuIdx = m_sceneData.indices;
  if (m_gpuIdx.empty()) m_gpuIdx.push_back(0u);

  m_gpuMats.clear();
  m_gpuMats.resize(std::max<std::size_t>(kVulkanMaterialStrideFloats,
                                         m_sceneData.materials.size() * kVulkanMaterialStrideFloats),
                   0.0f);
  for (std::size_t i = 0u; i < m_sceneData.materials.size(); ++i) {
    const auto& m = m_sceneData.materials[i];
    const std::size_t base = i * kVulkanMaterialStrideFloats;
    const uint32_t packed_effect = (m.material_effect & 1023u) | ((m.material_flags & 1u) ? 1024u : 0u);
    m_gpuMats[base + 0u] = m.albedo.x;
    m_gpuMats[base + 1u] = m.albedo.y;
    m_gpuMats[base + 2u] = m.albedo.z;
    m_gpuMats[base + 3u] = m.emissive.x;
    m_gpuMats[base + 4u] = m.emissive.y;
    m_gpuMats[base + 5u] = m.emissive.z;
    m_gpuMats[base + 6u] = m.roughness;
    m_gpuMats[base + 7u] = static_cast<float>(m.material_model);
    m_gpuMats[base + 8u] = m.metallic;
    m_gpuMats[base + 9u] = m.ior;
    m_gpuMats[base + 10u] = m.transmission;
    m_gpuMats[base + 11u] = m.clearcoat;
    m_gpuMats[base + 12u] = m.sheen;
    m_gpuMats[base + 13u] = m.anisotropy;
    m_gpuMats[base + 14u] = m.alpha;
    m_gpuMats[base + 15u] = static_cast<float>(packed_effect);
  }

  m_gpuInsts.clear();
  m_gpuInsts.resize(std::max<std::size_t>(kVulkanInstanceStrideU32,
                                          m_sceneData.instances.size() * kVulkanInstanceStrideU32),
                    0u);
  for (std::size_t i = 0u; i < m_sceneData.instances.size(); ++i) {
    const auto& inst = m_sceneData.instances[i];
    const std::size_t base = i * kVulkanInstanceStrideU32;
    m_gpuInsts[base + 0u] = inst.first_triangle;
    m_gpuInsts[base + 1u] = inst.triangle_count;
    m_gpuInsts[base + 2u] = inst.material_index;
    m_gpuInsts[base + 3u] = inst.flags;
    m_gpuInsts[base + 4u] = VkFloatBits(inst.translation.x);
    m_gpuInsts[base + 5u] = VkFloatBits(inst.translation.y);
    m_gpuInsts[base + 6u] = VkFloatBits(inst.translation.z);
    m_gpuInsts[base + 7u] = inst.transform_revision;
    m_gpuInsts[base + 8u] = VkFloatBits(inst.rotation.x);
    m_gpuInsts[base + 9u] = VkFloatBits(inst.rotation.y);
    m_gpuInsts[base + 10u] = VkFloatBits(inst.rotation.z);
    m_gpuInsts[base + 11u] = VkFloatBits(inst.rotation.w);
    m_gpuInsts[base + 12u] = VkFloatBits(inst.scale.x);
    m_gpuInsts[base + 13u] = VkFloatBits(inst.scale.y);
    m_gpuInsts[base + 14u] = VkFloatBits(inst.scale.z);
    m_gpuInsts[base + 15u] = 0u;
    m_gpuInsts[base + 16u] = 0u;
    m_gpuInsts[base + 17u] = 0u;
    m_gpuInsts[base + 18u] = 0u;
    m_gpuInsts[base + 19u] = 0u;
    m_gpuInsts[base + 20u] = 0u;
    m_gpuInsts[base + 21u] = 0u;
    m_gpuInsts[base + 22u] = 0u;
    m_gpuInsts[base + 23u] = 0u;
  }

  m_gpuLights.clear();
  m_gpuLights.resize(std::max<std::size_t>(kVulkanLightStrideFloats,
                                           m_sceneData.lights.size() * kVulkanLightStrideFloats),
                     0.0f);
  for (std::size_t i = 0u; i < m_sceneData.lights.size(); ++i) {
    const auto& lt = m_sceneData.lights[i];
    const std::size_t base = i * kVulkanLightStrideFloats;
    m_gpuLights[base + 0u] = lt.position.x;
    m_gpuLights[base + 1u] = lt.position.y;
    m_gpuLights[base + 2u] = lt.position.z;
    m_gpuLights[base + 3u] = lt.color.x;
    m_gpuLights[base + 4u] = lt.color.y;
    m_gpuLights[base + 5u] = lt.color.z;
    m_gpuLights[base + 6u] = lt.intensity;
    m_gpuLights[base + 7u] = std::max(0.0f, lt.radius);
    m_gpuLights[base + 8u] = lt.direction.x;
    m_gpuLights[base + 9u] = lt.direction.y;
    m_gpuLights[base + 10u] = lt.direction.z;
    m_gpuLights[base + 11u] = lt.spot_inner_cos;
    m_gpuLights[base + 12u] = lt.spot_outer_cos;
    m_gpuLights[base + 13u] = 0.0f;
    m_gpuLights[base + 14u] = 0.0f;
    m_gpuLights[base + 15u] = 0.0f;
  }

  m_gpuSdfs.clear();
  m_gpuSdfs.resize(std::max<std::size_t>(kVulkanSdfStrideFloats,
                                         m_sceneData.sdf_primitives.size() * kVulkanSdfStrideFloats),
                   0.0f);
  for (std::size_t i = 0u; i < m_sceneData.sdf_primitives.size(); ++i) {
    const auto& sdf = m_sceneData.sdf_primitives[i];
    const std::size_t base = i * kVulkanSdfStrideFloats;
    m_gpuSdfs[base + 0u] = static_cast<float>(sdf.shape);
    m_gpuSdfs[base + 1u] = static_cast<float>(sdf.material_index);
    m_gpuSdfs[base + 2u] = sdf.radius;
    m_gpuSdfs[base + 3u] = sdf.param_a;
    m_gpuSdfs[base + 4u] = sdf.position.x;
    m_gpuSdfs[base + 5u] = sdf.position.y;
    m_gpuSdfs[base + 6u] = sdf.position.z;
    m_gpuSdfs[base + 7u] = sdf.param_b;
    m_gpuSdfs[base + 8u] = sdf.scale.x;
    m_gpuSdfs[base + 9u] = sdf.scale.y;
    m_gpuSdfs[base + 10u] = sdf.scale.z;
    m_gpuSdfs[base + 11u] = 0.0f;
    m_gpuSdfs[base + 12u] = sdf.rotation.x;
    m_gpuSdfs[base + 13u] = sdf.rotation.y;
    m_gpuSdfs[base + 14u] = sdf.rotation.z;
    m_gpuSdfs[base + 15u] = 0.0f;
  }

  m_gpuEnvMeta = {
    m_sceneData.environment_map_width,
    m_sceneData.environment_map_height,
    0u,
    0u,
  };
  m_gpuEnv.clear();
  const uint64_t envPixelCount =
      static_cast<uint64_t>(m_sceneData.environment_map_width) *
      static_cast<uint64_t>(m_sceneData.environment_map_height);
  if (envPixelCount > 0u &&
      envPixelCount <= static_cast<uint64_t>(std::numeric_limits<std::size_t>::max() / 3u) &&
      m_sceneData.environment_map.size() >= static_cast<std::size_t>(envPixelCount)) {
    const auto scale = m_sceneData.environment_map_scale;
    m_gpuEnv.resize(static_cast<std::size_t>(envPixelCount) * 3u);
    for (std::size_t i = 0u; i < static_cast<std::size_t>(envPixelCount); ++i) {
      const auto& texel = m_sceneData.environment_map[i];
      const std::size_t base = i * 3u;
      m_gpuEnv[base + 0u] = texel.x * scale.x;
      m_gpuEnv[base + 1u] = texel.y * scale.y;
      m_gpuEnv[base + 2u] = texel.z * scale.z;
    }
    m_gpuEnvMeta[2u] = 1u;
  } else {
    m_gpuEnv.assign(3u, 0.0f);
    m_gpuEnvMeta = {0u, 0u, 0u, 0u};
  }

  // Upload with HOST_VISIBLE | HOST_COHERENT buffers. This favors simple
  // backend behavior over peak throughput; no staging queue is needed.
  destroy_scene_buffers();

  struct Upload { const void* data; VkDeviceSize sz; VkBuffer* buf; VkDeviceMemory* mem; };
  const Upload ups[] = {
    {m_gpuVerts.data(),  m_gpuVerts.size()  * sizeof(float),    &m_vertBuf, &m_vertMem},
    {m_gpuIdx.data(),    m_gpuIdx.size()    * sizeof(uint32_t), &m_idxBuf,  &m_idxMem},
    {m_gpuMats.data(),   m_gpuMats.size()   * sizeof(float),    &m_matBuf,  &m_matMem},
    {m_gpuInsts.data(),  m_gpuInsts.size()  * sizeof(uint32_t), &m_instBuf, &m_instMem},
    {m_gpuLights.data(), m_gpuLights.size() * sizeof(float),    &m_ltBuf,   &m_ltMem},
    {m_gpuSdfs.data(),   m_gpuSdfs.size()   * sizeof(float),    &m_sdfBuf,  &m_sdfMem},
    {m_gpuEnv.data(),    m_gpuEnv.size()    * sizeof(float),    &m_envBuf,  &m_envMem},
    {m_gpuEnvMeta.data(), m_gpuEnvMeta.size() * sizeof(uint32_t), &m_envMetaBuf, &m_envMetaMem},
  };
  for (const auto& u : ups) {
    if (!make_buffer(u.sz,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     *u.buf, *u.mem)) return false;
    void* p = nullptr;
    VK_CHK(vkMapMemory(m_device, *u.mem, 0, u.sz, 0, &p));
    std::memcpy(p, u.data, static_cast<std::size_t>(u.sz));
    vkUnmapMemory(m_device, *u.mem);
  }
  m_sceneUploaded = true;
  publish_device_memory_telemetry();

  // Rebind descriptors
  if (m_ds != VK_NULL_HANDLE) {
    vkFreeDescriptorSets(m_device, m_dsPool, 1, &m_ds);
    m_ds = VK_NULL_HANDLE;
  }
  return create_descriptors();
}

bool VulkanGpuPathTracer::reset_accumulation() {
  if (!m_configured || !m_filmPtr) return false;
  std::memset(m_filmPtr, 0,
              static_cast<std::size_t>(m_filmPixels) * 4u * sizeof(float));
  m_film.clear();
  m_cpuFilmDirty = false;
  m_counters = {};
  m_latestFilmReadbackToken = {};
  m_currentSample = 0u;
  ++m_accumulationGeneration;
  debug_check_state_contract("reset_accumulation");
  return true;
}

vkpt::core::Status VulkanGpuPathTracer::reset_accumulation_status() {
  const bool ok = reset_accumulation();
  return GpuBackendOperationStatus(
      "vulkan.reset_accumulation",
      ok,
      m_error,
      vkpt::core::StatusCode::NotReady);
}

bool VulkanGpuPathTracer::update_camera(const vkpt::pathtracer::Vec3& pos,
                                        const vkpt::pathtracer::Vec3& target,
                                        const vkpt::pathtracer::Vec3& up,
                                        float fov_deg) {
  if (!m_sceneUploaded) {
    return false;
  }
  auto camera = vkpt::pathtracer::ExtractCameraState(m_sceneData);
  camera.position = pos;
  camera.target = target;
  camera.up = up;
  camera.fov_deg = fov_deg;
  return update_camera_state(camera);
}

vkpt::core::Status VulkanGpuPathTracer::update_camera_status(
    const vkpt::pathtracer::Vec3& pos,
    const vkpt::pathtracer::Vec3& target,
    const vkpt::pathtracer::Vec3& up,
    float fov_deg) {
  const bool ok = update_camera(pos, target, up, fov_deg);
  return GpuBackendOperationStatus(
      "vulkan.update_camera",
      ok,
      m_error,
      vkpt::core::StatusCode::NotReady);
}

bool VulkanGpuPathTracer::update_camera_state(
    const vkpt::pathtracer::RTCameraState& camera) {
  if (!m_sceneUploaded) {
    return false;
  }
  vkpt::pathtracer::ApplyCameraState(m_sceneData, camera);
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData));
  m_cpuFilmDirty = false;
  debug_check_state_contract("update_camera_state");
  return true;
}

vkpt::core::Status VulkanGpuPathTracer::update_camera_state_status(
    const vkpt::pathtracer::RTCameraState& camera) {
  const bool ok = update_camera_state(camera);
  return GpuBackendOperationStatus(
      "vulkan.update_camera_state",
      ok,
      m_error,
      vkpt::core::StatusCode::NotReady);
}

bool VulkanGpuPathTracer::update_instance_transforms(
    const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) {
  if (!m_sceneUploaded || updates.empty()) {
    return false;
  }
  auto nextScene = m_sceneData;
  for (const auto& update : updates) {
    uint32_t instanceIndex = update.instance_index;
    if (instanceIndex >= nextScene.instances.size() && update.entity_id != 0u) {
      for (std::size_t index = 0u; index < nextScene.instances.size(); ++index) {
        if (nextScene.instances[index].entity_id == update.entity_id) {
          instanceIndex = static_cast<uint32_t>(index);
          break;
        }
      }
    }
    if (instanceIndex >= nextScene.instances.size()) {
      return false;
    }
    const auto& instance = nextScene.instances[instanceIndex];
    if (!instance.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform) ||
        instance.local_vertex_count == 0u ||
        instance.local_index_count < 3u) {
      return false;
    }
  }
  if (!vkpt::pathtracer::ApplyInstanceTransformUpdates(nextScene, updates)) {
    return false;
  }

  auto previousScene = m_sceneData;
  m_sceneData = std::move(nextScene);
  if (!recreate_scene_buffers_from_snapshot()) {
    m_sceneData = std::move(previousScene);
    (void)recreate_scene_buffers_from_snapshot();
    return false;
  }
  debug_check_state_contract("update_instance_transforms");
  return true;
}

vkpt::core::Status VulkanGpuPathTracer::update_instance_transforms_status(
    const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) {
  const bool ok = update_instance_transforms(updates);
  return GpuBackendOperationStatus(
      "vulkan.update_instance_transforms",
      ok,
      m_error,
      vkpt::core::StatusCode::NotReady);
}

vkpt::pathtracer::InstanceTransformPlan VulkanGpuPathTracer::plan_instance_transform_update(
    std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
    const vkpt::pathtracer::InstanceTransformUpdateOptions& /*options*/) const {
  if (!m_sceneUploaded || updates.empty()) {
    return {
        vkpt::pathtracer::InstanceTransformUpdateStatus::Unsupported,
        static_cast<std::uint32_t>(updates.size()),
        0u,
        "Vulkan scene buffers are not ready for transform updates"};
  }

  std::uint32_t matched = 0u;
  for (const auto& update : updates) {
    uint32_t instanceIndex = update.instance_index;
    if (instanceIndex >= m_sceneData.instances.size() && update.entity_id != 0u) {
      for (std::size_t index = 0u; index < m_sceneData.instances.size(); ++index) {
        if (m_sceneData.instances[index].entity_id == update.entity_id) {
          instanceIndex = static_cast<uint32_t>(index);
          break;
        }
      }
    }
    if (instanceIndex >= m_sceneData.instances.size()) {
      continue;
    }
    const auto& instance = m_sceneData.instances[instanceIndex];
    if (!instance.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform) ||
        instance.local_vertex_count == 0u ||
        instance.local_index_count < 3u) {
      return {
          vkpt::pathtracer::InstanceTransformUpdateStatus::BlockedNeedsFullStaticAccelRebuild,
          static_cast<std::uint32_t>(updates.size()),
          matched,
          "Vulkan transform update requires a full scene buffer rebuild"};
    }
    ++matched;
  }

  if (matched == 0u) {
    return {
        vkpt::pathtracer::InstanceTransformUpdateStatus::Failed,
        static_cast<std::uint32_t>(updates.size()),
        0u,
        "Vulkan transform update matched no instances"};
  }

  return {
      vkpt::pathtracer::InstanceTransformUpdateStatus::BlockedNeedsFullStaticAccelRebuild,
      static_cast<std::uint32_t>(updates.size()),
      matched,
      "Vulkan transform update is valid but requires scene buffer rebuild"};
}

vkpt::pathtracer::InstanceTransformUpdateResult VulkanGpuPathTracer::apply_instance_transform_update(
    std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
    const vkpt::pathtracer::InstanceTransformUpdateOptions& options) {
  const auto plan = plan_instance_transform_update(updates, options);
  if (!vkpt::pathtracer::TransformUpdateStatusAllowedByPolicy(plan.status,
                                                              options.fallback_policy)) {
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
  const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updateVec(updates.begin(), updates.end());
  if (!update_instance_transforms(updateVec)) {
    return {
        vkpt::pathtracer::InstanceTransformUpdateStatus::Failed,
        static_cast<std::uint32_t>(updates.size()),
        0u,
        0.0,
        0.0,
        0.0,
        0.0,
        "Vulkan transform update rebuild failed"};
  }
  return {
      vkpt::pathtracer::InstanceTransformUpdateStatus::AppliedFullStaticAccelRebuild,
      static_cast<std::uint32_t>(updates.size()),
      plan.matched_count,
      0.0,
      0.0,
      0.0,
      0.0,
      "Vulkan transform update rebuilt scene buffers"};
}

bool VulkanGpuPathTracer::update_scene_delta(
    const vkpt::pathtracer::RTSceneDeltaUpdate& update) {
  if (!m_sceneUploaded) {
    return false;
  }
  if (!vkpt::pathtracer::ApplySceneDeltaUpdate(m_sceneData, update)) {
    return false;
  }
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData));
  const bool ok = recreate_scene_buffers_from_snapshot();
  if (ok) {
    debug_check_state_contract("update_scene_delta");
  }
  return ok;
}

vkpt::core::Status VulkanGpuPathTracer::update_scene_delta_status(
    const vkpt::pathtracer::RTSceneDeltaUpdate& update) {
  const bool ok = update_scene_delta(update);
  return GpuBackendOperationStatus(
      "vulkan.update_scene_delta",
      ok,
      m_error,
      vkpt::core::StatusCode::NotReady);
}

bool VulkanGpuPathTracer::render_tile(
    const vkpt::pathtracer::RenderTile& tile,
    uint32_t frame_idx) {
  if (!m_valid || !m_configured || !m_sceneUploaded || !m_filmPtr) {
    EmitGpuBackendAnomaly("vulkan", "render_tile", "backend not ready", introspect());
    return false;
  }
  if (m_pipeline == VK_NULL_HANDLE || m_ds == VK_NULL_HANDLE) {
    EmitGpuBackendAnomaly("vulkan", "render_tile", "pipeline or descriptor set missing", introspect());
    return false;
  }
  if (tile.width == 0u || tile.height == 0u) {
    return true;
  }
  if (tile.x != 0u || tile.width < m_settings.width) {
    m_error = "Vulkan backend only supports full-width row tiles";
    EmitGpuBackendAnomaly("vulkan", "render_tile", m_error, introspect());
    return false;
  }
  const uint32_t sample_idx = tile.sample_index;

  const auto& sc = m_sceneData;

  // Camera basis — matches ScalarCpuPathTracer::build_or_update_acceleration
  auto norm3 = [](float x, float y, float z, float out[3]) {
    float l = std::sqrt(x*x + y*y + z*z);
    if (l < 1e-9f) l = 1.0f;
    out[0] = x/l; out[1] = y/l; out[2] = z/l;
  };
  float fwd[3];
  norm3(sc.camera_target.x - sc.camera_position.x,
        sc.camera_target.y - sc.camera_position.y,
        sc.camera_target.z - sc.camera_position.z, fwd);
  // right = cross(forward, camera_up)
  float rt[3] = {
    fwd[1]*sc.camera_up.z - fwd[2]*sc.camera_up.y,
    fwd[2]*sc.camera_up.x - fwd[0]*sc.camera_up.z,
    fwd[0]*sc.camera_up.y - fwd[1]*sc.camera_up.x
  };
  float rn[3]; norm3(rt[0], rt[1], rt[2], rn);
  // up = cross(right, forward)
  float un[3]; norm3(rn[1]*fwd[2]-rn[2]*fwd[1],
                     rn[2]*fwd[0]-rn[0]*fwd[2],
                     rn[0]*fwd[1]-rn[1]*fwd[0], un);

  PathTracePushConstants pc{};
  pc.camera_pos[0] = sc.camera_position.x;
  pc.camera_pos[1] = sc.camera_position.y;
  pc.camera_pos[2] = sc.camera_position.z;
  pc.fov_tan_half  = std::tan(0.5f * sc.camera_fov_deg * 3.14159265f / 180.0f);
  pc.cam_forward[0]=fwd[0]; pc.cam_forward[1]=fwd[1]; pc.cam_forward[2]=fwd[2];
  pc.aspect        = static_cast<float>(m_settings.width) /
                     std::max(1.0f, static_cast<float>(m_settings.height));
  pc.cam_right[0]  = rn[0];  pc.cam_right[1]  = rn[1];  pc.cam_right[2]  = rn[2];
  pc.num_sdfs      = static_cast<uint32_t>(sc.sdf_primitives.size());
  pc.cam_up[0]     = un[0];  pc.cam_up[1]     = un[1];  pc.cam_up[2]     = un[2];
  pc.sample_index  = sample_idx;
  pc.num_insts     = static_cast<uint32_t>(sc.instances.size());
  pc.num_mats      = static_cast<uint32_t>(sc.materials.size());
  pc.num_lights    = static_cast<uint32_t>(sc.lights.size());
  pc.width         = m_settings.width;
  pc.height        = m_settings.height;
  pc.base_seed     = static_cast<uint32_t>(m_settings.seed & 0xFFFFFFFFu)
                     ^ (frame_idx * 2654435761u);
  pc.env_r         = sc.environment_color.x;
  pc.env_g         = sc.environment_color.y;
  pc.env_b         = sc.environment_color.z;
  pc.max_depth_f   = static_cast<float>(std::max(1u, m_settings.max_depth));
  pc.aperture_radius = sc.camera_aperture_radius > 0.0f
      ? sc.camera_aperture_radius
      : m_settings.camera_aperture_radius;
  pc.focus_distance = sc.camera_focus_distance > 0.0f
      ? sc.camera_focus_distance
      : m_settings.camera_focus_distance;
  pc.iris_blade_count = std::min(sc.camera_iris_blade_count, 64u);
  pc.iris_rotation_radians = sc.camera_iris_rotation_degrees * (3.14159265f / 180.0f);
  pc.iris_roundness = std::clamp(sc.camera_iris_roundness, 0.0f, 1.0f);
  pc.anamorphic_squeeze = std::isfinite(sc.camera_anamorphic_squeeze)
      ? std::max(0.01f, sc.camera_anamorphic_squeeze)
      : 1.0f;

  const uint32_t maxY = std::min(tile.y + tile.height, m_settings.height);
  const uint32_t minY = std::min(tile.y, maxY);
  if (minY >= maxY) {
    return true;
  }
  if ((minY % kVulkanWorkgroupSizeY) != 0u ||
      ((maxY % kVulkanWorkgroupSizeY) != 0u && maxY != m_settings.height)) {
    m_error = "Vulkan row tile dispatch requires 8-row alignment except at the image bottom";
    EmitGpuBackendAnomaly("vulkan", "render_tile", m_error, introspect());
    return false;
  }
  if (m_cmdBufs.empty() || m_timelineSemaphore == VK_NULL_HANDLE) {
    m_error = "Vulkan timeline command ring is not initialized";
    EmitGpuBackendAnomaly("vulkan", "render_tile", m_error, introspect());
    return false;
  }

  // Queue row tiles through a reusable command-buffer ring. We only wait when a
  // slot is about to be reused, then once more on the final timeline value so
  // resolve_ldr/resolve_hdr keep the existing immediate-readback contract.
  std::vector<uint64_t> slotTimeline(m_cmdBufs.size(), m_completedTimelineValue);
  uint64_t lastSubmitted = m_completedTimelineValue;
  std::size_t slot = 0u;

  for (uint32_t tileY = minY; tileY < maxY;) {
    const uint32_t tileRows = std::min<uint32_t>(kVulkanTileHeightRows, maxY - tileY);
    const uint32_t tileEnd = tileY + tileRows;
    if (!wait_for_timeline(slotTimeline[slot])) {
      return false;
    }
    VkCommandBuffer cmdBuf = m_cmdBufs[slot];
    if (!record_tile_dispatch(cmdBuf, pc, tileY, tileEnd)) {
      if (lastSubmitted > m_completedTimelineValue) {
        (void)wait_for_timeline(lastSubmitted);
      }
      return false;
    }
    const uint32_t groupsX =
        (m_settings.width + kVulkanWorkgroupSizeX - 1u) / kVulkanWorkgroupSizeX;
    const uint32_t groupsY =
        (tileEnd - tileY + kVulkanWorkgroupSizeY - 1u) / kVulkanWorkgroupSizeY;
    const uint64_t signalValue = ++m_timelineValue;
    if (!submit_tile_dispatch(cmdBuf, signalValue, groupsX, groupsY)) {
      if (lastSubmitted > m_completedTimelineValue) {
        (void)wait_for_timeline(lastSubmitted);
      }
      return false;
    }
    slotTimeline[slot] = signalValue;
    lastSubmitted = signalValue;
    slot = (slot + 1u) % m_cmdBufs.size();
    tileY = tileEnd;
  }

  if (!wait_for_timeline(lastSubmitted)) {
    return false;
  }

  m_cpuFilmDirty = true;

  const uint64_t sampleInc =
      static_cast<uint64_t>(m_settings.width) * (maxY - minY);
  m_counters.samples += sampleInc;
  m_counters.rays    += sampleInc;
  m_currentSample = std::max(m_currentSample, sample_idx + 1u);
  debug_check_state_contract("render_tile");
  return true;
}

vkpt::core::Status VulkanGpuPathTracer::render_tile_status(
    const vkpt::pathtracer::RenderTile& tile,
    uint32_t frame_idx) {
  const bool ok = render_tile(tile, frame_idx);
  return GpuBackendOperationStatus(
      "vulkan.render_tile",
      ok,
      m_error,
      vkpt::core::StatusCode::NotReady);
}

vkpt::pathtracer::FilmLdr VulkanGpuPathTracer::resolve_ldr() const {
  rebuild_cpu_film_from_gpu();
  return m_film.resolve_ldr();
}
vkpt::pathtracer::FilmHdr VulkanGpuPathTracer::resolve_hdr() const {
  rebuild_cpu_film_from_gpu();
  return m_film.resolve_hdr();
}
vkpt::pathtracer::FilmReadbackToken VulkanGpuPathTracer::request_film_readback() {
  if (!m_valid || !m_configured || !m_sceneUploaded || !m_filmPtr) {
    return {};
  }
  vkpt::pathtracer::FilmReadbackToken token;
  token.id = m_nextFilmReadbackId++;
  token.fence_value = m_timelineValue;
  token.width = m_settings.width;
  token.height = m_settings.height;
  m_latestFilmReadbackToken = token;
  return token;
}
vkpt::pathtracer::FilmReadbackResult VulkanGpuPathTracer::poll_film(
    vkpt::pathtracer::FilmReadbackToken token) {
  if (!token || token.id != m_latestFilmReadbackToken.id ||
      token.width != m_settings.width || token.height != m_settings.height) {
    return {
        vkpt::pathtracer::FilmReadbackState::Invalid,
        {},
        "invalid Vulkan film readback token"};
  }
  if (!m_valid || !m_configured || !m_sceneUploaded || !m_filmPtr) {
    return {
        vkpt::pathtracer::FilmReadbackState::Failed,
        {},
        "Vulkan film readback requested before backend is ready"};
  }
  if (token.fence_value > m_completedTimelineValue) {
    if (!refresh_completed_timeline()) {
      return {
          vkpt::pathtracer::FilmReadbackState::Failed,
          {},
          m_error};
    }
    if (token.fence_value > m_completedTimelineValue) {
      return {vkpt::pathtracer::FilmReadbackState::Pending, {}, {}};
    }
  }
  rebuild_cpu_film_from_gpu();
  return {
      vkpt::pathtracer::FilmReadbackState::Ready,
      m_film.resolve_ldr(),
      {}};
}
vkpt::pathtracer::SampleCounters VulkanGpuPathTracer::read_counters() const {
  return m_counters;
}

vkpt::pathtracer::PathTracerStatus VulkanGpuPathTracer::status() const {
  vkpt::pathtracer::PathTracerStatus out;
  out.backend = "vulkan";
  out.lifecycle = LifecycleFromGpuState(m_valid,
                                        m_configured,
                                        m_hasScene,
                                        m_sceneUploaded,
                                        m_error);
  out.scene_loaded = m_hasScene;
  out.accel_valid = m_sceneUploaded;
  out.ready_to_render = m_valid && m_configured && m_hasScene && m_sceneUploaded &&
                        m_filmPtr != nullptr && m_pipeline != VK_NULL_HANDLE &&
                        m_ds != VK_NULL_HANDLE;
  out.current_sample = m_currentSample;
  out.last_sample_us = m_lastSubmitUs + m_lastFenceWaitUs;
  out.total_samples = m_counters.samples;
  out.accumulation_gen = m_accumulationGeneration;
  out.last_error = m_error;
  return out;
}

void VulkanGpuPathTracer::shutdown() {
  if (m_device != VK_NULL_HANDLE) {
    EmitGpuBackendStopped("vulkan", introspect());
    // Ensure no queued command buffer still references resources being torn down.
    vkDeviceWaitIdle(m_device);

    if (m_ds != VK_NULL_HANDLE) {
      vkFreeDescriptorSets(m_device, m_dsPool, 1, &m_ds);  m_ds = VK_NULL_HANDLE;
    }
    destroy_film_buffers();
    destroy_scene_buffers();
    destroy_pipeline();

    if (m_dsPool   != VK_NULL_HANDLE) { vkDestroyDescriptorPool(m_device, m_dsPool, nullptr);             m_dsPool   = VK_NULL_HANDLE; }
    if (m_dsLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(m_device, m_dsLayout, nullptr);      m_dsLayout = VK_NULL_HANDLE; }
    if (m_timelineSemaphore != VK_NULL_HANDLE) { vkDestroySemaphore(m_device, m_timelineSemaphore, nullptr); m_timelineSemaphore = VK_NULL_HANDLE; }
    if (m_cmdPool  != VK_NULL_HANDLE) { vkDestroyCommandPool(m_device, m_cmdPool, nullptr);               m_cmdPool  = VK_NULL_HANDLE; }
    m_cmdBufs.clear();
    m_timelineValue = 0u;
    m_completedTimelineValue = 0u;
    m_pendingDispatches = 0u;
    m_dispatchesSubmitted = 0u;
    m_dispatchesCompleted = 0u;
    m_lastSubmitUs = 0u;
    m_lastFenceWaitUs = 0u;
    vkDestroyDevice(m_device, nullptr);
    m_device = VK_NULL_HANDLE;
  }
  if (m_instance != VK_NULL_HANDLE) { vkDestroyInstance(m_instance, nullptr);                           m_instance = VK_NULL_HANDLE; }
  m_valid = false;
  m_cpuFilmDirty = false;
  m_latestFilmReadbackToken = {};
}

// ============================================================================
// Private helpers
// ============================================================================

void VulkanGpuPathTracer::debug_check_state_contract(const char* operation) const {
  (void)operation;
#ifndef NDEBUG
  assert(!m_sceneUploaded || m_hasScene);
  assert(!m_hasScene || m_configured);
  assert(!m_configured || m_valid);
  const bool ready = m_valid && m_configured && m_hasScene && m_sceneUploaded;
  if (ready) {
    assert(m_filmPtr != nullptr);
    assert(m_pipeline != VK_NULL_HANDLE);
    assert(m_ds != VK_NULL_HANDLE);
  }
#endif
}

vkpt::core::Status VulkanGpuPathTracer::init_device() {
  // Instance (no surface extensions needed for off-screen compute)
  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  ai.pApplicationName   = "vkPathTracer";
  ai.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  ai.apiVersion         = VK_API_VERSION_1_2;

  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &ai;
  VK_STATUS_CHK(vkCreateInstance(&ici, nullptr, &m_instance));

  // Physical device — prefer discrete
  uint32_t cnt = 0;
  VK_STATUS_CHK(vkEnumeratePhysicalDevices(m_instance, &cnt, nullptr));
  if (cnt == 0) {
    m_error = "No Vulkan physical devices";
    return vkpt::core::Status::error(vkpt::core::StatusCode::NotReady, m_error);
  }
  std::vector<VkPhysicalDevice> devs(cnt);
  VK_STATUS_CHK(vkEnumeratePhysicalDevices(m_instance, &cnt, devs.data()));

  // Enumerate all physical devices and log them to help verify the right GPU
  // is selected.  We prefer DISCRETE_GPU over INTEGRATED_GPU.
  auto deviceTypeName = [](VkPhysicalDeviceType t) -> const char* {
    switch (t) {
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "DISCRETE";
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "INTEGRATED";
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "VIRTUAL";
      case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
      default:                                      return "OTHER";
    }
  };

  m_physDev = devs[0];
  for (uint32_t i = 0; i < devs.size(); ++i) {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(devs[i], &p);

    // Sum DEVICE_LOCAL heap sizes to get VRAM estimate
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(devs[i], &mp);
    uint64_t vramBytes = 0;
    for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
      if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        vramBytes += mp.memoryHeaps[h].size;
    const uint32_t vramMb = static_cast<uint32_t>(vramBytes / (1024u * 1024u));
    const uint32_t apiMaj = VK_VERSION_MAJOR(p.apiVersion);
    const uint32_t apiMin = VK_VERSION_MINOR(p.apiVersion);

    std::ostringstream ds;
    ds << "  [gpu" << i << "] " << p.deviceName
       << "  type=" << deviceTypeName(p.deviceType)
       << "  api=" << apiMaj << "." << apiMin
       << "  vram=" << vramMb << " MB"
       << (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "  <-- DISCRETE (preferred)" : "");
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Info, "vulkan", ds.str());

    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
      m_physDev = devs[i];   // last discrete wins; keeps highest-perf GPU
  }

  // Confirm selected device
  {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(m_physDev, &p);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(m_physDev, &mp);
    uint64_t vramBytes = 0;
    for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
      if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        vramBytes += mp.memoryHeaps[h].size;

    m_gpuName    = p.deviceName;
    m_vramMb     = static_cast<uint32_t>(vramBytes / (1024u * 1024u));
    m_deviceMemoryLimitBytes = vramBytes;
    m_apiVersion = p.apiVersion;
    m_gpuType    = deviceTypeName(p.deviceType);

    std::ostringstream sel;
    sel << "Selected GPU: " << m_gpuName
        << "  type=" << m_gpuType
        << "  api=" << VK_VERSION_MAJOR(m_apiVersion) << "." << VK_VERSION_MINOR(m_apiVersion)
        << "  vram=" << m_vramMb << " MB";
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Info, "vulkan", sel.str());
  }

  // Compute queue family
  uint32_t qfCnt = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(m_physDev, &qfCnt, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qfCnt);
  vkGetPhysicalDeviceQueueFamilyProperties(m_physDev, &qfCnt, qf.data());
  m_qFam = UINT32_MAX;
  for (uint32_t i = 0; i < qfCnt; ++i)
    if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { m_qFam = i; break; }
  if (m_qFam == UINT32_MAX) {
    m_error = "No compute queue family";
    return vkpt::core::Status::error(vkpt::core::StatusCode::Unsupported, m_error);
  }
  if (m_apiVersion < VK_API_VERSION_1_2) {
    m_error = "Selected Vulkan device must expose Vulkan 1.2 for timeline submissions";
    return vkpt::core::Status::error(vkpt::core::StatusCode::Unsupported, m_error);
  }

  VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
  timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.pNext = &timelineFeatures;
  vkGetPhysicalDeviceFeatures2(m_physDev, &features2);
  if (!timelineFeatures.timelineSemaphore) {
    m_error = "Selected Vulkan device does not support timeline semaphores";
    return vkpt::core::Status::error(vkpt::core::StatusCode::Unsupported, m_error);
  }
  timelineFeatures.timelineSemaphore = VK_TRUE;

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = m_qFam;
  qci.queueCount       = 1;
  qci.pQueuePriorities = &prio;

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &timelineFeatures;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos    = &qci;
  VK_STATUS_CHK(vkCreateDevice(m_physDev, &dci, nullptr, &m_device));
  vkGetDeviceQueue(m_device, m_qFam, 0, &m_queue);
  return vkpt::core::Status::ok("Vulkan device initialized");
}

vkpt::core::Status VulkanGpuPathTracer::create_cmd_pool() {
  VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  ci.queueFamilyIndex = m_qFam;
  VK_STATUS_CHK(vkCreateCommandPool(m_device, &ci, nullptr, &m_cmdPool));

  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool        = m_cmdPool;
  ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = kVulkanCommandBufferCount;
  m_cmdBufs.assign(kVulkanCommandBufferCount, VK_NULL_HANDLE);
  VK_STATUS_CHK(vkAllocateCommandBuffers(m_device, &ai, m_cmdBufs.data()));

  VkSemaphoreTypeCreateInfo typeInfo{};
  typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  typeInfo.initialValue = 0u;
  VkSemaphoreCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  sci.pNext = &typeInfo;
  VK_STATUS_CHK(vkCreateSemaphore(m_device, &sci, nullptr, &m_timelineSemaphore));
  m_timelineValue = 0u;
  m_completedTimelineValue = 0u;
  m_pendingDispatches = 0u;
  m_dispatchesSubmitted = 0u;
  m_dispatchesCompleted = 0u;
  m_lastSubmitUs = 0u;
  m_lastFenceWaitUs = 0u;
  m_recentFenceTimeout = false;
  m_recentDeviceLost = false;
  return vkpt::core::Status::ok("Vulkan command pool initialized");
}

bool VulkanGpuPathTracer::record_tile_dispatch(
    VkCommandBuffer cmd_buf,
    const PathTracePushConstants& pc,
    uint32_t start_y,
    uint32_t end_y) {
  if (cmd_buf == VK_NULL_HANDLE || start_y >= end_y) {
    return false;
  }
  if ((start_y % kVulkanWorkgroupSizeY) != 0u ||
      ((end_y % kVulkanWorkgroupSizeY) != 0u && end_y != m_settings.height)) {
    m_error = "Vulkan row tile dispatch is not workgroup aligned";
    return false;
  }

  const uint32_t dispatchX =
      (m_settings.width + kVulkanWorkgroupSizeX - 1u) / kVulkanWorkgroupSizeX;
  const uint32_t dispatchY =
      (end_y - start_y + kVulkanWorkgroupSizeY - 1u) / kVulkanWorkgroupSizeY;
  if (dispatchX == 0u || dispatchY == 0u) {
    return false;
  }

  VK_CHK(vkResetCommandBuffer(cmd_buf, 0));
  VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHK(vkBeginCommandBuffer(cmd_buf, &cbi));

  vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
  vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_pipeLayout, 0, 1, &m_ds, 0, nullptr);
  vkCmdPushConstants(cmd_buf, m_pipeLayout,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(PathTracePushConstants), &pc);
  vkCmdDispatchBase(cmd_buf,
                    0u, start_y / kVulkanWorkgroupSizeY, 0u,
                    dispatchX, dispatchY, 1u);
  VK_CHK(vkEndCommandBuffer(cmd_buf));
  return true;
}

bool VulkanGpuPathTracer::submit_tile_dispatch(
    VkCommandBuffer cmd_buf,
    uint64_t signal_value,
    uint32_t groups_x,
    uint32_t groups_y) {
  if (m_timelineSemaphore == VK_NULL_HANDLE || cmd_buf == VK_NULL_HANDLE) {
    m_error = "Vulkan timeline submission is not initialized";
    return false;
  }

  VkTimelineSemaphoreSubmitInfo timelineSubmit{};
  timelineSubmit.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
  timelineSubmit.signalSemaphoreValueCount = 1u;
  timelineSubmit.pSignalSemaphoreValues = &signal_value;

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.pNext = &timelineSubmit;
  si.commandBufferCount = 1u;
  si.pCommandBuffers = &cmd_buf;
  si.signalSemaphoreCount = 1u;
  si.pSignalSemaphores = &m_timelineSemaphore;
  const auto submitStart = std::chrono::steady_clock::now();
  VK_CHK(vkQueueSubmit(m_queue, 1u, &si, VK_NULL_HANDLE));
  m_lastSubmitUs = ElapsedUs(submitStart);
  ++m_dispatchesSubmitted;
  ++m_pendingDispatches;
  VKP_LOG_SAMPLED(kTelemetrySamplePeriodNs,
                  Info,
                  "gpu",
                  "dispatch_submitted",
                  "backend",
                  "vulkan",
                  "groups_x",
                  static_cast<uint64_t>(groups_x),
                  "groups_y",
                  static_cast<uint64_t>(groups_y),
                  "timeline",
                  signal_value,
                  "submit_us",
                  m_lastSubmitUs,
                  "pending",
                  static_cast<uint64_t>(m_pendingDispatches));
  return true;
}

bool VulkanGpuPathTracer::wait_for_timeline(uint64_t value) {
  if (value <= m_completedTimelineValue) {
    return true;
  }
  if (m_timelineSemaphore == VK_NULL_HANDLE) {
    m_error = "Vulkan timeline wait requested before semaphore creation";
    return false;
  }
  VkSemaphoreWaitInfo waitInfo{};
  waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
  waitInfo.semaphoreCount = 1u;
  waitInfo.pSemaphores = &m_timelineSemaphore;
  waitInfo.pValues = &value;
  const uint64_t previousCompleted = m_completedTimelineValue;
  const auto waitStart = std::chrono::steady_clock::now();
  const VkResult waitResult =
      vkWaitSemaphores(m_device, &waitInfo, static_cast<uint64_t>(10e9));
  if (waitResult != VK_SUCCESS) {
    m_recentFenceTimeout = waitResult == VK_TIMEOUT;
    m_recentDeviceLost = waitResult == VK_ERROR_DEVICE_LOST;
    std::ostringstream ss;
    ss << "vkWaitSemaphores failed: VkResult=" << static_cast<int>(waitResult);
    m_error = ss.str();
    return false;
  }
  m_recentFenceTimeout = false;
  m_lastFenceWaitUs = ElapsedUs(waitStart);
  VKP_METRIC_OBSERVE("vkp.gpu.fence_wait_us", m_lastFenceWaitUs);
  m_completedTimelineValue = value;
  const uint64_t completedDelta = value > previousCompleted
      ? value - previousCompleted
      : 0u;
  m_dispatchesCompleted += completedDelta;
  const uint32_t completedPending = static_cast<uint32_t>(
      std::min<uint64_t>(static_cast<uint64_t>(m_pendingDispatches), completedDelta));
  m_pendingDispatches -= completedPending;
  VKP_LOG_SAMPLED(kTelemetrySamplePeriodNs,
                  Info,
                  "gpu",
                  "dispatch_completed",
                  "backend",
                  "vulkan",
                  "timeline",
                  value,
                  "fence_wait_us",
                  m_lastFenceWaitUs,
                  "pending",
                  static_cast<uint64_t>(m_pendingDispatches),
                  "completed_total",
                  m_dispatchesCompleted);
  return true;
}

bool VulkanGpuPathTracer::refresh_completed_timeline() {
  if (m_timelineSemaphore == VK_NULL_HANDLE) {
    m_error = "Vulkan timeline refresh requested before semaphore creation";
    return false;
  }
  uint64_t completedValue = 0u;
  const VkResult result =
      vkGetSemaphoreCounterValue(m_device, m_timelineSemaphore, &completedValue);
  if (result != VK_SUCCESS) {
    m_recentDeviceLost = result == VK_ERROR_DEVICE_LOST;
    std::ostringstream ss;
    ss << "vkGetSemaphoreCounterValue failed: VkResult=" << static_cast<int>(result);
    m_error = ss.str();
    return false;
  }
  if (completedValue <= m_completedTimelineValue) {
    return true;
  }
  const uint64_t completedDelta = completedValue - m_completedTimelineValue;
  m_completedTimelineValue = completedValue;
  m_dispatchesCompleted += completedDelta;
  const uint32_t completedPending = static_cast<uint32_t>(
      std::min<uint64_t>(static_cast<uint64_t>(m_pendingDispatches), completedDelta));
  m_pendingDispatches -= completedPending;
  return true;
}

vkpt::core::Status VulkanGpuPathTracer::create_pipeline() {
  // Load SPIR-V from file
  std::ifstream f(m_spvPath, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    m_error = "Cannot open SPIR-V: " + m_spvPath;
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Error, "vulkan", m_error);
    return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument, m_error);
  }
  const auto spvSz = static_cast<std::size_t>(f.tellg());
  if (spvSz == 0u || (spvSz % sizeof(uint32_t)) != 0u) {
    m_error = "Invalid SPIR-V byte size: " + std::to_string(spvSz);
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Error, "vulkan", m_error);
    return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument, m_error);
  }
  f.seekg(0);
  std::vector<uint32_t> spv(spvSz / 4u);
  f.read(reinterpret_cast<char*>(spv.data()), static_cast<std::streamsize>(spvSz));
  if (!f) {
    m_error = "Failed to read SPIR-V: " + m_spvPath;
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Error, "vulkan", m_error);
    return vkpt::core::Status::error(vkpt::core::StatusCode::InternalError, m_error);
  }
  f.close();

  const auto compileStart = std::chrono::steady_clock::now();
  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = spvSz;
  smci.pCode    = spv.data();
  VK_STATUS_CHK(vkCreateShaderModule(m_device, &smci, nullptr, &m_shaderMod));

  // Descriptor set layout: vertex, index, material, instance, light, film, SDF,
  // environment map, and environment metadata buffers. The binding order is
  // fixed by pathtrace.comp.
  VkDescriptorSetLayoutBinding binds[9]{};
  for (uint32_t i = 0; i < 9; ++i) {
    binds[i].binding         = i;
    binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binds[i].descriptorCount = 1;
    binds[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dsli.bindingCount = 9;
  dsli.pBindings    = binds;
  VK_STATUS_CHK(vkCreateDescriptorSetLayout(m_device, &dsli, nullptr, &m_dsLayout));

  // Push constant range
  VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PathTracePushConstants)};

  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount         = 1;
  plci.pSetLayouts            = &m_dsLayout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges    = &pcr;
  VK_STATUS_CHK(vkCreatePipelineLayout(m_device, &plci, nullptr, &m_pipeLayout));

  VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = m_shaderMod;
  stage.pName  = "main";

  VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage  = stage;
  cpci.layout = m_pipeLayout;
  VK_STATUS_CHK(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci,
                                         nullptr, &m_pipeline));
  VKP_LOG(Info,
          "gpu",
          "shader_compiled",
          "backend",
          "vulkan",
          "shader",
          m_spvPath,
          "bytes",
          static_cast<uint64_t>(spvSz),
          "compile_us",
          ElapsedUs(compileStart),
          "entry",
          "main");
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Info, "vulkan",
      "Compute pipeline created from " + m_spvPath);
  return vkpt::core::Status::ok("Vulkan compute pipeline initialized");
}

bool VulkanGpuPathTracer::create_descriptors() {
  // Create descriptor pool on first call
  if (m_dsPool == VK_NULL_HANDLE) {
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 36};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets       = 8;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &ps;
    VK_CHK(vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_dsPool));
  }

  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool     = m_dsPool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts        = &m_dsLayout;
  VK_CHK(vkAllocateDescriptorSets(m_device, &ai, &m_ds));

  // configure() can create the descriptor pool before scene buffers exist. Once
  // scene upload completes, this function is called again to write all bindings.
  if (!m_filmBuf || !m_vertBuf) return true;

  const std::size_t szV  = m_gpuVerts.size()   * sizeof(float);
  const std::size_t szI  = m_gpuIdx.size()     * sizeof(uint32_t);
  const std::size_t szM  = m_gpuMats.size()    * sizeof(float);
  const std::size_t szIn = m_gpuInsts.size()   * sizeof(uint32_t);
  const std::size_t szL  = m_gpuLights.size()  * sizeof(float);
  const std::size_t szF  = static_cast<std::size_t>(m_filmPixels) * 4u * sizeof(float);
  const std::size_t szS  = m_gpuSdfs.size()    * sizeof(float);
  const std::size_t szE  = m_gpuEnv.size()     * sizeof(float);
  const std::size_t szEM = m_gpuEnvMeta.size() * sizeof(uint32_t);

  const VkDescriptorBufferInfo dbi[9] = {
    {m_vertBuf, 0, szV},  {m_idxBuf, 0, szI},
    {m_matBuf,  0, szM},  {m_instBuf, 0, szIn},
    {m_ltBuf,   0, szL},  {m_filmBuf, 0, szF},
    {m_sdfBuf,  0, szS},  {m_envBuf,  0, szE},
    {m_envMetaBuf, 0, szEM},
  };
  VkWriteDescriptorSet writes[9]{};
  for (uint32_t i = 0; i < 9; ++i) {
    writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet          = m_ds;
    writes[i].dstBinding      = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo     = &dbi[i];
  }
  vkUpdateDescriptorSets(m_device, 9, writes, 0, nullptr);
  return true;
}

uint32_t VulkanGpuPathTracer::find_mem_type(uint32_t bits,
                                            VkMemoryPropertyFlags flags) const {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(m_physDev, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((bits & (1u << i)) &&
        (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
  return UINT32_MAX;
}

bool VulkanGpuPathTracer::make_buffer(VkDeviceSize size,
                                      VkBufferUsageFlags usage,
                                      VkMemoryPropertyFlags props,
                                      VkBuffer& buf, VkDeviceMemory& mem) {
  if (size == 0) size = 4;
  // Use local handles until every Vulkan allocation step succeeds so callers do
  // not inherit half-created buffer state on error.
  VkBuffer localBuf = VK_NULL_HANDLE;
  VkDeviceMemory localMem = VK_NULL_HANDLE;
  auto cleanup = [&]() {
    if (localMem != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, localMem, nullptr);
      localMem = VK_NULL_HANDLE;
    }
    if (localBuf != VK_NULL_HANDLE) {
      vkDestroyBuffer(m_device, localBuf, nullptr);
      localBuf = VK_NULL_HANDLE;
    }
  };
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size        = size;
  bci.usage       = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult result = vkCreateBuffer(m_device, &bci, nullptr, &localBuf);
  if (result != VK_SUCCESS) {
    std::ostringstream ss;
    ss << "vkCreateBuffer failed: VkResult=" << static_cast<int>(result);
    m_error = ss.str();
    m_recentDeviceLost = result == VK_ERROR_DEVICE_LOST;
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Error, "vulkan", m_error);
    return false;
  }

  VkMemoryRequirements mr{};
  vkGetBufferMemoryRequirements(m_device, localBuf, &mr);
  const uint32_t mt = find_mem_type(mr.memoryTypeBits, props);
  if (mt == UINT32_MAX) {
    m_error = "No suitable memory type";
    cleanup();
    return false;
  }

  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize  = mr.size;
  mai.memoryTypeIndex = mt;
  result = vkAllocateMemory(m_device, &mai, nullptr, &localMem);
  if (result != VK_SUCCESS) {
    std::ostringstream ss;
    ss << "vkAllocateMemory failed: VkResult=" << static_cast<int>(result);
    m_error = ss.str();
    m_recentDeviceLost = result == VK_ERROR_DEVICE_LOST;
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Error, "vulkan", m_error);
    cleanup();
    return false;
  }
  result = vkBindBufferMemory(m_device, localBuf, localMem, 0);
  if (result != VK_SUCCESS) {
    std::ostringstream ss;
    ss << "vkBindBufferMemory failed: VkResult=" << static_cast<int>(result);
    m_error = ss.str();
    m_recentDeviceLost = result == VK_ERROR_DEVICE_LOST;
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Error, "vulkan", m_error);
    cleanup();
    return false;
  }
  buf = localBuf;
  mem = localMem;
  return true;
}

uint64_t VulkanGpuPathTracer::estimate_device_memory_bytes() const {
  auto buffer_bytes = [](std::size_t bytes, bool live) -> uint64_t {
    if (!live) {
      return 0u;
    }
    return static_cast<uint64_t>(std::max<std::size_t>(bytes, 4u));
  };

  uint64_t total = 0u;
  total += buffer_bytes(static_cast<std::size_t>(m_filmPixels) * 4u * sizeof(float),
                        m_filmBuf != VK_NULL_HANDLE);
  total += buffer_bytes(m_gpuVerts.size() * sizeof(float), m_vertBuf != VK_NULL_HANDLE);
  total += buffer_bytes(m_gpuIdx.size() * sizeof(uint32_t), m_idxBuf != VK_NULL_HANDLE);
  total += buffer_bytes(m_gpuMats.size() * sizeof(float), m_matBuf != VK_NULL_HANDLE);
  total += buffer_bytes(m_gpuInsts.size() * sizeof(uint32_t), m_instBuf != VK_NULL_HANDLE);
  total += buffer_bytes(m_gpuLights.size() * sizeof(float), m_ltBuf != VK_NULL_HANDLE);
  total += buffer_bytes(m_gpuSdfs.size() * sizeof(float), m_sdfBuf != VK_NULL_HANDLE);
  total += buffer_bytes(m_gpuEnv.size() * sizeof(float), m_envBuf != VK_NULL_HANDLE);
  total += buffer_bytes(m_gpuEnvMeta.size() * sizeof(uint32_t), m_envMetaBuf != VK_NULL_HANDLE);
  return total;
}

void VulkanGpuPathTracer::publish_device_memory_telemetry() {
  m_deviceMemoryBytes = estimate_device_memory_bytes();
  VKP_METRIC_SET("vkp.gpu.device_memory_bytes", m_deviceMemoryBytes);
  if (m_deviceMemoryLimitBytes > 0u &&
      m_deviceMemoryBytes * 10ull >= m_deviceMemoryLimitBytes * 9ull) {
    VKP_LOG(Warn,
            "gpu",
            "device_memory_high",
            "backend",
            "vulkan",
            "used_bytes",
            m_deviceMemoryBytes,
            "total_bytes",
            m_deviceMemoryLimitBytes);
  }
}

void VulkanGpuPathTracer::rebuild_cpu_film_from_gpu() const {
  if (!m_cpuFilmDirty || !m_configured || !m_filmPtr) {
    return;
  }

  // The shader stores RGB sums plus sample count in alpha. FilmBuffer expects
  // samples, so feed it the average once instead of replaying all samples.
  const float* src = reinterpret_cast<const float*>(m_filmPtr);
  m_film.resize(m_settings.width, m_settings.height);
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData));
  m_film.clear();
  for (uint32_t y = 0; y < m_settings.height; ++y) {
    for (uint32_t x = 0; x < m_settings.width; ++x) {
      const uint32_t p = (y * m_settings.width + x) * 4u;
      const float cnt = std::max(1.0f, src[p + 3u]);
      // Store the mean as one synthetic sample so the FilmBuffer resolve does
      // not divide the already-averaged GPU accumulation a second time.
      const vkpt::pathtracer::Vec3 avg{src[p] / cnt, src[p + 1u] / cnt, src[p + 2u] / cnt};
      m_film.add_sample(x, y, avg);
    }
  }
  m_cpuFilmDirty = false;
}

void VulkanGpuPathTracer::destroy_film_buffers() {
  if (m_filmPtr != nullptr && m_filmMem != VK_NULL_HANDLE)
    vkUnmapMemory(m_device, m_filmMem);
  m_filmPtr = nullptr;
  m_cpuFilmDirty = false;
  if (m_filmBuf != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_filmBuf, nullptr); m_filmBuf = VK_NULL_HANDLE; }
  if (m_filmMem != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_filmMem, nullptr);    m_filmMem = VK_NULL_HANDLE; }
  publish_device_memory_telemetry();
}

void VulkanGpuPathTracer::destroy_scene_buffers() {
  auto del = [&](VkBuffer& b, VkDeviceMemory& m) {
    if (b != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, b, nullptr); b = VK_NULL_HANDLE; }
    if (m != VK_NULL_HANDLE) { vkFreeMemory(m_device, m, nullptr);    m = VK_NULL_HANDLE; }
  };
  del(m_vertBuf, m_vertMem); del(m_idxBuf, m_idxMem);
  del(m_matBuf,  m_matMem);  del(m_instBuf, m_instMem);
  del(m_ltBuf,   m_ltMem);
  del(m_sdfBuf,  m_sdfMem);  del(m_envBuf,  m_envMem);
  del(m_envMetaBuf, m_envMetaMem);
  m_sceneUploaded = false;
  publish_device_memory_telemetry();
}

void VulkanGpuPathTracer::destroy_pipeline() {
  if (m_pipeline   != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_pipeline, nullptr);          m_pipeline   = VK_NULL_HANDLE; }
  if (m_pipeLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_pipeLayout, nullptr);  m_pipeLayout = VK_NULL_HANDLE; }
  if (m_shaderMod  != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_shaderMod, nullptr);     m_shaderMod  = VK_NULL_HANDLE; }
}

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_VULKAN
