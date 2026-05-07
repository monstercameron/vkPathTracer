#ifdef PT_ENABLE_D3D12

#include "gpu/D3D12GpuPathTracer.h"
#include "gpu/D3D12GpuPathTracerInternal.h"
#include "core/Logging.h"

#include <algorithm>
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

D3D12GpuPathTracer::D3D12GpuPathTracer(std::string hlsl_path, std::string entry_point)
    : m_hlslPath(std::move(hlsl_path)), m_entryPoint(std::move(entry_point)) {
  m_shaderTraversalMode = SelectShaderTraversalMode();
  m_packedTriangleBufferEnabled = SelectPackedTriangleBuffer();
  LogDebug("D3D12 tracer ctor hlsl=" + m_hlslPath + " entry=" + m_entryPoint +
           " shader_traversal=" + m_shaderTraversalMode +
           " packed_triangles=" + (m_packedTriangleBufferEnabled ? "true" : "false"));
  m_valid = init_device() && create_root_sig_and_pso();
  if (!m_valid) LogError("D3D12GpuPathTracer init failed: " + m_error);
}

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

bool D3D12GpuPathTracer::configure(const vkpt::pathtracer::RenderSettings& s) {
  if (!m_valid) {
    m_error = "configure before init";
    LogError("configure rejected: " + m_error);
    return false;
  }
  if (s.width == 0u || s.height == 0u) {
    m_error = "configure invalid dimensions";
    LogError("configure rejected: " + m_error);
    return false;
  }
  const uint64_t filmPixels = static_cast<uint64_t>(s.width) * s.height;
  if (filmPixels > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    m_error = "film dimensions exceed D3D12 backend limits";
    LogError("configure rejected: " + m_error);
    return false;
  }
  // Configuration invalidates all GPU scene state because film dimensions,
  // batching, and optional postprocess buffers are tied to RenderSettings.
  m_settings   = s;
  m_configured = true;
  m_filmPixels = static_cast<uint32_t>(filmPixels);
  m_film.resize(s.width, s.height);
  m_film.set_resolve_settings(s.film_resolve);
  m_film.clear();
  m_counters          = {};
  m_hasScene          = false;
  m_sceneUploaded     = false;
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
  return create_film_buffer();
}

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
