#pragma once

#ifdef PT_ENABLE_D3D12

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>

#include "pathtracer/PathTracer.h"
#include "gpu/GpuBackendIntrospection.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace vkpt::gpu {

static constexpr UINT kD3D12FrameCount = 2;

/// Root constant buffer shared by the D3D12 compute shaders and DXR raygen.
/// Keep this byte-for-byte aligned with the HLSL PathTraceConstants layout.
struct PathTraceConstants {
    float  camera_pos_x;  float camera_pos_y;  float camera_pos_z;  float fov_tan_half;
    float  cam_fwd_x;     float cam_fwd_y;     float cam_fwd_z;     float aspect;
    float  cam_right_x;   float cam_right_y;   float cam_right_z;   uint32_t num_sdfs;
    float  cam_up_x;      float cam_up_y;      float cam_up_z;      uint32_t sample_index;
    uint32_t num_insts;   uint32_t num_mats;    uint32_t num_lights; uint32_t width;
    uint32_t height;      uint32_t base_seed;   float    env_r;      float env_g;
    float  env_b;         float  max_depth_f;
    uint32_t rays_per_pixel;
    float  exposure; // tonemap exposure (replaces unused _pad1)
    float  aperture_radius; float focus_distance; uint32_t iris_blade_count; float iris_rotation_radians;
    float  iris_roundness; float anamorphic_squeeze; uint32_t tone_map; uint32_t output_transform;
    float  gamma; uint32_t clamp_output; float white_balance_r; float white_balance_g;
    float  white_balance_b; uint32_t denoiser_enabled; float denoiser_strength; float denoiser_color_sigma;
    uint32_t temporal_enabled; uint32_t temporal_history_valid; float temporal_feedback; float temporal_depth_sigma;
    float temporal_normal_power; float temporal_color_margin; uint32_t static_bvh_node_count; uint32_t dynamic_bvh_node_count;
    float prev_camera_pos_x; float prev_camera_pos_y; float prev_camera_pos_z; float prev_fov_tan_half;
    float prev_cam_fwd_x; float prev_cam_fwd_y; float prev_cam_fwd_z; float prev_aspect;
    float prev_cam_right_x; float prev_cam_right_y; float prev_cam_right_z; float _pad2;
    float prev_cam_up_x; float prev_cam_up_y; float prev_cam_up_z; float _pad3;
};
static_assert(sizeof(PathTraceConstants) == 272,
              "PathTraceConstants size mismatch (expected 272)");

struct D3D12TemporalCameraState {
  float camera_pos_x = 0.0f;
  float camera_pos_y = 0.0f;
  float camera_pos_z = 0.0f;
  float fov_tan_half = 1.0f;
  float cam_fwd_x = 0.0f;
  float cam_fwd_y = 0.0f;
  float cam_fwd_z = -1.0f;
  float aspect = 1.0f;
  float cam_right_x = 1.0f;
  float cam_right_y = 0.0f;
  float cam_right_z = 0.0f;
  float cam_up_x = 0.0f;
  float cam_up_y = 1.0f;
  float cam_up_z = 0.0f;
};

