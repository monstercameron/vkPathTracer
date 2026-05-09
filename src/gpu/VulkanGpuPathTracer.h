#pragma once

#ifdef PT_ENABLE_VULKAN

#include "pathtracer/PathTracer.h"
#include "gpu/GpuBackendIntrospection.h"
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vkpt::gpu {

/// Push constants consumed by pathtrace.comp.
///
/// This is the per-dispatch camera/render state. Field order comes from
/// PathTracePushConstants.inc, which is also included by pathtrace.comp.
struct PathTracePushConstants {
#define VKPT_PC_FLOAT3(name) float name[3];
#define VKPT_PC_FLOAT(name) float name;
#define VKPT_PC_UINT(name) std::uint32_t name;
#include "gpu/PathTracePushConstants.inc"
#undef VKPT_PC_UINT
#undef VKPT_PC_FLOAT
#undef VKPT_PC_FLOAT3
};

inline constexpr std::uint32_t kPathTracePushConstantWordCount = 0u
#define VKPT_PC_FLOAT3(name) +3u
#define VKPT_PC_FLOAT(name) +1u
#define VKPT_PC_UINT(name) +1u
#include "gpu/PathTracePushConstants.inc"
#undef VKPT_PC_UINT
#undef VKPT_PC_FLOAT
#undef VKPT_PC_FLOAT3
    ;

inline constexpr std::size_t kPathTracePushConstantByteSize =
    static_cast<std::size_t>(kPathTracePushConstantWordCount) * sizeof(std::uint32_t);

static_assert(sizeof(PathTracePushConstants) == kPathTracePushConstantByteSize,
              "PathTracePushConstants must match the shared schema byte size");
static_assert(kPathTracePushConstantByteSize == 128,
              "PathTracePushConstants Vulkan ABI changed; update the shader schema and tests");
static_assert(offsetof(PathTracePushConstants, env_r) == 88,
              "PathTracePushConstants env color fields must stay scalarized for GLSL packing");

/// Compile-time contract for native Vulkan path-tracer submission.
///
/// The compute shader is full-width and uses gl_GlobalInvocationID, so native
/// tiling is row-based and workgroup-aligned. Legacy CPU resolve remains
/// available, and the backend also exposes a token/poll film readback contract
/// over the timeline value used to publish the host-visible film.
struct VulkanGpuSubmissionContract {
  uint32_t command_buffer_count = 3u;
  uint32_t max_tiles_in_flight = 3u;
  uint32_t workgroup_size_x = 8u;
  uint32_t workgroup_size_y = 8u;
  uint32_t tile_height_rows = 16u;
  bool uses_timeline_semaphore = true;
  bool uses_dispatch_base_tiles = true;
  bool waits_once_after_tile_batch = true;
  bool row_tiles_are_workgroup_aligned = true;
  bool emits_shader_compiled_event = true;
  bool emits_dispatch_events = true;
  bool records_fence_wait_histogram = true;
  bool records_device_memory_gauge = true;
  bool exposes_introspection = true;
  bool exposes_health_probe_contract = true;
  bool exposes_split_film_readback = true;
  bool film_readback_token_carries_timeline = true;
  bool init_device_returns_status = true;
};

/// Vulkan compute implementation of IPathTracer.
///
/// This backend uses host-visible storage buffers for scene and film data,
/// dispatches pathtrace.comp for each sample, then lazily mirrors the GPU film
/// into FilmBuffer when CPU-side resolve APIs are called.
///
/// VulkanGpuPathTracer state contract (impl-side, layered on top of the
/// IPathTracer state grid in PathTracer.h):
///
/// state\method      init_device  introspect  is_valid  last_error  gpu_name  vram_mb  gpu_type  vulkan_api  health_report
/// Uninitialized     ->Configured ok          false     ""          ""        0        ""        0           ok
/// Configured        illegal      ok          true      ""/error    ok        ok       ok        ok          ok
/// SceneLoaded       illegal      ok          true      ""/error    ok        ok       ok        ok          ok
/// Ready             illegal      ok          true      ""/error    ok        ok       ok        ok          ok
/// Failed            error        ok          false     error       ok        ok       ok        ok          failed
///
/// The IPathTracer state machine grid in src/pathtracer/PathTracer.h still
/// governs configure/load_scene_snapshot/build_or_update_acceleration/
/// render_tile/reset_accumulation/update_*/resolve_*/status/shutdown.
class VulkanGpuPathTracer final : public vkpt::pathtracer::IPathTracer,
                                  public IGpuBackendIntrospect {
 public:
  /// spv_path is the compiled pathtrace.comp SPIR-V module.
  explicit VulkanGpuPathTracer(std::string spv_path);
  ~VulkanGpuPathTracer() override;

  /// Allocates the host-visible film buffer and resets descriptor bindings.
  vkpt::core::Status configure(const vkpt::pathtracer::RenderSettings& s) override;
  vkpt::core::Status update_render_settings_status(
      const vkpt::pathtracer::RenderSettings& s);
  /// Stores a CPU scene snapshot; build_or_update_acceleration() performs upload.
  vkpt::core::Status load_scene_snapshot(const vkpt::pathtracer::PathTracerSceneSnapshot& scene) override;
  /// Packs PathTracerSceneSnapshot into shader-facing buffers and refreshes descriptors.
  vkpt::core::Status build_or_update_acceleration() override;
  bool reset_accumulation() override;
  vkpt::core::Status reset_accumulation_status();
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
  bool update_scene_delta(const vkpt::pathtracer::RTSceneDeltaUpdate& update) override;
  vkpt::core::Status update_scene_delta_status(
      const vkpt::pathtracer::RTSceneDeltaUpdate& update);
  /// Records and submits compute work for the requested tile sample.
  bool render_tile(const vkpt::pathtracer::RenderTile& tile,
                   uint32_t frame_idx) override;
  vkpt::core::Status render_tile_status(const vkpt::pathtracer::RenderTile& tile,
                                        uint32_t frame_idx);
  vkpt::pathtracer::FilmLdr resolve_ldr() const override;
  vkpt::pathtracer::FilmHdr resolve_hdr() const override;
  vkpt::pathtracer::FilmReadbackToken request_film_readback() override;
  vkpt::pathtracer::FilmReadbackResult poll_film(
      vkpt::pathtracer::FilmReadbackToken token) override;
  vkpt::pathtracer::SampleCounters read_counters() const override;
  vkpt::pathtracer::PathTracerStatus status() const override;
  const vkpt::pathtracer::FilmBuffer& film() const override {
    rebuild_cpu_film_from_gpu();
    return m_film;
  }
  void shutdown() override;

  bool        is_valid()    const { return m_valid; }
  std::string last_error()  const { return m_error; }
  std::string gpu_name()    const { return m_gpuName; }
  uint32_t    vram_mb()     const { return m_vramMb; }
  std::string gpu_type()    const { return m_gpuType; }
  uint32_t    vulkan_api()  const { return m_apiVersion; }
  GpuBackendIntrospection introspect() const override;
  vkpt::core::health::Report health_report() const;
  static VulkanGpuSubmissionContract submission_contract();

 private:
  // --- init / teardown -------------------------------------------------------
  /// Creates a headless Vulkan instance, selects a compute-capable GPU, and opens a queue.
  vkpt::core::Status init_device();
  /// Allocates reusable primary command buffers and the completion timeline.
  vkpt::core::Status create_cmd_pool();
  /// Loads SPIR-V and creates descriptor layout, pipeline layout, and compute PSO.
  vkpt::core::Status create_pipeline();
  /// Allocates/writes the storage-buffer descriptor set once buffers exist.
  bool create_descriptors();
  void destroy_scene_buffers();
  void destroy_film_buffers();
  void destroy_pipeline();
  void destroy_device();

  // --- helpers ---------------------------------------------------------------
  uint32_t find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags flags) const;
  /// Creates a VkBuffer and owns cleanup on partial allocation/bind failures.
  bool make_buffer(VkDeviceSize size,
                   VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags props,
                   VkBuffer& buf, VkDeviceMemory& mem);
  /// Copies the accumulated host-visible GPU film into the CPU FilmBuffer cache.
  void rebuild_cpu_film_from_gpu() const;
  bool recreate_scene_buffers_from_snapshot();
  bool record_tile_dispatch(VkCommandBuffer cmd_buf,
                            const PathTracePushConstants& pc,
                            uint32_t start_y,
                            uint32_t end_y);
  bool submit_tile_dispatch(VkCommandBuffer cmd_buf,
                            uint64_t signal_value,
                            uint32_t groups_x,
                            uint32_t groups_y);
  bool wait_for_timeline(uint64_t value);
  bool refresh_completed_timeline();
  uint64_t estimate_device_memory_bytes() const;
  void publish_device_memory_telemetry();
  void debug_check_state_contract(const char* operation) const;

  // --- Vulkan handles --------------------------------------------------------
  VkInstance       m_instance   = VK_NULL_HANDLE;
  VkPhysicalDevice m_physDev    = VK_NULL_HANDLE;
  VkDevice         m_device     = VK_NULL_HANDLE;
  VkQueue          m_queue      = VK_NULL_HANDLE;
  uint32_t         m_qFam       = 0;

  VkCommandPool    m_cmdPool    = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> m_cmdBufs;
  VkSemaphore      m_timelineSemaphore = VK_NULL_HANDLE;
  uint64_t         m_timelineValue = 0u;
  uint64_t         m_completedTimelineValue = 0u;
  uint32_t         m_pendingDispatches = 0u;
  uint64_t         m_dispatchesSubmitted = 0u;
  uint64_t         m_dispatchesCompleted = 0u;
  uint64_t         m_lastSubmitUs = 0u;
  uint64_t         m_lastFenceWaitUs = 0u;
  uint64_t         m_deviceMemoryBytes = 0u;
  uint64_t         m_deviceMemoryLimitBytes = 0u;
  bool             m_recentDeviceLost = false;
  bool             m_recentFenceTimeout = false;

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
  VkBuffer      m_sdfBuf   = VK_NULL_HANDLE; VkDeviceMemory m_sdfMem   = VK_NULL_HANDLE;
  VkBuffer      m_envBuf   = VK_NULL_HANDLE; VkDeviceMemory m_envMem   = VK_NULL_HANDLE;
  VkBuffer      m_envMetaBuf = VK_NULL_HANDLE; VkDeviceMemory m_envMetaMem = VK_NULL_HANDLE;

  // --- Film buffer (host-visible persistent accumulation) --------------------
  VkBuffer      m_filmBuf  = VK_NULL_HANDLE; VkDeviceMemory m_filmMem  = VK_NULL_HANDLE;
  void*         m_filmPtr  = nullptr;  // persistent mapped pointer

  // --- State -----------------------------------------------------------------
  vkpt::pathtracer::RenderSettings        m_settings{};
  vkpt::pathtracer::PathTracerSceneSnapshot           m_sceneData{};
  mutable vkpt::pathtracer::FilmBuffer    m_film;
  mutable vkpt::pathtracer::SampleCounters m_counters{};
  mutable bool m_cpuFilmDirty = false;
  vkpt::pathtracer::FilmReadbackToken m_latestFilmReadbackToken{};
  uint64_t m_nextFilmReadbackId = 1u;
  uint64_t m_accumulationGeneration = 0u;
  uint32_t m_currentSample = 0u;

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

  // Flat GPU-side arrays packed from PathTracerSceneSnapshot
  std::vector<float>    m_gpuVerts;   // 3 floats / vertex
  std::vector<uint32_t> m_gpuIdx;     // raw triangle index array
  std::vector<float>    m_gpuMats;    // 16 floats / material
  std::vector<uint32_t> m_gpuInsts;   // 24 uints / instance
  std::vector<float>    m_gpuLights;  // 16 floats / light
  std::vector<float>    m_gpuSdfs;    // 16 floats / SDF primitive
  std::vector<float>    m_gpuEnv;     // RGB float texels
  std::vector<uint32_t> m_gpuEnvMeta; // width, height, enabled, pad
};

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_VULKAN
