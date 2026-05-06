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

#include <cstdint>
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
    float temporal_normal_power; float temporal_color_margin; float _pad0; float _pad1;
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
class D3D12GpuPathTracer final : public vkpt::pathtracer::IPathTracer {
 public:
  /// Creates the backend and defers most GPU resource allocation until configure/load.
  D3D12GpuPathTracer(std::string hlsl_path, std::string entry_point = "main");
  ~D3D12GpuPathTracer() override;

  /// Allocates film resources and records render settings for subsequent uploads.
  bool configure(const vkpt::pathtracer::RenderSettings& s) override;
  /// Stores a CPU scene snapshot; call build_or_update_acceleration() to upload it.
  bool load_scene_snapshot(const vkpt::pathtracer::RTSceneData& scene) override;
  /// Packs scene data into shader layouts, uploads buffers, and builds DXR AS if enabled.
  bool build_or_update_acceleration() override;
  /// Clears GPU and CPU-visible accumulation state without rebuilding scene buffers.
  bool reset_accumulation() override;
  /// Updates only camera state after scene buffers exist; invalidates temporal history.
  bool update_camera(const vkpt::pathtracer::Vec3& pos,
                     const vkpt::pathtracer::Vec3& target,
                     const vkpt::pathtracer::Vec3& up,
                     float fov_deg) override;
  bool update_camera_state(const vkpt::pathtracer::RTCameraState& camera) override;
  /// Uploads dynamic instance transforms and refreshes the software BVH or DXR TLAS.
  bool update_instance_transforms(
      const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) override;
  /// Applies material/light-only deltas that do not require triangle repacking.
  bool update_scene_delta(const vkpt::pathtracer::RTSceneDeltaUpdate& update) override;
  /// Dispatches one GPU sample batch and optionally readbacks the LDR display buffer.
  bool render_sample_batch(uint32_t sy, uint32_t ey,
                           uint32_t sample_idx, uint32_t frame_idx) override;
  vkpt::pathtracer::FilmLdr resolve_ldr() const override;
  vkpt::pathtracer::FilmHdr resolve_hdr() const override;
  vkpt::pathtracer::SampleCounters read_counters() const override;
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

 private:
  /// Creates the DXGI factory, device, queue, command list, fence, and upload heap.
  bool init_device();
  /// Creates DXR-specific queue/list/fence objects after device capability probing.
  bool init_dxr_runtime_objects();
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
  /// Uploads only the dynamic instance buffer and its dynamic-instance BVH.
  bool upload_instance_buffer();
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
  bool create_dxr_desc_heap();
  /// Dispatches the hardware ray tracing path and shared postprocess/readback chain.
  bool dispatch_dxr_rays(uint32_t sample_idx, uint32_t frame_idx, bool doReadback);
  bool wait_for_dxr_gpu();
  void destroy_dxr_resources();

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
  vkpt::pathtracer::RTSceneData           m_sceneData{};
  mutable vkpt::pathtracer::FilmBuffer    m_film;
  mutable vkpt::pathtracer::SampleCounters m_counters{};

  std::string m_hlslPath;
  std::string m_entryPoint;
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
  bool        m_dxrPipelineReady = false;
  std::string m_rtHlslPath;
  // DXR resources
  Microsoft::WRL::ComPtr<ID3D12Resource>              m_blasBuffer;
  Microsoft::WRL::ComPtr<ID3D12Resource>              m_blasScratch;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>  m_dxrBlasBuffers;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>  m_dxrBlasScratch;
  std::vector<D3D12_RAYTRACING_INSTANCE_DESC>          m_dxrInstanceDescs;
  Microsoft::WRL::ComPtr<ID3D12Resource>              m_blasVertUpload;
  Microsoft::WRL::ComPtr<ID3D12Resource>              m_blasIdxUpload;
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
  bool m_forceReadbackEverySample = false;
  bool m_dynamicInstanceTransformsAllowed = true;
  std::string m_dxrBuildMode = "fast_trace";
  uint32_t m_bvhLeafSize = 16;
  uint32_t m_bvhBucketCount = 16;
  std::string m_bvhSplitMode = "sah";
  std::string m_shaderTraversalMode = "baseline";
  bool m_packedTriangleBufferEnabled = true;
  bool m_temporalHistoryValid = false;
  D3D12TemporalCameraState m_temporalPrevCamera{};

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
  bool m_dynamicInstanceTransformsEnabled = false;
  uint32_t m_staticTriangleCount = 0;
  uint32_t m_dynamicInstanceCount = 0;
  mutable uint32_t m_lastSampleIdx = 0;
  mutable vkpt::pathtracer::FilmLdr m_ldrResolve; // latest GPU-tonemapped frame
};

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
