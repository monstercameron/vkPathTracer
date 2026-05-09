#ifdef PT_ENABLE_D3D12

#include "gpu/D3D12GpuPathTracer.h"
#include "gpu/D3D12GpuPathTracerInternal.h"
#include "core/Logging.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>
#include <array>
#include <iomanip>
#include <iterator>
#include <limits>
#include <wincodec.h>

namespace vkpt::gpu {

namespace {

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

D3D12GpuPathTracer::D3D12GpuPathTracer(std::string hlsl_path,
                                       std::string entry_point,
                                       std::optional<std::uint32_t> adapter_index)
    : m_hlslPath(std::move(hlsl_path)),
      m_entryPoint(std::move(entry_point)),
      m_adapterIndex(adapter_index) {
  m_shaderTraversalMode = SelectShaderTraversalMode();
  m_packedTriangleBufferEnabled = SelectPackedTriangleBuffer();
  std::string adapterTag = m_adapterIndex.has_value()
      ? (" adapter=" + std::to_string(*m_adapterIndex))
      : "";
  LogDebug("D3D12 tracer ctor hlsl=" + m_hlslPath + " entry=" + m_entryPoint +
           adapterTag +
           " shader_traversal=" + m_shaderTraversalMode +
           " packed_triangles=" + (m_packedTriangleBufferEnabled ? "true" : "false"));
  vkpt::core::Status initStatus = init_device();
  m_valid = initStatus.is_ok() && create_root_sig_and_pso();
  if (!m_valid && m_error.empty()) {
    m_error = initStatus.message;
  }
  if (!m_valid) {
    LogError("D3D12GpuPathTracer init failed: " + m_error);
    EmitGpuBackendAnomaly("d3d12",
                          "initialize",
                          m_error.empty() ? "initialization failed" : m_error,
                          introspect());
  } else {
    EmitGpuBackendStarted("d3d12", introspect());
  }
}

D3D12GpuPathTracer::D3D12GpuPathTracer(std::string hlsl_path,
                                       std::optional<std::uint32_t> adapter_index)
    : D3D12GpuPathTracer(std::move(hlsl_path), std::string("main"), adapter_index) {}

D3D12GpuPathTracer::D3D12GpuPathTracer(std::string hlsl_path, std::string entry_point)
    : D3D12GpuPathTracer(std::move(hlsl_path), std::move(entry_point), std::nullopt) {}

D3D12GpuPathTracer::~D3D12GpuPathTracer() { shutdown(); }

std::string D3D12GpuPathTracer::dxr_tier_string() const {
  if (m_dxrTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
    return "not_supported";
  }
  if (m_dxrTier >= D3D12_RAYTRACING_TIER_1_0) {
    // Some SDK/header combos may expose newer tiers without named enum labels.
    // Treat any tier >= 1.1 as "1.1+" to avoid contradictory reporting.
    if (m_dxrTier > D3D12_RAYTRACING_TIER_1_0) {
      return "1.1+";
    }
    return "1.0";
  }
  return "unknown";
}

void D3D12GpuPathTracer::set_prefer_dxr(bool enabled) {
  // DXR can be toggled after construction. Runtime objects and the state object
  // are created lazily so the compute backend remains cheap on non-DXR runs.
  const bool wasEnabled = m_preferDxr;
  m_preferDxr = enabled;
  m_usingDxrDispatch = false;
  m_loggedDxrFallback = false;
  if (enabled && !wasEnabled && m_sceneUploaded) {
    const std::size_t expectedTriDataFloats =
        static_cast<std::size_t>(m_gpuIdx.size() / 3u) * kGpuTriDataStrideFloats;
    if (m_gpuTriData.size() < expectedTriDataFloats) {
      m_sceneUploaded = false;
      m_dxrAccelReady = false;
    }
  }
  if (enabled && m_dxrSupported && !m_device5) {
    if (FAILED(m_device.As(&m_device5))) {
      m_dxrSupported = false;
      m_dxrTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
      LogError("set_prefer_dxr: ID3D12Device5 query failed");
    }
  }
  if (enabled && m_dxrSupported && !m_dxrRuntimeObjectsReady) {
    if (!init_dxr_runtime_objects()) {
      LogError("set_prefer_dxr: runtime object init failed: " + m_error);
    }
  }
  if (enabled && m_dxrRuntimeObjectsReady && !m_dxrPipelineReady) {
    // Derive RT shader path: replace _cs.hlsl -> _rt.hlsl
    m_rtHlslPath = m_hlslPath;
    auto pos = m_rtHlslPath.rfind("_cs.hlsl");
    if (pos != std::string::npos) {
      m_rtHlslPath.replace(pos, 8, "_rt.hlsl");
    } else {
      auto extpos = m_rtHlslPath.rfind(".hlsl");
      if (extpos != std::string::npos)
        m_rtHlslPath.replace(extpos, 5, "_rt.hlsl");
      else
        m_rtHlslPath += "_rt.hlsl";
    }
    LogInfo("set_prefer_dxr: rt_shader=" + m_rtHlslPath);
    if (!create_dxr_pipeline()) {
      LogError("set_prefer_dxr: pipeline creation failed: " + m_error);
    }
  }
}

// ============================================================================
// IPathTracer
// ============================================================================

vkpt::core::Status D3D12GpuPathTracer::configure(
    const vkpt::pathtracer::RenderSettings& s) {
  if (!m_valid) {
    m_error = "configure before init";
    LogError("configure rejected: " + m_error);
    EmitGpuBackendAnomaly("d3d12", "configure", m_error, introspect());
    return vkpt::core::Status::error(vkpt::core::StatusCode::NotReady, m_error);
  }
  if (s.width == 0u || s.height == 0u) {
    m_error = "configure invalid dimensions";
    LogError("configure rejected: " + m_error);
    EmitGpuBackendAnomaly("d3d12", "configure", m_error, introspect());
    return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument, m_error);
  }
  const uint64_t filmPixels = static_cast<uint64_t>(s.width) * s.height;
  if (filmPixels > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    m_error = "film dimensions exceed D3D12 backend limits";
    LogError("configure rejected: " + m_error);
    EmitGpuBackendAnomaly("d3d12", "configure", m_error, introspect());
    return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument, m_error);
  }
  // Configuration invalidates all GPU scene state because film dimensions,
  // batching, and optional postprocess buffers are tied to RenderSettings.
  const auto previousDeterminism = m_settings.determinism_context();
  m_settings   = s;
  vkpt::core::EmitDeterminismChangedIfNeeded("gpu",
                                             previousDeterminism,
                                             m_settings.determinism_context());
  m_configured = true;
  m_filmPixels = static_cast<uint32_t>(filmPixels);
  // The D3D12 backend keeps accumulation in the GPU UAV (m_filmBuf) and
  // only the resolve_settings on m_film matter for HDR fallbacks; no code
  // path writes per-pixel samples into m_film, so we leave its CPU buffers
  // empty rather than allocating ~20 bytes/pixel that we'll never use.
  m_film.resize(0u, 0u);
  m_film.set_resolve_settings(s.film_resolve);
  m_counters          = {};
  m_hasScene          = false;
  m_sceneUploaded     = false;
  m_ldrResolve.rgba8.clear();
  m_ldrResolve.width = 0u;
  m_ldrResolve.height = 0u;
  m_latestFilmReadbackToken = {};
  m_filmGeneration = 0u;
  m_ldrResolveGeneration = 0u;
  m_raysPerPixelPerDispatch = SelectRaysPerPixelPerDispatch(s);
  m_readbackInterval = SelectReadbackInterval(s);
  const bool interactivePreview = s.spp == std::numeric_limits<uint32_t>::max();
  m_forceReadbackEverySample =
      interactivePreview || ParseEnvBool("PT_D3D12_FORCE_READBACK_EVERY_SAMPLE", false);
  m_dynamicInstanceTransformsAllowed = ParseEnvBool("PT_D3D12_DYNAMIC_INSTANCE_TRANSFORMS", true);
  m_temporalHistoryValid = false;
  m_dxrBuildMode = SelectDxrBuildMode();
  m_bvhLeafSize = SelectBvhLeafSize();
  m_bvhBucketCount = SelectBvhBucketCount();
  m_bvhSplitMode = SelectBvhSplitMode();
  std::ostringstream cfg;
  cfg << "configure width=" << s.width << " height=" << s.height
      << " spp=" << s.spp
      << " max_depth=" << s.max_depth << " seed=" << s.seed
      << " gpu_denoiser=" << (s.enable_denoiser ? "true" : "false")
      << " temporal_aa=" << (s.enable_temporal_aa ? "true" : "false")
      << " rays_per_pixel_per_dispatch=" << m_raysPerPixelPerDispatch
      << " readback_interval=" << m_readbackInterval
      << " force_readback_every_sample=" << (m_forceReadbackEverySample ? "true" : "false")
      << " dynamic_instance_transforms=" << (m_dynamicInstanceTransformsAllowed ? "true" : "false")
      << " dxr_build_mode=" << m_dxrBuildMode
      << " bvh_leaf_size=" << m_bvhLeafSize
      << " bvh_bucket_count=" << m_bvhBucketCount
      << " bvh_split_mode=" << m_bvhSplitMode
      << " shader_traversal=" << m_shaderTraversalMode
      << " packed_triangles=" << (m_packedTriangleBufferEnabled ? "true" : "false");
  LogDebug(cfg.str());
  destroy_film_buffer();
  if (!create_film_buffer()) {
    EmitGpuBackendAnomaly("d3d12", "configure", m_error, introspect());
    return vkpt::core::Status::error(vkpt::core::StatusCode::InternalError, m_error);
  }
  EmitGpuBackendConfig("d3d12", introspect());
  debug_check_state_contract("configure");
  return vkpt::core::Status::ok("D3D12 path tracer configured");
}

