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
    float  _pad[3]; // pad to 112 bytes (multiple of 16)
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

 private:
  bool init_device();
  bool create_root_sig_and_pso();
  bool create_film_buffer();
  bool upload_scene_buffers();
  void destroy_scene_buffers();
  void destroy_film_buffer();
  void wait_for_gpu();

  Microsoft::WRL::ComPtr<IDXGIFactory6>  m_factory;
  Microsoft::WRL::ComPtr<ID3D12Device>   m_device;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue>       m_cmdQueue;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator>   m_cmdAllocator;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
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

  Microsoft::WRL::ComPtr<ID3D12Resource> m_filmBuf;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_filmReadbackBuf;
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
};

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
