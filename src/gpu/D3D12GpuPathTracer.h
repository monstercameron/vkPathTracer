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
};
static_assert(sizeof(PathTraceConstants) == 112,
              "PathTraceConstants size mismatch (expected 112)");

class D3D12GpuPathTracer final : public vkpt::pathtracer::IPathTracer {
 public:
  D3D12GpuPathTracer(std::string hlsl_path, std::string entry_point = "main");
  ~D3D12GpuPathTracer() override;

  bool configure(const vkpt::pathtracer::RenderSettings& s) override;
  bool load_scene_snapshot(const vkpt::pathtracer::RTSceneData& scene) override;
  bool build_or_update_acceleration() override;
  bool reset_accumulation() override;
  bool update_camera(const vkpt::pathtracer::Vec3& pos,
                     const vkpt::pathtracer::Vec3& target,
                     const vkpt::pathtracer::Vec3& up,
                     float fov_deg) override;
  bool update_instance_transforms(
      const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) override;
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

 private:
  bool init_device();
  bool init_dxr_runtime_objects();
  bool create_root_sig_and_pso();
  bool create_tonemap_pso(const std::string& src);
  bool create_film_buffer();
  bool ensure_compute_srv_uav_heap();
  bool should_readback_sample(uint32_t sample_idx) const;
  bool upload_scene_buffers();
  bool upload_instance_buffer();
  void destroy_scene_buffers();
  void destroy_film_buffer();
  bool wait_for_gpu();
  // DXR pipeline
  bool compile_dxil(const std::string& path, std::vector<uint8_t>& outDxil);
  bool create_dxr_global_root_sig();
  bool create_dxr_pipeline();
  bool build_dxr_acceleration_structures();
  bool update_dxr_instance_buffer_and_tlas();
  bool create_dxr_desc_heap();
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
  Microsoft::WRL::ComPtr<ID3D12Resource> m_dynamicBvhBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_localBvhBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_sdfBuf;

  Microsoft::WRL::ComPtr<ID3D12Resource> m_filmBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_filmReadbackBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_ldrBuf;          // GPU-side RGBA8 tonemap output
  Microsoft::WRL::ComPtr<ID3D12Resource> m_ldrReadbackBuf;  // CPU-readable copy of ldrBuf
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
  uint32_t m_bvhLeafSize = 4;
  uint32_t m_bvhBucketCount = 8;
  std::string m_bvhSplitMode = "sah";

  std::vector<float>    m_gpuVerts;
  std::vector<uint32_t> m_gpuIdx;
  std::vector<float>    m_gpuMats;
  std::vector<uint32_t> m_gpuInsts;
  std::vector<float>    m_gpuLights;
  std::vector<float>    m_gpuBvh;
  std::vector<uint32_t> m_gpuTriMat;
  std::vector<float>    m_gpuDynamicBvh;
  std::vector<float>    m_gpuLocalBvh;
  std::vector<float>    m_gpuSdfs;
  bool m_dynamicInstanceTransformsEnabled = false;
  uint32_t m_staticTriangleCount = 0;
  uint32_t m_dynamicInstanceCount = 0;
  mutable uint32_t m_lastSampleIdx = 0;
  mutable vkpt::pathtracer::FilmLdr m_ldrResolve; // latest GPU-tonemapped frame
};

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
