#ifdef PT_ENABLE_D3D12

#include "gpu/D3D12GpuPathTracerInternal.h"

#include <array>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

namespace vkpt::gpu {

bool D3D12GpuPathTracer::ensure_compute_srv_uav_heap() {
  if (m_srvUavHeap) return true;

  // The heap layout is part of the shader ABI. New buffers must be appended
  // deliberately and mirrored in create_root_sig_and_pso() and HLSL bindings.
  // Slots: t0-t14 (15 SRVs), u0 = FilmBuf, u1 = LdrBuf,
  // u2 = denoised HDR, u3 = current guide, u4 = temporal HDR,
  // u5 = temporal history, u6 = previous guide.
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 22;
  heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvUavHeap)))) {
    m_error = "srv heap";
    LogError("ensure_compute_srv_uav_heap: " + m_error);
    return false;
  }

  const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();

  auto makeSrv = [&](int slot, ID3D12Resource* buf, UINT64 size, DXGI_FORMAT fmt, UINT64 elementBytes = 4u) {
    if (!buf || size == 0u) {
      std::ostringstream st;
      st << "makeSrv slot=" << slot << " missing resource or empty size";
      LogError(st.str());
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = fmt;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements      = static_cast<UINT>(size / std::max<UINT64>(1u, elementBytes));
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(slot) * inc;
    m_device->CreateShaderResourceView(buf, &srv, h);
  };

  makeSrv(0, m_vertBuf.Get(),   m_gpuVerts.size()   * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(1, m_idxBuf.Get(),    m_gpuIdx.size()     * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(2, m_matBuf.Get(),    m_gpuMats.size()    * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(3, m_instBuf.Get(),   m_gpuInsts.size()   * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(4, m_ltBuf.Get(),     m_gpuLights.size()  * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(5, m_bvhBuf.Get(),    m_gpuBvh.size()     * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(6, m_triMatBuf.Get(), m_gpuTriMat.size()  * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(7, m_dynamicBvhBuf.Get(), m_gpuDynamicBvh.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(8, m_localBvhBuf.Get(), m_gpuLocalBvh.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(9, m_sdfBuf.Get(), m_gpuSdfs.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(10, m_triDataBuf.Get(), m_gpuTriData.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(11, m_envBuf.Get(), m_gpuEnv.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(12, m_envMetaBuf.Get(), m_gpuEnvMeta.size() * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(13, m_texelBuf.Get(), m_gpuTexels.size() * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(14, m_texMetaBuf.Get(), m_gpuTextureMeta.size() * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);

  {
    const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(filmSize);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(15) * inc;
    m_device->CreateUnorderedAccessView(m_filmBuf.Get(), nullptr, &uav, h);
  }
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format             = DXGI_FORMAT_R32_UINT;
    uav.ViewDimension      = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = static_cast<UINT>(m_filmPixels);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(16) * inc;
    m_device->CreateUnorderedAccessView(m_ldrBuf.Get(), nullptr, &uav, h);
  }
  {
    const UINT64 denoiseSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(denoiseSize);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(17) * inc;
    m_device->CreateUnorderedAccessView(m_denoiseBuf.Get(), nullptr, &uav, h);
  }
  {
    const UINT64 guideSize = static_cast<UINT64>(m_filmPixels) * 8u * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(guideSize);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(18) * inc;
    m_device->CreateUnorderedAccessView(m_guideBuf.Get(), nullptr, &uav, h);
  }
  {
    const UINT64 temporalSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(temporalSize);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(19) * inc;
    m_device->CreateUnorderedAccessView(m_temporalBuf.Get(), nullptr, &uav, h);
  }
  {
    const UINT64 temporalSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(temporalSize);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(20) * inc;
    m_device->CreateUnorderedAccessView(m_temporalHistoryBuf.Get(), nullptr, &uav, h);
  }
  {
    const UINT64 guideSize = static_cast<UINT64>(m_filmPixels) * 8u * sizeof(float);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(guideSize);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHandle;
    h.ptr += static_cast<SIZE_T>(21) * inc;
    m_device->CreateUnorderedAccessView(m_prevGuideBuf.Get(), nullptr, &uav, h);
  }

  return true;
}

bool D3D12GpuPathTracer::render_tile(const vkpt::pathtracer::RenderTile& tile,
                                     uint32_t frame_idx) {
  // The D3D12 backend ignores the tile range and dispatches the whole film.
  // Sample batching is controlled by m_raysPerPixelPerDispatch in constants.
  if (tile.width == 0u || tile.height == 0u) {
    return true;
  }
  const uint32_t sample_idx = tile.sample_index;
  const uint64_t callIndex = ++g_d3d12RenderBatchCalls;
  const bool verbose = D3D12VerboseLoggingEnabled() &&
      ((sample_idx % 16u == 0u) || (frame_idx % 16u == 0u));
  if (!m_valid || !m_configured || !m_sceneUploaded) {
    m_error = "render not ready";
    std::ostringstream st;
    st << "render_sample_batch rejected: " << m_error
       << " valid=" << (m_valid ? "true" : "false")
       << " configured=" << (m_configured ? "true" : "false")
       << " sceneUploaded=" << (m_sceneUploaded ? "true" : "false")
       << " filmPixels=" << m_filmPixels;
    LogError(st.str());
    EmitGpuBackendAnomaly("d3d12", "render_tile", m_error, introspect());
    return false;
  }
  if (!m_filmBuf || !m_filmReadbackBuf || !m_filmReadbackPtr || !m_filmPixels) {
    m_error = "film resources unavailable";
    std::ostringstream st;
    st << "render_sample_batch rejected: " << m_error
       << " filmBuf=" << (m_filmBuf ? "ok" : "null")
       << " readbackBuf=" << (m_filmReadbackBuf ? "ok" : "null")
       << " readbackPtr=" << (m_filmReadbackPtr ? "ok" : "null")
       << " filmPixels=" << m_filmPixels;
    LogError(st.str());
    EmitGpuBackendAnomaly("d3d12", "render_tile", m_error, introspect());
    return false;
  }
  if (m_settings.width == 0u || m_settings.height == 0u) {
    m_error = "invalid film dimensions";
    LogError("render_sample_batch rejected: " + m_error);
    EmitGpuBackendAnomaly("d3d12", "render_tile", m_error, introspect());
    return false;
  }
  if (verbose) {
    std::ostringstream st;
    st << "render_sample_batch cfg sample=" << sample_idx
       << " frame=" << frame_idx
       << " call=" << callIndex;
    LogDebug(st.str());
  }

  const bool probe = verbose;
  if (probe) {
    std::ostringstream st;
    st << "render_sample_batch probe sample=" << sample_idx
       << " frame=" << frame_idx
       << " dims=" << m_settings.width << "x" << m_settings.height
       << " film_pixels=" << m_filmPixels
       << " states(vf/rs)= "
       << (m_filmBuf ? "film=ok" : "film=null") << "/"
       << (m_filmReadbackBuf ? "rb=ok" : "rb=null");
    LogDebug(st.str());
  }

  if (verbose) {
    std::ostringstream st;
    st << "render_sample_batch start frame=" << frame_idx << " sample=" << sample_idx
       << " dims=" << m_settings.width << "x" << m_settings.height;
    LogDebug(st.str());
  }

  const bool fastMotionSample = m_fastMotionSamplesRemaining > 0u;
  if (m_preferDxr && !fastMotionSample) {
    if (!m_dxrSupported || !m_dxrRuntimeObjectsReady) {
      m_error = "DXR path requested but runtime objects are unavailable";
      LogError("render_sample_batch: " + m_error);
      return false;
    }
    if (m_dxrPipelineReady && m_dxrAccelReady) {
      if (m_dxrTlasUpdatePending) {
        if (!update_dxr_instance_buffer_and_tlas_from(m_gpuInsts, m_dxrInstanceDescs)) {
          LogError("render_sample_batch: " + m_error);
          return false;
        }
        m_dxrTlasUpdatePending = false;
      }
      // Route to hardware DXR dispatch
      m_usingDxrDispatch = true;
      return dispatch_dxr_rays(sample_idx, frame_idx, should_readback_sample(sample_idx));
    }
    if (!m_loggedDxrFallback) {
      LogInfo("DXR mode requested; DispatchRays path not wired yet, using compute fallback");
      m_loggedDxrFallback = true;
    }
    m_usingDxrDispatch = false;
  } else {
    m_usingDxrDispatch = false;
  }

  const auto& sc = m_sceneData;

  // Camera basis
  auto norm3 = [](float x, float y, float z, float* out) {
    float l = std::sqrt(x*x + y*y + z*z);
    if (l < 1e-9f) l = 1.0f;
    out[0]=x/l; out[1]=y/l; out[2]=z/l;
  };
  float fwd[3]; norm3(sc.camera_target.x - sc.camera_position.x,
    sc.camera_target.y - sc.camera_position.y,
    sc.camera_target.z - sc.camera_position.z, fwd);
  float rt[3] = {fwd[1]*sc.camera_up.z - fwd[2]*sc.camera_up.y,
    fwd[2]*sc.camera_up.x - fwd[0]*sc.camera_up.z,
    fwd[0]*sc.camera_up.y - fwd[1]*sc.camera_up.x};
  float rn[3]; norm3(rt[0],rt[1],rt[2],rn);
  float un[3]; norm3(rn[1]*fwd[2]-rn[2]*fwd[1],
    rn[2]*fwd[0]-rn[0]*fwd[2], rn[0]*fwd[1]-rn[1]*fwd[0],un);

  PathTraceConstants pc{};
  pc.camera_pos_x = sc.camera_position.x;
  pc.camera_pos_y = sc.camera_position.y;
  pc.camera_pos_z = sc.camera_position.z;
  pc.fov_tan_half = std::tan(0.5f * sc.camera_fov_deg * 3.14159265f / 180.0f);
  pc.cam_fwd_x=fwd[0]; pc.cam_fwd_y=fwd[1]; pc.cam_fwd_z=fwd[2];
  pc.aspect = static_cast<float>(m_settings.width) /
              std::max(1.0f, static_cast<float>(m_settings.height));
  pc.cam_right_x=rn[0]; pc.cam_right_y=rn[1]; pc.cam_right_z=rn[2];
  pc.num_sdfs = static_cast<uint32_t>(sc.sdf_primitives.size());
  pc.cam_up_x=un[0]; pc.cam_up_y=un[1]; pc.cam_up_z=un[2];
  pc.sample_index = sample_idx;
  pc.num_insts    = static_cast<uint32_t>(sc.instances.size());
  pc.num_mats     = static_cast<uint32_t>(sc.materials.size());
  pc.num_lights   = static_cast<uint32_t>(sc.lights.size());
  pc.width        = m_settings.width;
  pc.height       = m_settings.height;
  pc.base_seed    = static_cast<uint32_t>(m_settings.seed & 0xFFFFFFFFu)
                    ^ (frame_idx * 2654435761u);
  pc.env_r = sc.environment_color.x;
  pc.env_g = sc.environment_color.y;
  pc.env_b = sc.environment_color.z;
  pc.max_depth_f = fastMotionSample
      ? 1.0f
      : static_cast<float>(std::max(1u, m_settings.max_depth));
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
  if (verbose) {
    std::ostringstream st;
    st << "render_sample_batch constants sample_index=" << pc.sample_index
       << " seed=" << pc.base_seed
       << " scene(inst/mat/light)=" << pc.num_insts << "/" << pc.num_mats << "/" << pc.num_lights
       << " max_depth=" << pc.max_depth_f
       << " env=(" << pc.env_r << "," << pc.env_g << "," << pc.env_b << ")";
    LogDebug(st.str());
  }
  if (probe) {
    const float fwdLen = std::sqrt(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    const float rightLen = std::sqrt(rn[0]*rn[0] + rn[1]*rn[1] + rn[2]*rn[2]);
    const float upLen = std::sqrt(un[0]*un[0] + un[1]*un[1] + un[2]*un[2]);
    std::ostringstream st;
    st << "render_sample_batch basis lens fwd=" << fwdLen
       << " right=" << rightLen << " up=" << upLen
       << " fwd=(" << fwd[0] << "," << fwd[1] << "," << fwd[2] << ")"
       << " right=(" << rn[0] << "," << rn[1] << "," << rn[2] << ")"
       << " up=(" << un[0] << "," << un[1] << "," << un[2] << ")";
    LogDebug(st.str());
    std::array<uint32_t, kPathTraceRoot32BitValues> pcWords{};
    static_assert(sizeof(pcWords) <= sizeof(PathTraceConstants));
    std::memcpy(pcWords.data(), &pc, sizeof(pcWords));
    std::ostringstream dump;
    dump << "render_sample_batch constant_u32=";
    for (UINT i = 0u; i < kPathTraceRoot32BitValues; ++i) {
      if (i) dump << ",";
      dump << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << pcWords[static_cast<std::size_t>(i)];
    }
    dump << std::dec;
    LogDebug(dump.str());
    if (g_d3d12RenderBatchCalls <= 4u) {
      std::ostringstream sampleVals;
      const float camPos[3] = {sc.camera_position.x, sc.camera_position.y, sc.camera_position.z};
      const float camTarget[3] = {sc.camera_target.x, sc.camera_target.y, sc.camera_target.z};
      sampleVals << "camera_pos=(" << camPos[0] << "," << camPos[1] << "," << camPos[2] << ")"
                 << " camera_target=(" << camTarget[0] << "," << camTarget[1] << "," << camTarget[2] << ")"
                 << " material0=" << FormatFirstN(m_gpuMats.data(), 16u, 16u)
                 << " first_inst=" << FormatFirstN(m_gpuInsts.data(), 4u, 4u)
                 << " first_light=" << FormatFirstN(m_gpuLights.data(), 8u, 8u);
      LogDebug(sampleVals.str());
    }
  }

  // Reset command list
  const HRESULT resetAllocatorHr = m_cmdAllocator->Reset();
  if (FAILED(resetAllocatorHr)) {
    m_error = "allocator reset hr=" + FormatHr(resetAllocatorHr);
    LogError("render_sample_batch: " + m_error);
    return false;
  }
  const HRESULT resetCmdListHr = m_cmdList->Reset(m_cmdAllocator.Get(), m_pso.Get());
  if (FAILED(resetCmdListHr)) {
    m_error = "cmd list reset hr=" + FormatHr(resetCmdListHr);
    LogError("render_sample_batch: " + m_error);
    return false;
  }
  constexpr UINT64 kMotionUploadOffset = 4096u;
  UINT64 nextUploadOffset = kMotionUploadOffset;
  if (!emit_pending_instance_upload(m_cmdList.Get(),
                                    kMotionUploadOffset,
                                    &nextUploadOffset)) {
    LogError("render_sample_batch: " + m_error);
    return false;
  }
  if (!emit_pending_scene_delta_uploads(m_cmdList.Get(),
                                        nextUploadOffset,
                                        &nextUploadOffset)) {
    LogError("render_sample_batch: " + m_error);
    return false;
  }

  // Upload constants through the persistent upload heap and bind it as a CBV.
  // The command list is fenced before this memory is reused on the next submit.
  m_cmdList->SetComputeRootSignature(m_rootSig.Get());

  // Create descriptor heap for SRVs/UAVs lazily and reuse across samples.
  // Slots: t0-t14 SRVs, u0 film, u1 LDR, u2 denoised HDR, u3/u6 guides, u4/u5 temporal.
  if (!ensure_compute_srv_uav_heap()) return false;
  D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
  const auto resolveSettings =
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData);
  const auto whiteBalance = vkpt::pathtracer::WhiteBalanceScale(resolveSettings.white_balance_kelvin);
  const uint32_t effectiveRaysPerPixel =
      fastMotionSample ? 1u : std::max(1u, m_raysPerPixelPerDispatch);
  const bool doReadback = should_readback_sample(sample_idx);
  const bool doDenoise = doReadback && !fastMotionSample && m_settings.enable_denoiser;
  const bool doTemporal = doReadback && !fastMotionSample && m_settings.enable_temporal_aa;
  const bool doGuide = doDenoise || doTemporal;
  if (!m_settings.enable_temporal_aa || fastMotionSample) {
    m_temporalHistoryValid = false;
  }
  if (doDenoise && (!m_guidePso || !m_denoisePso || !m_guideBuf || !m_denoiseBuf)) {
    m_error = "GPU denoiser resources unavailable";
    LogError("render_sample_batch: " + m_error);
    return false;
  }
  if (doTemporal && (!m_guidePso || !m_temporalPso || !m_guideBuf ||
                     !m_temporalBuf || !m_temporalHistoryBuf || !m_prevGuideBuf)) {
    m_error = "GPU temporal AA resources unavailable";
    LogError("render_sample_batch: " + m_error);
    return false;
  }
  pc.rays_per_pixel = effectiveRaysPerPixel;
  pc.exposure = resolveSettings.exposure;
  pc.tone_map = static_cast<uint32_t>(resolveSettings.tone_map);
  pc.output_transform = static_cast<uint32_t>(resolveSettings.output_transform);
  pc.gamma = resolveSettings.gamma;
  pc.clamp_output = resolveSettings.clamp_output ? 1u : 0u;
  pc.white_balance_r = whiteBalance.x;
  pc.white_balance_g = whiteBalance.y;
  pc.white_balance_b = whiteBalance.z;
  pc.denoiser_enabled = doDenoise ? 1u : 0u;
  pc.denoiser_strength = doDenoise ? 1.0f : 0.0f;
  pc.denoiser_color_sigma = 0.22f;
  pc.temporal_enabled = doTemporal ? 1u : 0u;
  pc.temporal_history_valid = (doTemporal && m_temporalHistoryValid) ? 1u : 0u;
  pc.temporal_feedback = 0.92f;
  pc.temporal_depth_sigma = 0.05f;
  pc.temporal_normal_power = 28.0f;
  pc.temporal_color_margin = 0.12f;
  pc.static_bvh_node_count = (m_staticTriangleCount > 0u)
      ? static_cast<uint32_t>(m_gpuBvh.size() / kGpuBvhNodeStrideFloats)
      : 0u;
  pc.dynamic_bvh_node_count = (m_dynamicInstanceCount > 0u)
      ? static_cast<uint32_t>(m_gpuDynamicBvh.size() / kGpuBvhNodeStrideFloats)
      : 0u;
  FillPreviousCameraConstants(pc, m_temporalHistoryValid ? m_temporalPrevCamera : MakeTemporalCameraState(pc));

  ID3D12DescriptorHeap* heaps[] = {m_srvUavHeap.Get()};
  m_cmdList->SetDescriptorHeaps(1, heaps);
  m_cmdList->SetComputeRootDescriptorTable(1, gpuHandle);

  // Root constants (b0) mirror PathTraceConstants exactly.
  std::memcpy(m_uploadPtr, &pc, sizeof(pc));
  m_cmdList->SetComputeRootConstantBufferView(0, m_uploadBuf->GetGPUVirtualAddress());

  // --- Transition FilmBuf and LdrBuf to UAV for GPU work --------------------
  D3D12_RESOURCE_BARRIER filmBarrier{};
  filmBarrier.Type                       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  filmBarrier.Transition.pResource       = m_filmBuf.Get();
  filmBarrier.Transition.StateBefore     = D3D12_RESOURCE_STATE_COMMON;
  filmBarrier.Transition.StateAfter      = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  filmBarrier.Transition.Subresource     = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_cmdList->ResourceBarrier(1u, &filmBarrier);

  D3D12_RESOURCE_BARRIER ldrBarrier{};
  ldrBarrier.Type                        = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  ldrBarrier.Transition.pResource        = m_ldrBuf.Get();
  ldrBarrier.Transition.StateBefore      = D3D12_RESOURCE_STATE_COMMON;
  ldrBarrier.Transition.StateAfter       = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ldrBarrier.Transition.Subresource      = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  if (doReadback) {
    m_cmdList->ResourceBarrier(1u, &ldrBarrier);
  }

  D3D12_RESOURCE_BARRIER denoiseBarrier = MakeTransitionBarrier(
      m_denoiseBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_RESOURCE_BARRIER guideBarrier = MakeTransitionBarrier(
      m_guideBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_RESOURCE_BARRIER temporalBarrier = MakeTransitionBarrier(
      m_temporalBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_RESOURCE_BARRIER temporalHistoryBarrier = MakeTransitionBarrier(
      m_temporalHistoryBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_RESOURCE_BARRIER prevGuideBarrier = MakeTransitionBarrier(
      m_prevGuideBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (doDenoise || doGuide || doTemporal) {
    std::array<D3D12_RESOURCE_BARRIER, 5> startBarriers{};
    UINT barrierCount = 0u;
    if (doDenoise) startBarriers[barrierCount++] = denoiseBarrier;
    if (doGuide) startBarriers[barrierCount++] = guideBarrier;
    if (doTemporal) {
      startBarriers[barrierCount++] = temporalBarrier;
      startBarriers[barrierCount++] = temporalHistoryBarrier;
      startBarriers[barrierCount++] = prevGuideBarrier;
    }
    if (barrierCount > 0u) {
      m_cmdList->ResourceBarrier(barrierCount, startBarriers.data());
    }
  }

  // --- Path trace dispatch --------------------------------------------------
  const UINT groupsX = (m_settings.width + 7u) / 8u;
  const UINT groupsY = (m_settings.height + 7u) / 8u;
  if (verbose) {
    std::ostringstream d;
    d << "dispatch groups " << groupsX << "x" << groupsY;
    LogDebug(d.str());
  }
  m_cmdList->SetPipelineState(m_pso.Get());
  constexpr wchar_t kPathTraceEvent[] = L"D3D12 Compute PathTrace Dispatch";
  m_cmdList->BeginEvent(0, kPathTraceEvent, sizeof(kPathTraceEvent));
  m_cmdList->Dispatch(groupsX, groupsY, 1u);
  m_cmdList->EndEvent();

  if (doReadback) {
  // Post passes are only scheduled for display/readback samples. Accumulation
  // samples that are not read back leave only the HDR film in UAV state briefly.
  // UAV barrier on FilmBuf: ensure path trace writes are visible before tonemap reads.
  D3D12_RESOURCE_BARRIER uavBarrier{};
  uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uavBarrier.UAV.pResource = m_filmBuf.Get();
  m_cmdList->ResourceBarrier(1, &uavBarrier);

  if (doGuide) {
    m_cmdList->SetPipelineState(m_guidePso.Get());
    constexpr wchar_t kGuideEvent[] = L"D3D12 Compute Guide Dispatch";
    m_cmdList->BeginEvent(0, kGuideEvent, sizeof(kGuideEvent));
    m_cmdList->Dispatch(groupsX, groupsY, 1u);
    m_cmdList->EndEvent();

    uavBarrier.UAV.pResource = m_guideBuf.Get();
    m_cmdList->ResourceBarrier(1, &uavBarrier);
  }

  if (doTemporal) {
    m_cmdList->SetPipelineState(m_temporalPso.Get());
    constexpr wchar_t kTemporalEvent[] = L"D3D12 Compute Temporal AA Dispatch";
    m_cmdList->BeginEvent(0, kTemporalEvent, sizeof(kTemporalEvent));
    m_cmdList->Dispatch(groupsX, groupsY, 1u);
    m_cmdList->EndEvent();

    uavBarrier.UAV.pResource = m_temporalBuf.Get();
    m_cmdList->ResourceBarrier(1, &uavBarrier);
  }

  if (doDenoise) {
    m_cmdList->SetPipelineState(m_denoisePso.Get());
    constexpr wchar_t kDenoiseEvent[] = L"D3D12 Compute Denoise Dispatch";
    m_cmdList->BeginEvent(0, kDenoiseEvent, sizeof(kDenoiseEvent));
    m_cmdList->Dispatch(groupsX, groupsY, 1u);
    m_cmdList->EndEvent();

    uavBarrier.UAV.pResource = m_denoiseBuf.Get();
    m_cmdList->ResourceBarrier(1, &uavBarrier);
  }

  // --- GPU tonemap dispatch: RGBA32F film → RGBA8 LdrBuf --------------------
  m_cmdList->SetPipelineState(m_tonemapPso.Get());
  constexpr wchar_t kTonemapEvent[] = L"D3D12 Compute Tonemap Dispatch";
  m_cmdList->BeginEvent(0, kTonemapEvent, sizeof(kTonemapEvent));
  m_cmdList->Dispatch(groupsX, groupsY, 1u);
  m_cmdList->EndEvent();

  // UAV barrier on LdrBuf: ensure tonemap writes complete before copy-out.
  uavBarrier.UAV.pResource = m_ldrBuf.Get();
  m_cmdList->ResourceBarrier(1, &uavBarrier);

  if (doTemporal) {
    // Preserve the previous temporal output and guide buffers on GPU so the
    // next readback sample can reproject without a CPU round trip.
    const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
    const UINT64 guideSize = static_cast<UINT64>(m_filmPixels) * 8u * sizeof(float);
    std::array<D3D12_RESOURCE_BARRIER, 4> copyStartBarriers = {
        MakeTransitionBarrier(m_temporalBuf.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        MakeTransitionBarrier(m_temporalHistoryBuf.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
        MakeTransitionBarrier(m_guideBuf.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        MakeTransitionBarrier(m_prevGuideBuf.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)};
    m_cmdList->ResourceBarrier(static_cast<UINT>(copyStartBarriers.size()), copyStartBarriers.data());
    m_cmdList->CopyBufferRegion(m_temporalHistoryBuf.Get(), 0, m_temporalBuf.Get(), 0, filmSize);
    m_cmdList->CopyBufferRegion(m_prevGuideBuf.Get(), 0, m_guideBuf.Get(), 0, guideSize);

    std::array<D3D12_RESOURCE_BARRIER, 4> copyEndBarriers = {
        MakeTransitionBarrier(m_temporalBuf.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        MakeTransitionBarrier(m_temporalHistoryBuf.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
        MakeTransitionBarrier(m_guideBuf.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        MakeTransitionBarrier(m_prevGuideBuf.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)};
    m_cmdList->ResourceBarrier(static_cast<UINT>(copyEndBarriers.size()), copyEndBarriers.data());
  }

  // --- Transition for readback ----------------------------------------------
  // FilmBuf: UAV → COMMON (stays accumulating; no CPU readback needed for display)
  filmBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  filmBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
  // LdrBuf: UAV → COPY_SOURCE
  ldrBarrier.Transition.StateBefore  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ldrBarrier.Transition.StateAfter   = D3D12_RESOURCE_STATE_COPY_SOURCE;
  if (doDenoise || (doGuide && !doTemporal)) {
    std::array<D3D12_RESOURCE_BARRIER, 4> readbackBarriers{};
    UINT barrierCount = 0u;
    readbackBarriers[barrierCount++] = filmBarrier;
    readbackBarriers[barrierCount++] = ldrBarrier;
    denoiseBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    denoiseBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    guideBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    guideBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    if (doDenoise) readbackBarriers[barrierCount++] = denoiseBarrier;
    if (doGuide && !doTemporal) readbackBarriers[barrierCount++] = guideBarrier;
    m_cmdList->ResourceBarrier(barrierCount, readbackBarriers.data());
  } else {
    D3D12_RESOURCE_BARRIER readbackBarriers[2] = {filmBarrier, ldrBarrier};
    m_cmdList->ResourceBarrier(2, readbackBarriers);
  }

  // Copy LdrBuf (RGBA8, 4 B/pixel) to CPU-visible readback buffer.
  const UINT64 ldrSize = static_cast<UINT64>(m_filmPixels) * sizeof(uint32_t);
  m_cmdList->CopyBufferRegion(m_ldrReadbackBuf.Get(), 0, m_ldrBuf.Get(), 0, ldrSize);

  // LdrBuf: COPY_SOURCE → COMMON
  ldrBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  ldrBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
  m_cmdList->ResourceBarrier(1, &ldrBarrier);
  } else {
    filmBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    filmBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    m_cmdList->ResourceBarrier(1, &filmBarrier);
  }

  const auto closeRes = m_cmdList->Close();
  if (FAILED(closeRes)) {
    m_error = "cmd list close hr=" + FormatHr(closeRes);
    LogError("render_sample_batch: " + m_error);
    return false;
  }
  ID3D12CommandList* lists[] = {m_cmdList.Get()};
  if (verbose) {
    LogDebug("render_sample_batch execute cmdlist frame=" + std::to_string(frame_idx) +
             " sample=" + std::to_string(sample_idx));
  }
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    m_error = "wait_for_gpu failed";
    LogError("render_sample_batch: " + m_error);
    return false;
  }
  const auto removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during render hr=" + FormatHr(removeHr);
    LogError("render_sample_batch: " + m_error);
    return false;
  }

  // Store GPU-tonemapped LDR result for resolve_ldr() — zero CPU work needed.
  if (doTemporal) {
    m_temporalHistoryValid = true;
    m_temporalPrevCamera = MakeTemporalCameraState(pc);
  }
  ++m_filmGeneration;
  if (doReadback && m_ldrReadbackPtr && m_filmPixels > 0u) {
    m_ldrResolve.width  = m_settings.width;
    m_ldrResolve.height = m_settings.height;
    m_ldrResolve.rgba8.resize(static_cast<size_t>(m_filmPixels) * 4u);
    // LdrBuf stores uint packed as R|G<<8|B<<16|0xFF<<24.
    // In little-endian memory that maps directly to RGBA8 byte layout.
    std::memcpy(m_ldrResolve.rgba8.data(), m_ldrReadbackPtr,
                static_cast<size_t>(m_filmPixels) * 4u);
    m_ldrResolveGeneration = m_filmGeneration;
    if (verbose) {
      // Quick non-black probe on byte data (very cheap)
      const auto* b = m_ldrResolve.rgba8.data();
      bool nonBlack = false;
      for (size_t i = 0; i < std::min<size_t>(m_filmPixels, 64u) * 4u; i += 4u)
        if (b[i] || b[i+1u] || b[i+2u]) { nonBlack = true; break; }
      LogDebug("render_sample_batch ldr readback frame=" + std::to_string(frame_idx)
               + " non_black=" + (nonBlack ? "yes" : "no"));
    }
  } else {
    m_latestFilmReadbackToken = {};
  }
  if (m_fastMotionSamplesRemaining > 0u) {
    --m_fastMotionSamplesRemaining;
  }
  const uint64_t rpp = static_cast<uint64_t>(effectiveRaysPerPixel);
  const uint64_t sampleInc = static_cast<uint64_t>(m_settings.width) * m_settings.height * rpp;
  const uint64_t raysPerSample =
      EstimateLogicalRaysPerD3D12Sample(m_settings, m_sceneData, m_usingDxrDispatch);
  m_counters.samples += sampleInc;
  m_counters.rays    += SaturatingMulU64(sampleInc, raysPerSample);
  m_lastSampleIdx     = sample_idx * effectiveRaysPerPixel + (effectiveRaysPerPixel - 1u);
  if (verbose) {
    std::ostringstream en;
    en << "render_sample_batch complete frame=" << frame_idx << " sample=" << sample_idx
       << " samples=" << m_counters.samples << " rays=" << m_counters.rays
       << " estimated_rays_per_sample=" << raysPerSample;
    LogDebug(en.str());
  }
  debug_check_state_contract("render_tile");
  return true;
}

vkpt::core::Status D3D12GpuPathTracer::render_tile_status(
    const vkpt::pathtracer::RenderTile& tile,
    uint32_t frame_idx) {
  const bool ok = render_tile(tile, frame_idx);
  return GpuBackendOperationStatus(
      "d3d12.render_tile",
      ok,
      m_error,
      vkpt::core::StatusCode::NotReady);
}

bool D3D12GpuPathTracer::create_root_sig_and_pso() {
  // Compute and postprocess shaders share one compact root signature: a CBV for
  // PathTraceConstants plus one descriptor table for all scene/film buffers.
  // Root sig: param0 = constants CBV (b0), param1 = descriptor table
  D3D12_ROOT_PARAMETER params[2]{};
  // Constants (b0)
  params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0;
  params[0].Descriptor.RegisterSpace  = 0;
  params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
  // Descriptor table (t0-t14 SRV + u0-u6 UAV at slots 15-21)
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors     = 15;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].RegisterSpace      = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors     = 7; // u0 film, u1 LDR, u2 denoise, u3/u6 guides, u4/u5 temporal
  ranges[1].BaseShaderRegister = 0;
  ranges[1].RegisterSpace      = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = 15;
  params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 2;
  params[1].DescriptorTable.pDescriptorRanges   = ranges;
  params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rsd{};
  rsd.NumParameters = 2;
  rsd.pParameters   = params;
  rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
  HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
  if (FAILED(hr)) {
    m_error = "SerializeRootSignature";
    if (err) LogError(static_cast<const char*>(err->GetBufferPointer()));
    return false;
  }
  const HRESULT createRootHr = m_device->CreateRootSignature(0, sig->GetBufferPointer(),
      sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));
  if (FAILED(createRootHr)) {
    m_error = "CreateRootSignature hr=" + FormatHr(createRootHr);
    LogError("create_root_sig_and_pso: " + m_error);
    return false;
  }

  // Load HLSL source and compile at runtime
  std::ifstream f(m_hlslPath, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    m_error = "Cannot open HLSL: " + m_hlslPath;
    LogError("create_root_sig_and_pso: " + m_error);
    return false;
  }
  const std::streampos shaderEnd = f.tellg();
  if (shaderEnd <= std::streampos{0}) {
    m_error = "HLSL file is empty or unreadable: " + m_hlslPath;
    LogError("create_root_sig_and_pso: " + m_error);
    return false;
  }
  const auto sz = static_cast<size_t>(shaderEnd);
  LogDebug("compile shader " + m_hlslPath + " size=" + std::to_string(sz) + " bytes");
  f.seekg(0);
  std::string src(sz, '\0');
  if (!f.read(src.data(), static_cast<std::streamsize>(src.size()))) {
    m_error = "Failed to read HLSL: " + m_hlslPath;
    LogError("create_root_sig_and_pso: " + m_error);
    return false;
  }
  f.close();

  UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  // Keep shader optimization enabled in debug builds. The Qt/D3D12 preview is
  // commonly launched from a debug preset, and unoptimized HLSL dominates ray
  // throughput far more than CPU-side debug symbols help here.
  compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
  compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
  const char* traversalDefine = ShaderTraversalDefine(m_shaderTraversalMode);
  const char* packedTrisDefine = BoolDefine(m_packedTriangleBufferEnabled);
  D3D_SHADER_MACRO shaderMacros[] = {
      {"PT_D3D12_STATIC_TRAVERSAL_MODE", traversalDefine},
      {"PT_D3D12_PACKED_TRIANGLES", packedTrisDefine},
      {nullptr, nullptr}};
  HRESULT compileHr = D3DCompile(src.c_str(), src.size(), m_hlslPath.c_str(),
      shaderMacros, nullptr, m_entryPoint.c_str(), "cs_5_0",
      compileFlags, 0, &csBlob, &errBlob);
  if (FAILED(compileHr)) {
    m_error = "D3DCompile failed";
    if (errBlob) LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature     = m_rootSig.Get();
  pso.CS.pShaderBytecode  = csBlob->GetBufferPointer();
  pso.CS.BytecodeLength   = csBlob->GetBufferSize();
  const HRESULT createPsoHr = m_device->CreateComputePipelineState(&pso,
      IID_PPV_ARGS(&m_pso));
  if (FAILED(createPsoHr)) {
    m_error = "CreateComputePipelineState hr=" + FormatHr(createPsoHr);
    LogError("create_root_sig_and_pso: " + m_error);
    return false;
  }
  LogDebug("compiled shader bytes=" + std::to_string(csBlob->GetBufferSize()));
  LogInfo("D3D12 compute PSO created from " + m_hlslPath +
          " shader_traversal=" + m_shaderTraversalMode +
          " packed_triangles=" + (m_packedTriangleBufferEnabled ? "true" : "false"));

  // Compile tonemap entry point from same source
  if (!create_tonemap_pso(src)) return false;
  if (!create_guide_pso(src)) return false;
  if (!create_temporal_pso(src)) return false;
  if (!create_denoise_pso(src)) return false;

  return true;
}

bool D3D12GpuPathTracer::create_tonemap_pso(const std::string& src) {
  UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
  compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
  HRESULT hr = D3DCompile(src.c_str(), src.size(), m_hlslPath.c_str(),
      nullptr, nullptr, "tonemap_main", "cs_5_0",
      compileFlags, 0, &csBlob, &errBlob);
  if (FAILED(hr)) {
    m_error = "D3DCompile tonemap_main failed";
    if (errBlob) LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature    = m_rootSig.Get();
  pso.CS.pShaderBytecode = csBlob->GetBufferPointer();
  pso.CS.BytecodeLength  = csBlob->GetBufferSize();
  const HRESULT createHr = m_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_tonemapPso));
  if (FAILED(createHr)) {
    m_error = "CreateComputePipelineState tonemap hr=" + FormatHr(createHr);
    LogError("create_tonemap_pso: " + m_error);
    return false;
  }
  LogInfo("D3D12 tonemap PSO created");
  return true;
}

bool D3D12GpuPathTracer::create_guide_pso(const std::string& src) {
  UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
  compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  const char* traversalDefine = ShaderTraversalDefine(m_shaderTraversalMode);
  const char* packedTrisDefine = BoolDefine(m_packedTriangleBufferEnabled);
  D3D_SHADER_MACRO shaderMacros[] = {
      {"PT_D3D12_STATIC_TRAVERSAL_MODE", traversalDefine},
      {"PT_D3D12_PACKED_TRIANGLES", packedTrisDefine},
      {nullptr, nullptr}};
  Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
  HRESULT hr = D3DCompile(src.c_str(), src.size(), m_hlslPath.c_str(),
      shaderMacros, nullptr, "guide_main", "cs_5_0",
      compileFlags, 0, &csBlob, &errBlob);
  if (FAILED(hr)) {
    m_error = "D3DCompile guide_main failed";
    if (errBlob) LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_rootSig.Get();
  pso.CS.pShaderBytecode = csBlob->GetBufferPointer();
  pso.CS.BytecodeLength = csBlob->GetBufferSize();
  const HRESULT createHr = m_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_guidePso));
  if (FAILED(createHr)) {
    m_error = "CreateComputePipelineState guide hr=" + FormatHr(createHr);
    LogError("create_guide_pso: " + m_error);
    return false;
  }
  LogInfo("D3D12 denoiser guide PSO created");
  return true;
}

bool D3D12GpuPathTracer::create_denoise_pso(const std::string& src) {
  UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
  compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
  HRESULT hr = D3DCompile(src.c_str(), src.size(), m_hlslPath.c_str(),
      nullptr, nullptr, "denoise_main", "cs_5_0",
      compileFlags, 0, &csBlob, &errBlob);
  if (FAILED(hr)) {
    m_error = "D3DCompile denoise_main failed";
    if (errBlob) LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_rootSig.Get();
  pso.CS.pShaderBytecode = csBlob->GetBufferPointer();
  pso.CS.BytecodeLength = csBlob->GetBufferSize();
  const HRESULT createHr = m_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_denoisePso));
  if (FAILED(createHr)) {
    m_error = "CreateComputePipelineState denoise hr=" + FormatHr(createHr);
    LogError("create_denoise_pso: " + m_error);
    return false;
  }
  LogInfo("D3D12 denoise PSO created");
  return true;
}

bool D3D12GpuPathTracer::create_temporal_pso(const std::string& src) {
  UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#else
  compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
  HRESULT hr = D3DCompile(src.c_str(), src.size(), m_hlslPath.c_str(),
      nullptr, nullptr, "temporal_main", "cs_5_0",
      compileFlags, 0, &csBlob, &errBlob);
  if (FAILED(hr)) {
    m_error = "D3DCompile temporal_main failed";
    if (errBlob) LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = m_rootSig.Get();
  pso.CS.pShaderBytecode = csBlob->GetBufferPointer();
  pso.CS.BytecodeLength = csBlob->GetBufferSize();
  const HRESULT createHr = m_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_temporalPso));
  if (FAILED(createHr)) {
    m_error = "CreateComputePipelineState temporal hr=" + FormatHr(createHr);
    LogError("create_temporal_pso: " + m_error);
    return false;
  }
  LogInfo("D3D12 temporal AA PSO created");
  return true;
}

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