bool D3D12GpuPathTracer::update_render_settings(const vkpt::pathtracer::RenderSettings& s) {
  if (!m_valid || !m_configured || s.width == 0u || s.height == 0u) {
    return false;
  }
  if (s.width != m_settings.width || s.height != m_settings.height ||
      !m_filmBuf || !m_ldrBuf || !m_denoiseBuf || !m_guideBuf ||
      !m_temporalBuf || !m_temporalHistoryBuf || !m_prevGuideBuf) {
    return false;
  }

  const auto previousDeterminism = m_settings.determinism_context();
  m_settings = s;
  vkpt::core::EmitDeterminismChangedIfNeeded("gpu",
                                             previousDeterminism,
                                             m_settings.determinism_context());
  m_raysPerPixelPerDispatch = SelectRaysPerPixelPerDispatch(s);
  m_readbackInterval = SelectReadbackInterval(s);
  const bool interactivePreview = s.spp == std::numeric_limits<uint32_t>::max();
  m_forceReadbackEverySample =
      interactivePreview || ParseEnvBool("PT_D3D12_FORCE_READBACK_EVERY_SAMPLE", false);
  m_dynamicInstanceTransformsAllowed = ParseEnvBool("PT_D3D12_DYNAMIC_INSTANCE_TRANSFORMS", true);
  m_film.set_resolve_settings(s.film_resolve);
  if (m_hasScene) {
    m_film.set_resolve_settings(
        vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData));
  }
  m_temporalHistoryValid = false;
  m_ldrResolve.rgba8.clear();
  m_ldrResolve.width = 0u;
  m_ldrResolve.height = 0u;
  m_latestFilmReadbackToken = {};
  ++m_filmGeneration;
  m_ldrResolveGeneration = 0u;
  LogDebug("D3D12 render settings updated without scene rebuild");
  debug_check_state_contract("update_render_settings");
  return true;
}

