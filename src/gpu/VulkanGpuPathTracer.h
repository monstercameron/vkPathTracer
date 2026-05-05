#pragma once

#ifdef PT_ENABLE_VULKAN

#include "pathtracer/PathTracer.h"
#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace vkpt::gpu {

// Push constants layout — must match pathtrace.comp exactly.
struct PathTracePushConstants {
    float camera_pos[3];  float fov_tan_half;  // 16
    float cam_forward[3]; float aspect;         // 32
    float cam_right[3];   float pad0;           // 48
    float cam_up[3];      uint32_t sample_index;// 64
    uint32_t num_insts;   uint32_t num_mats;    // 72
    uint32_t num_lights;  uint32_t width;       // 80
    uint32_t height;      uint32_t base_seed;   // 88
    float env_color[3];   float max_depth_f;    // 104
    float aperture_radius; float focus_distance; // 112
    uint32_t iris_blade_count; float iris_rotation_radians; // 120
    float iris_roundness; float anamorphic_squeeze; // 128
};
static_assert(sizeof(PathTracePushConstants) == 128,
              "PathTracePushConstants size mismatch");

// Real Vulkan compute path tracer implementing IPathTracer.
// Creates VkInstance/VkDevice, uploads scene data to GPU buffers,
// dispatches the pathtrace.comp compute shader per sample, reads back
// the RGBA32F film buffer, and converts to LDR via the CPU FilmBuffer.
class VulkanGpuPathTracer final : public vkpt::pathtracer::IPathTracer {
 public:
  // spv_path: path to the compiled pathtrace.spv SPIR-V file
  explicit VulkanGpuPathTracer(std::string spv_path);
  ~VulkanGpuPathTracer() override;

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

  bool        is_valid()    const { return m_valid; }
  std::string last_error()  const { return m_error; }
  std::string gpu_name()    const { return m_gpuName; }
  uint32_t    vram_mb()     const { return m_vramMb; }
  std::string gpu_type()    const { return m_gpuType; }
  uint32_t    vulkan_api()  const { return m_apiVersion; }

 private:
  // --- init / teardown -------------------------------------------------------
  bool init_device();
  bool create_cmd_pool();
  bool create_pipeline();
  bool create_descriptors();
  void destroy_scene_buffers();
  void destroy_film_buffers();
  void destroy_pipeline();
  void destroy_device();

  // --- helpers ---------------------------------------------------------------
  uint32_t find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags flags) const;
  bool make_buffer(VkDeviceSize size,
                   VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags props,
                   VkBuffer& buf, VkDeviceMemory& mem);

  // --- Vulkan handles --------------------------------------------------------
  VkInstance       m_instance   = VK_NULL_HANDLE;
  VkPhysicalDevice m_physDev    = VK_NULL_HANDLE;
  VkDevice         m_device     = VK_NULL_HANDLE;
  VkQueue          m_queue      = VK_NULL_HANDLE;
  uint32_t         m_qFam       = 0;

  VkCommandPool    m_cmdPool    = VK_NULL_HANDLE;
  VkCommandBuffer  m_cmdBuf     = VK_NULL_HANDLE;
  VkFence          m_fence      = VK_NULL_HANDLE;

  VkDescriptorSetLayout m_dsLayout = VK_NULL_HANDLE;
  VkDescriptorPool      m_dsPool   = VK_NULL_HANDLE;
  VkDescriptorSet       m_ds       = VK_NULL_HANDLE;

  VkShaderModule   m_shaderMod  = VK_NULL_HANDLE;
  VkPipelineLayout m_pipeLayout  = VK_NULL_HANDLE;
  VkPipeline       m_pipeline    = VK_NULL_HANDLE;

  // --- GPU scene buffers (device-local, written once per load_scene) ---------
  VkBuffer      m_vertBuf  = VK_NULL_HANDLE; VkDeviceMemory m_vertMem  = VK_NULL_HANDLE;
  VkBuffer      m_idxBuf   = VK_NULL_HANDLE; VkDeviceMemory m_idxMem   = VK_NULL_HANDLE;
  VkBuffer      m_matBuf   = VK_NULL_HANDLE; VkDeviceMemory m_matMem   = VK_NULL_HANDLE;
  VkBuffer      m_instBuf  = VK_NULL_HANDLE; VkDeviceMemory m_instMem  = VK_NULL_HANDLE;
  VkBuffer      m_ltBuf    = VK_NULL_HANDLE; VkDeviceMemory m_ltMem    = VK_NULL_HANDLE;

  // --- Film buffer (host-visible persistent accumulation) --------------------
  VkBuffer      m_filmBuf  = VK_NULL_HANDLE; VkDeviceMemory m_filmMem  = VK_NULL_HANDLE;
  void*         m_filmPtr  = nullptr;  // persistent mapped pointer

  // --- State -----------------------------------------------------------------
  vkpt::pathtracer::RenderSettings        m_settings{};
  vkpt::pathtracer::RTSceneData           m_sceneData{};
  mutable vkpt::pathtracer::FilmBuffer    m_film;
  mutable vkpt::pathtracer::SampleCounters m_counters{};

  std::string m_spvPath;
  std::string m_error;
  std::string m_gpuName;
  std::string m_gpuType;
  uint32_t    m_vramMb     = 0;
  uint32_t    m_apiVersion = 0;

  bool m_valid          = false;
  bool m_configured     = false;
  bool m_hasScene       = false;
  bool m_sceneUploaded  = false;

  uint32_t m_filmPixels = 0;

  // Flat GPU-side arrays packed from RTSceneData
  std::vector<float>    m_gpuVerts;   // 3 floats / vertex
  std::vector<uint32_t> m_gpuIdx;     // raw triangle index array
  std::vector<float>    m_gpuMats;    // 8 floats / material
  std::vector<uint32_t> m_gpuInsts;   // 4 uints  / instance
  std::vector<float>    m_gpuLights;  // 8 floats / light
};

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_VULKAN