/// D3D12 path tracer backend with a compute primary path and optional DXR dispatch.
///
/// The backend owns device lifetime, scene buffer packing/upload, GPU film
/// accumulation, optional post passes, and the DXR BLAS/TLAS/SBT objects used
/// when hardware ray tracing is selected.
///
/// D3D12GpuPathTracer state contract (impl-side, layered on top of the
/// IPathTracer state grid in PathTracer.h):
///
/// state\method      init_device   init_dxr_runtime_objects  introspect  is_valid  last_error  gpu_name  vram_mb  dxr_supported  using_dxr_dispatch  health_report
/// Uninitialized     ->Configured  illegal                   ok          false     ""          ""        0        false          false               ok
/// Configured        illegal       ok-if-dxr-pref            ok          true      ""/error    ok        ok       ok             ok                  ok
/// SceneLoaded       illegal       ok-if-dxr-pref            ok          true      ""/error    ok        ok       ok             ok                  ok
/// Ready             illegal       noop                      ok          true      ""/error    ok        ok       ok             ok                  ok
/// Failed            error         error                     ok          false     error       ok        ok       ok             ok                  failed
///
/// init_dxr_runtime_objects() is opt-in via set_prefer_dxr(); it is only called
/// after init_device() succeeded and before build_or_update_acceleration().
/// The IPathTracer state machine grid in src/pathtracer/PathTracer.h still
/// governs configure/load_scene_snapshot/build_or_update_acceleration/
/// render_tile/reset_accumulation/update_*/resolve_*/status/shutdown.
class D3D12GpuPathTracer final : public vkpt::pathtracer::IPathTracer,
                                 public IGpuBackendIntrospect {
 public:
  /// Lightweight descriptor for an enumerable D3D12 adapter. EnumerateAdapters()
  /// queries DXGI without creating a full ID3D12Device, so it is safe to call
  /// before deciding how many tracers to instantiate (and on which adapters).
  struct AdapterDescriptor {
    std::uint32_t index = 0u;
    std::wstring description;
    std::uint64_t dedicated_vram_bytes = 0u;
    std::uint64_t shared_system_bytes = 0u;
    bool is_software = false;
    bool dxr_supported = false;
  };

  /// Returns one descriptor per enumerable DXGI adapter, in
  /// EnumAdapters1 order. Indices are stable within a single call and can be
  /// passed to the constructor as `adapter_index` to bind a tracer to a
  /// specific GPU. Software adapters and adapters that fail
  /// D3D12CreateDevice(11_0, nullptr) are still listed (with `is_software`
  /// set or `dxr_supported=false`) so callers can filter explicitly.
  static std::vector<AdapterDescriptor> EnumerateAdapters();

  /// Creates the backend and defers most GPU resource allocation until configure/load.
  ///
  /// When `adapter_index` is set, init_device() selects the matching DXGI
  /// adapter (`EnumAdapters1` order). When unset, the legacy "max VRAM wins"
  /// selection is preserved so single-GPU callers see byte-identical behavior.
  explicit D3D12GpuPathTracer(std::string hlsl_path,
                              std::optional<std::uint32_t> adapter_index = std::nullopt);
  D3D12GpuPathTracer(std::string hlsl_path, std::string entry_point);
  D3D12GpuPathTracer(std::string hlsl_path,
                     std::string entry_point,
                     std::optional<std::uint32_t> adapter_index);
  ~D3D12GpuPathTracer() override;

  /// Allocates film resources and records render settings for subsequent uploads.
  vkpt::core::Status configure(const vkpt::pathtracer::RenderSettings& s) override;
  /// Updates constants-only settings without rebuilding scene buffers when possible.
  bool update_render_settings(const vkpt::pathtracer::RenderSettings& s) override;
  vkpt::core::Status update_render_settings_status(
      const vkpt::pathtracer::RenderSettings& s);
  /// Stores a CPU scene snapshot; call build_or_update_acceleration() to upload it.
  vkpt::core::Status load_scene_snapshot(const vkpt::pathtracer::PathTracerSceneSnapshot& scene) override;
  /// Packs scene data into shader layouts, uploads buffers, and builds DXR AS if enabled.
  vkpt::core::Status build_or_update_acceleration() override;
  /// Clears GPU and CPU-visible accumulation state without rebuilding scene buffers.
  bool reset_accumulation() override;
  vkpt::core::Status reset_accumulation_status();
  /// Updates only camera state after scene buffers exist; invalidates temporal history.
  bool update_camera(const vkpt::pathtracer::Vec3& pos,
                     const vkpt::pathtracer::Vec3& target,
                     const vkpt::pathtracer::Vec3& up,
                     float fov_deg) override;
  vkpt::core::Status update_camera_status(const vkpt::pathtracer::Vec3& pos,
                                          const vkpt::pathtracer::Vec3& target,
                                          const vkpt::pathtracer::Vec3& up,
                                          float fov_deg);
  bool update_camera_state(const vkpt::pathtracer::RTCameraState& camera) override;
  vkpt::core::Status update_camera_state_status(
      const vkpt::pathtracer::RTCameraState& camera);
  /// Uploads dynamic instance transforms and refreshes the software BVH or DXR TLAS.
  bool update_instance_transforms(
      const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) override;
  vkpt::core::Status update_instance_transforms_status(
      const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates);
  vkpt::pathtracer::InstanceTransformPlan plan_instance_transform_update(
      std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
      const vkpt::pathtracer::InstanceTransformUpdateOptions& options) const override;
  vkpt::pathtracer::InstanceTransformUpdateResult apply_instance_transform_update(
      std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
      const vkpt::pathtracer::InstanceTransformUpdateOptions& options) override;
  /// Applies material/light-only deltas that do not require triangle repacking.
  bool update_scene_delta(const vkpt::pathtracer::RTSceneDeltaUpdate& update) override;
  vkpt::core::Status update_scene_delta_status(
      const vkpt::pathtracer::RTSceneDeltaUpdate& update);
  /// Dispatches one GPU tile request and optionally readbacks the LDR display buffer.
  bool render_tile(const vkpt::pathtracer::RenderTile& tile,
                   uint32_t frame_idx) override;
  vkpt::core::Status render_tile_status(const vkpt::pathtracer::RenderTile& tile,
                                        uint32_t frame_idx);
  bool render_tiles(std::span<const vkpt::pathtracer::RenderTile> tiles,
                    std::uint32_t frame_idx,
                    std::stop_token stop) override;
  bool supports_tile_batching() const override { return true; }
  vkpt::pathtracer::FilmLdr resolve_ldr() const override;
  vkpt::pathtracer::FilmHdr resolve_hdr() const override;
  vkpt::pathtracer::FilmReadbackToken request_film_readback() override;
  vkpt::pathtracer::FilmReadbackResult poll_film(
      vkpt::pathtracer::FilmReadbackToken token) override;
  vkpt::pathtracer::SampleCounters read_counters() const override;
  vkpt::pathtracer::PathTracerStatus status() const override;
  const vkpt::pathtracer::FilmBuffer& film() const override { return m_film; }
  void shutdown() override;

  bool        is_valid()     const { return m_valid; }
  std::string last_error() const { return m_error; }
  std::string gpu_name()   const { return m_gpuName; }
  uint32_t    vram_mb()    const { return m_vramMb; }
  bool        dxr_supported() const { return m_dxrSupported; }
  std::string dxr_tier_string() const;
  void        set_prefer_dxr(bool enabled);
  bool        prefer_dxr() const { return m_preferDxr; }
  bool        using_dxr_dispatch() const { return m_usingDxrDispatch; }
  uint32_t    rays_per_pixel_per_dispatch() const { return m_raysPerPixelPerDispatch; }
  uint32_t    readback_interval() const { return m_readbackInterval; }
  bool        force_readback_every_sample() const { return m_forceReadbackEverySample; }
  bool        dynamic_instance_transforms_allowed() const { return m_dynamicInstanceTransformsAllowed; }
  std::string dxr_build_mode() const { return m_dxrBuildMode; }
  uint32_t    bvh_leaf_size() const { return m_bvhLeafSize; }
  uint32_t    bvh_bucket_count() const { return m_bvhBucketCount; }
  std::string bvh_split_mode() const { return m_bvhSplitMode; }
  std::string shader_traversal_mode() const { return m_shaderTraversalMode; }
  bool        packed_triangle_buffer_enabled() const { return m_packedTriangleBufferEnabled; }
  GpuBackendIntrospection introspect() const override {
    const std::uint64_t total = static_cast<std::uint64_t>(m_vramMb) * 1024ull * 1024ull;
    const std::uint64_t used =
        m_uploadSize +
        (m_filmBuf ? m_filmBuf->GetDesc().Width : 0ull) +
        (m_ldrBuf ? m_ldrBuf->GetDesc().Width : 0ull) +
        (m_denoiseBuf ? m_denoiseBuf->GetDesc().Width : 0ull) +
        (m_guideBuf ? m_guideBuf->GetDesc().Width : 0ull) +
        (m_temporalBuf ? m_temporalBuf->GetDesc().Width : 0ull);
    GpuBackendIntrospection info;
    info.adapter_name = m_gpuName;
    info.vram_bytes_used = used;
    info.vram_bytes_total = total;
    info.pending_dispatches = 0u;
    info.last_present_us = 0u;
    info.last_fence_wait_us = 0u;
    info.timeline_value = static_cast<std::uint64_t>(
        std::max<UINT64>(m_fenceValue, m_nextFenceValue));
    info.device_lost_recent = !m_error.empty() && m_error.find("device") != std::string::npos;
    info.fence_timeout_recent = !m_error.empty() && m_error.find("fence") != std::string::npos;
    if (!m_valid && !m_error.empty()) {
      info.lifecycle = vkpt::core::contracts::ComponentLifecycle::Failed;
    } else if (!m_configured) {
      info.lifecycle = vkpt::core::contracts::ComponentLifecycle::Uninitialized;
    } else if (!m_sceneUploaded) {
      info.lifecycle = vkpt::core::contracts::ComponentLifecycle::Initializing;
    } else {
      info.lifecycle = vkpt::core::contracts::ComponentLifecycle::Ready;
    }
    info.current_flow_id = m_filmGeneration;
    info.set_determinism(m_settings.determinism_context());
    info.last_error = m_error;
    return info;
  }
  vkpt::core::health::Report health_report() const {
    return GpuHealthReportFromIntrospection(introspect());
  }

 private:
  struct DynamicInstanceTransformUpdateStage {
    // Mirrors only the slice of m_sceneData that can be mutated by a transform
    // update (the instances vector). The rest of the scene snapshot —
    // vertices, indices, materials, textures, lights, env map — is immutable
    // for transform updates and is left untouched on the live PathTracerSceneSnapshot.
    std::vector<vkpt::pathtracer::RTInstance> instances;
    std::vector<uint32_t> gpu_instances;
    std::vector<float> gpu_dynamic_bvh;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> dxr_instance_descs;
    uint32_t dynamic_instance_count = 0u;
    uint32_t matched_count = 0u;
    uint32_t first_changed_instance = 0u;
    uint32_t changed_instance_count = 0u;
  };

  /// Creates the DXGI factory, device, queue, command list, fence, and upload heap.
  vkpt::core::Status init_device();
  /// Creates DXR-specific queue/list/fence objects after device capability probing.
  vkpt::core::Status init_dxr_runtime_objects();
  /// Builds the compute root signature and all compute/postprocess PSOs.
  bool create_root_sig_and_pso();
  bool create_tonemap_pso(const std::string& src);
  bool create_denoise_pso(const std::string& src);
  bool create_guide_pso(const std::string& src);
  bool create_temporal_pso(const std::string& src);
  bool create_film_buffer();
  /// Creates or refreshes the shader-visible compute SRV/UAV descriptor heap.
  bool ensure_compute_srv_uav_heap();
  bool should_readback_sample(uint32_t sample_idx) const;
  /// Stages all packed scene arrays through the persistent upload heap.
  bool upload_scene_buffers();
  /// Uploads dynamic instance changes and the dynamic-instance BVH.
  bool upload_instance_buffer(uint32_t firstInstance = 0u,
                              uint32_t instanceCount = std::numeric_limits<uint32_t>::max());
  bool upload_instance_buffer_from(const std::vector<uint32_t>& gpuInstances,
                                   const std::vector<float>& gpuDynamicBvh,
                                   uint32_t firstInstance = 0u,
                                   uint32_t instanceCount = std::numeric_limits<uint32_t>::max());
  bool upload_instance_material_buffers(bool uploadInstances, bool uploadTriData);
  bool emit_pending_instance_upload(ID3D12GraphicsCommandList* commandList,
                                    UINT64 uploadOffsetBytes,
                                    UINT64* nextUploadOffsetBytes = nullptr);
  bool emit_pending_scene_delta_uploads(ID3D12GraphicsCommandList* commandList,
                                        UINT64 uploadOffsetBytes,
                                        UINT64* nextUploadOffsetBytes = nullptr);
  /// Uploads material and/or light buffers for small scene deltas.
  bool upload_material_light_buffers(bool uploadMaterials, bool uploadLights);
  bool build_texture_buffers();
  void destroy_scene_buffers();
  void destroy_film_buffer();
  bool wait_for_gpu();
  // DXR pipeline
  /// Compiles the DXR HLSL library to DXIL with runtime feature defines.
  bool compile_dxil(const std::string& path, std::vector<uint8_t>& outDxil);
  bool create_dxr_global_root_sig();
  /// Creates the DXR state object and shader binding table records.
  bool create_dxr_pipeline();
  /// Builds BLAS objects for static/dynamic geometry and a TLAS for dispatch.
  bool build_dxr_acceleration_structures();
  /// Refits the TLAS after dynamic instance transforms change.
  bool update_dxr_instance_buffer_and_tlas();
  bool update_dxr_instance_buffer_and_tlas_from(
      const std::vector<uint32_t>& gpuInstances,
      std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& dxrInstanceDescs);
  bool create_dxr_desc_heap();
  /// Dispatches the hardware ray tracing path and shared postprocess/readback chain.
  bool dispatch_dxr_rays(uint32_t sample_idx, uint32_t frame_idx, bool doReadback);
  bool wait_for_dxr_gpu();
  void destroy_dxr_resources();
  void debug_check_state_contract(const char* operation) const;

  Microsoft::WRL::ComPtr<IDXGIFactory6>  m_factory;
  Microsoft::WRL::ComPtr<ID3D12Device>   m_device;
  Microsoft::WRL::ComPtr<ID3D12Device5>  m_device5;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue>       m_cmdQueue;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator>   m_cmdAllocator;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue>         m_dxrCmdQueue;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator>     m_dxrCmdAllocator;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> m_dxrCmdList;
  Microsoft::WRL::ComPtr<ID3D12Fence>                m_dxrFence;
  HANDLE  m_dxrFenceEvent  = nullptr;
  UINT64  m_dxrFenceValue  = 0;
  Microsoft::WRL::ComPtr<ID3D12RootSignature>       m_rootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState>       m_pso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState>       m_tonemapPso; // second PSO for tonemap pass
  Microsoft::WRL::ComPtr<ID3D12PipelineState>       m_denoisePso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState>       m_guidePso;
  Microsoft::WRL::ComPtr<ID3D12PipelineState>       m_temporalPso;
  Microsoft::WRL::ComPtr<ID3D12Fence>               m_fence;
  HANDLE m_fenceEvent = nullptr;
  UINT64 m_fenceValue = 0;

  // Per-frame command recording context. Replaces the legacy single-allocator
  // model on the render-tile hot path so consecutive tiles can be queued and
  // signalled without each one waiting on the GPU. Cold paths (scene upload,
  // BVH/AS build, DXR build/refit) continue to use m_cmdAllocator/m_cmdList
  // because they explicitly drain the queue around CPU-visible operations.
  struct FrameContext {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList;
    UINT64 fence_value = 0;
    bool in_flight = false;
  };
  std::array<FrameContext, kD3D12FrameCount> m_frameCtx;
  Microsoft::WRL::ComPtr<ID3D12Fence> m_timelineFence;
  HANDLE m_timelineFenceEvent = nullptr;
  UINT64 m_nextFenceValue = 1;
  size_t m_frameRingIdx = 0;

  bool init_frame_contexts();
  void destroy_frame_contexts();
  bool wait_for_fence(UINT64 target);

  Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuf; // staging upload heap
  void* m_uploadPtr = nullptr;
  UINT64 m_uploadSize = 0;

  Microsoft::WRL::ComPtr<ID3D12Resource> m_vertBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_idxBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_matBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_instBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_ltBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_bvhBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_triMatBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_triDataBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_dynamicBvhBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_localBvhBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_sdfBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_texelBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_texMetaBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_envBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_envMetaBuf;

  Microsoft::WRL::ComPtr<ID3D12Resource> m_filmBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_filmReadbackBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_ldrBuf;          // GPU-side RGBA8 tonemap output
  Microsoft::WRL::ComPtr<ID3D12Resource> m_ldrReadbackBuf;  // CPU-readable copy of ldrBuf
  Microsoft::WRL::ComPtr<ID3D12Resource> m_denoiseBuf;      // GPU-side HDR denoise output
  Microsoft::WRL::ComPtr<ID3D12Resource> m_guideBuf;        // GPU-side albedo/normal/depth guide buffer
  Microsoft::WRL::ComPtr<ID3D12Resource> m_temporalBuf;     // GPU-side temporally accumulated HDR
  Microsoft::WRL::ComPtr<ID3D12Resource> m_temporalHistoryBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_prevGuideBuf;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvUavHeap;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_clearHeap; // persistent heap for reset_accumulation
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_clearCpuHeap; // CPU-only clear descriptor
  void* m_filmReadbackPtr = nullptr;
  void* m_ldrReadbackPtr  = nullptr;

  vkpt::pathtracer::RenderSettings        m_settings{};
  vkpt::pathtracer::PathTracerSceneSnapshot           m_sceneData{};
  mutable vkpt::pathtracer::FilmBuffer    m_film;
  mutable vkpt::pathtracer::SampleCounters m_counters{};

  std::string m_hlslPath;
  std::string m_entryPoint;
  std::optional<std::uint32_t> m_adapterIndex;
  std::string m_error;
  std::string m_gpuName;
  uint32_t    m_vramMb     = 0;
  bool        m_dxrSupported = false;
  D3D12_RAYTRACING_TIER m_dxrTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
  bool        m_dxrRuntimeObjectsReady = false;
  bool        m_preferDxr = false;
  bool        m_usingDxrDispatch = false;
  bool        m_loggedDxrFallback = false;
  bool        m_dxrAccelReady    = false;
  bool        m_dxrTlasUpdatePending = false;
  bool        m_dxrPipelineReady = false;
  std::string m_rtHlslPath;
  // DXR resources
  Microsoft::WRL::ComPtr<ID3D12Resource>              m_blasBuffer;
  Microsoft::WRL::ComPtr<ID3D12Resource>              m_blasScratch;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>  m_dxrBlasBuffers;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>  m_dxrBlasScratch;
  std::vector<D3D12_RAYTRACING_INSTANCE_DESC>          m_dxrInstanceDescs;
  Microsoft::WRL::ComPtr<ID3D12Resource>              m_tlasBuffer;
  Microsoft::WRL::ComPtr<ID3D12Resource>              m_tlasScratch;
  Microsoft::WRL::ComPtr<ID3D12Resource>              m_tlasInstanceBuf;
  Microsoft::WRL::ComPtr<ID3D12StateObject>           m_rtPso;
  Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> m_rtPsoProps;
  Microsoft::WRL::ComPtr<ID3D12Resource>              m_sbtBuffer;
  void*                                               m_sbtMappedPtr = nullptr;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>        m_dxrDescHeap;
  Microsoft::WRL::ComPtr<ID3D12RootSignature>         m_dxrGlobalRootSig;

  bool m_valid          = false;
  bool m_configured     = false;
  bool m_hasScene       = false;
  bool m_sceneUploaded  = false;
  uint32_t m_filmPixels = 0;
  uint32_t m_raysPerPixelPerDispatch = 1;
  uint32_t m_readbackInterval = 4;
  uint32_t m_fastMotionSamplesRemaining = 0;
  bool m_pendingInstanceUpload = false;
  uint32_t m_pendingInstanceUploadFirst = 0u;
  uint32_t m_pendingInstanceUploadCount = 0u;
  bool m_pendingMaterialUpload = false;
  bool m_pendingLightUpload = false;
  bool m_pendingInstanceMaterialUpload = false;
  bool m_pendingTriDataUpload = false;
  bool m_forceReadbackEverySample = false;
  bool m_dynamicInstanceTransformsAllowed = true;
  std::string m_dxrBuildMode = "fast_build";
  uint32_t m_bvhLeafSize = 16;
  uint32_t m_bvhBucketCount = 16;
  std::string m_bvhSplitMode = "sah";
  std::string m_shaderTraversalMode = "baseline";
  bool m_packedTriangleBufferEnabled = true;
  bool m_temporalHistoryValid = false;
  D3D12TemporalCameraState m_temporalPrevCamera{};
  vkpt::pathtracer::FilmReadbackToken m_latestFilmReadbackToken{};
  uint64_t m_nextFilmReadbackId = 1u;
  uint64_t m_filmGeneration = 0u;
  uint64_t m_ldrResolveGeneration = 0u;

  std::vector<float>    m_gpuVerts;
  std::vector<float>    m_gpuTexcoords;
  std::vector<uint32_t> m_gpuIdx;
  std::vector<float>    m_gpuMats;
  std::vector<uint32_t> m_gpuInsts;
  std::vector<float>    m_gpuLights;
  std::vector<float>    m_gpuBvh;
  std::vector<uint32_t> m_gpuTriMat;
  std::vector<float>    m_gpuTriData;
  std::vector<float>    m_gpuDynamicBvh;
  std::vector<float>    m_gpuLocalBvh;
  std::vector<float>    m_gpuSdfs;
  std::vector<uint32_t> m_gpuTexels;
  std::vector<uint32_t> m_gpuTextureMeta;
  std::vector<float>    m_gpuEnv;
  std::vector<uint32_t> m_gpuEnvMeta;
  bool m_dynamicInstanceTransformsEnabled = false;
  uint32_t m_staticTriangleCount = 0;
  uint32_t m_dynamicInstanceCount = 0;
  bool stage_dynamic_instance_transform_update(
      std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
      DynamicInstanceTransformUpdateStage& out) const;
  mutable uint32_t m_lastSampleIdx = 0;
  mutable vkpt::pathtracer::FilmLdr m_ldrResolve; // latest GPU-tonemapped frame
};

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