vkpt::core::Status D3D12GpuPathTracer::update_render_settings_status(
    const vkpt::pathtracer::RenderSettings& s) {
  const bool ok = update_render_settings(s);
  return GpuBackendOperationStatus(
      "d3d12.update_render_settings",
      ok,
      m_error,
      vkpt::core::StatusCode::NotReady);
}

vkpt::pathtracer::PathTracerStatus D3D12GpuPathTracer::status() const {
  vkpt::pathtracer::PathTracerStatus out;
  out.backend = m_usingDxrDispatch ? "d3d12-dxr" : "d3d12-compute";
  out.lifecycle = LifecycleFromGpuState(m_valid,
                                        m_configured,
                                        m_hasScene,
                                        m_sceneUploaded,
                                        m_error);
  out.scene_loaded = m_hasScene;
  out.accel_valid = m_sceneUploaded;
  out.ready_to_render = m_valid && m_configured && m_hasScene && m_sceneUploaded &&
                        m_filmBuf && m_ldrBuf && m_cmdQueue;
  out.current_sample = m_counters.samples == 0u ? 0u : (m_lastSampleIdx + 1u);
  out.total_samples = m_counters.samples;
  out.accumulation_gen = m_filmGeneration;
  out.last_error = m_error;
  return out;
}

void D3D12GpuPathTracer::debug_check_state_contract(const char* operation) const {
  (void)operation;
#ifndef NDEBUG
  assert(!m_sceneUploaded || m_hasScene);
  assert(!m_hasScene || m_configured);
  assert(!m_configured || m_valid);
  const bool ready = m_valid && m_configured && m_hasScene && m_sceneUploaded;
  if (ready) {
    assert(m_filmBuf.Get() != nullptr);
    assert(m_ldrBuf.Get() != nullptr);
    assert(m_cmdQueue.Get() != nullptr);
  }
#endif
}

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
