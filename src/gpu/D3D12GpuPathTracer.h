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
    float  cam_right_x;   float cam_right_y;   float cam_right_z;   float pad0;
    float  cam_up_x;      float cam_up_y;      float cam_up_z;      uint32_t sample_index;
    uint32_t num_insts;   uint32_t num_mats;    uint32_t num_lights; uint32_t width;
    uint32_t height;      uint32_t base_seed;   float    env_r;      float env_g;
    float  env_b;         float  max_depth_f;
    uint32_t rays_per_pixel;
    float  _pad1; // pad to 112 bytes (multiple of 16)
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
  bool render_sample_batch(uint32_t sy, uint32_t ey,
                           uint32_t sample_idx, uint32_t frame_idx) override;
  vkpt::pathtracer::FilmLdr resolve_ldr() const override;
  vkpt::pathtracer::FilmHdr resolve_hdr() const override;
  vkpt::pathtracer::SampleCounters read_counters() const override;
  const vkpt::pathtracer::FilmBuffer& film() const override { return m_film; }
  void shutdown() override;

  bool        is_valid()   const { return m_valid; }
  std::string last_error() const { return m_error; }
  std::string gpu_name()   const { return m_gpuName; }
  uint32_t    vram_mb()    const { return m_vramMb; }
  bool        dxr_supported() const { return m_dxrSupported; }
  std::string dxr_tier_string() const;
  void        set_prefer_dxr(bool enabled);
  bool        prefer_dxr() const { return m_preferDxr; }
  bool        using_dxr_dispatch() const { return m_usingDxrDispatch; }

 private:
  bool init_device();
  bool init_dxr_runtime_objects();
  bool create_root_sig_and_pso();
  bool create_film_buffer();
  bool upload_scene_buffers();
  void destroy_scene_buffers();
  void destroy_film_buffer();
  bool wait_for_gpu();
  // DXR pipeline
  bool compile_dxil(const std::string& path, std::vector<uint8_t>& outDxil);
  bool create_dxr_global_root_sig();
  bool create_dxr_pipeline();
  bool build_dxr_acceleration_structures();
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

  Microsoft::WRL::ComPtr<ID3D12Resource> m_filmBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_filmReadbackBuf;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvUavHeap;
  void* m_filmReadbackPtr = nullptr;

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

  std::vector<float>    m_gpuVerts;
  std::vector<uint32_t> m_gpuIdx;
  std::vector<float>    m_gpuMats;
  std::vector<uint32_t> m_gpuInsts;
  std::vector<float>    m_gpuLights;
  std::vector<float>    m_gpuBvh;
  std::vector<uint32_t> m_gpuTriMat;
  mutable uint32_t m_lastSampleIdx = 0;
};

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
