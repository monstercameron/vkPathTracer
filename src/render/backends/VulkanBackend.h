#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "render/backends/FrameGraph.h"
#include "render/interface/RenderContracts.h"
#include "pathtracer/PathTracer.h"
#include "scene/SceneSnapshot.h"
#include "scene/SnapshotRing.h"

namespace vkpt::render {

/// Vulkan compute compiler facade; currently validates GLSL compute metadata.
class VulkanShaderCompiler final : public IShaderCompiler {
 public:
  bool supports_feature(std::string_view feature) const override;
  bool compile_compute_shader(const ComputePipelineDesc& desc, std::string& out_artifact, std::string* diagnostics) override;
};

/// In-memory cache for Vulkan skeleton shader artifacts and manifests.
class VulkanShaderCache final : public IShaderCache {
 public:
  bool query(std::string_view key, std::string& binary) override;
  bool store(std::string_view key, const std::string& binary) override;
  bool invalidate(std::string_view key) override;
  std::string explain_miss(std::string_view key) const override;
  std::vector<CachedManifest> dump_manifest() const override;

 private:
  std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>> m_entries;
};

/// Simulated Vulkan resource allocator backed by CPU memory.
class VulkanResourceAllocator final : public IRenderResourceAllocator {
 public:
  ResourceHandle create_buffer(const BufferDesc& desc) override;
  bool destroy_buffer(ResourceHandle handle) override;
  ResourceHandle create_texture(const TextureDesc& desc) override;
  bool destroy_texture(ResourceHandle handle) override;
  bool upload_data(ResourceHandle target, const void* data, std::size_t byte_count) override;
  bool readback(ResourceHandle source, void* out_data, std::size_t out_size) const override;

 private:
  struct ResourceRecord {
    std::string label;
    std::vector<std::uint8_t> data;
    bool is_texture = false;
    std::uint64_t version = 0u;
    std::size_t size_bytes = 0u;
  };

  ResourceHandle next_handle();
  bool copy_or_fill_resource(ResourceHandle handle, const void* data, std::size_t byte_count);

  ResourceHandle m_nextHandle = 1u;
  std::unordered_map<ResourceHandle, ResourceRecord> m_resources;
};

/// Command context matching the Vulkan backend contract without native Vk objects.
class VulkanCommandContext final : public IRenderCommandContext {
 public:
  explicit VulkanCommandContext(std::uint32_t command_buffer_index = 0u,
                                std::uint64_t wait_timeline_value = 0u,
                                std::uint64_t signal_timeline_value = 0u);
  bool begin_frame() override;
  bool end_frame() override;
  bool begin_pass(PassType type, std::string_view label) override;
  bool end_pass() override;
  bool dispatch(uint32_t x, uint32_t y, uint32_t z) override;
  bool copy_buffer_to_texture(ResourceHandle source_buffer, ResourceHandle target_texture) override;
  bool barrier(ResourceHandle resource, std::uint32_t usage_before, std::uint32_t usage_after) override;

  std::uint32_t command_buffer_index() const { return m_commandBufferIndex; }
  std::uint64_t wait_timeline_value() const { return m_waitTimelineValue; }
  std::uint64_t signal_timeline_value() const { return m_signalTimelineValue; }
  std::uint32_t dispatch_count() const { return m_dispatchCount; }

 private:
  std::uint32_t m_commandBufferIndex = 0u;
  std::uint64_t m_waitTimelineValue = 0u;
  std::uint64_t m_signalTimelineValue = 0u;
  std::uint32_t m_dispatchCount = 0u;
  bool m_frameOpen = false;
  bool m_passOpen = false;
};

/// Headless Vulkan swapchain placeholder.
class VulkanSwapchain final : public IRenderSwapchain {
 public:
  explicit VulkanSwapchain(std::uint32_t width = 0u, std::uint32_t height = 0u);
  bool present() override;
  std::uint32_t width() const override;
  std::uint32_t height() const override;
  bool resize(std::uint32_t width, std::uint32_t height) override;

 private:
  std::uint32_t m_width = 0u;
  std::uint32_t m_height = 0u;
};

/// Simulated Vulkan device; command contexts are only created while begun.
class VulkanDevice final : public IRenderDevice {
 public:
  explicit VulkanDevice(std::unique_ptr<VulkanResourceAllocator> allocator,
                        std::unique_ptr<VulkanSwapchain> swapchain,
                        std::uint32_t command_buffer_count = 2u);
  bool begin() override;
  bool end() override;
  std::unique_ptr<IRenderCommandContext> create_command_context() override;
  IRenderSwapchain* swapchain() const override;
  IRenderResourceAllocator* allocator();
  std::uint32_t command_buffer_count() const { return m_commandBufferCount; }

 private:
  std::unique_ptr<VulkanResourceAllocator> m_allocator;
  std::unique_ptr<VulkanSwapchain> m_swapchain;
  std::uint32_t m_commandBufferCount = 2u;
  std::uint32_t m_nextCommandBuffer = 0u;
  bool m_running = false;
};

enum class VulkanSnapshotTransitionKind : std::uint8_t {
  None,
  InitialUpload,
  Continue,
  ReprojectCamera,
  RefitTransforms,
  ReshadeMaterials,
  RebuildScene
};

struct VulkanSnapshotBindingState {
  bool snapshot_bound = false;
  std::uint64_t generation = 0u;
  std::uint64_t topology_revision = 0u;
  std::uint64_t transform_revision = 0u;
  std::uint64_t camera_revision = 0u;
  std::uint64_t material_revision = 0u;
  VulkanSnapshotTransitionKind last_transition = VulkanSnapshotTransitionKind::None;
  bool geometry_storage_reused_from_previous = false;
  bool acceleration_reused_from_previous = false;
  bool transform_refit_descriptor = false;
  bool reset_accumulation = false;
  bool rebuild_tile_schedule = false;
  std::uint64_t bind_count = 0u;
};

struct VulkanTileBatchConfig {
  std::uint32_t max_tiles_in_flight = 2u;
  std::uint32_t command_buffer_count = 2u;
  std::uint64_t frame_index = 0u;
  bool present_after_batch = true;
};

struct VulkanTileSubmissionRecord {
  std::uint32_t tile_id = 0u;
  std::uint32_t gpu_id = 0u;
  std::uint32_t command_buffer_index = 0u;
  std::uint32_t sample_index = 0u;
  std::uint64_t frame_index = 0u;
  std::uint64_t snapshot_generation = 0u;
  std::uint64_t wait_timeline_value = 0u;
  std::uint64_t signal_timeline_value = 0u;
  std::uint32_t in_flight_after_submit = 0u;
};

struct VulkanTimelineDiagnostics {
  bool timeline_semaphore = true;
  bool double_buffered_command_buffers = true;
  bool cpu_gpu_overlap_observed = false;
  std::uint32_t command_buffer_count = 2u;
  std::uint32_t max_tiles_in_flight = 2u;
  std::uint32_t current_in_flight = 0u;
  std::uint32_t max_observed_in_flight = 0u;
  std::uint64_t last_submitted_value = 0u;
  std::uint64_t last_completed_value = 0u;
  std::uint64_t last_present_value = 0u;
  std::uint64_t total_tile_submissions = 0u;
  std::uint64_t total_presents = 0u;
};

struct VulkanTileBatchDiagnostics {
  bool success = false;
  std::uint32_t submitted_tiles = 0u;
  std::uint32_t max_observed_in_flight = 0u;
  std::uint64_t first_timeline_value = 0u;
  std::uint64_t last_submitted_timeline_value = 0u;
  std::uint64_t completed_timeline_value = 0u;
  std::uint64_t present_timeline_value = 0u;
  std::vector<VulkanTileSubmissionRecord> submissions;
  std::string error;
};

std::string_view VulkanSnapshotTransitionKindToString(VulkanSnapshotTransitionKind kind);

/// Vulkan compute backend skeleton used before native VkDevice integration.
class VulkanComputeBackend final : public IRenderBackend {
 public:
  bool initialize() override;
  bool shutdown() override;
  BackendKind kind() const override;
  std::string name() const override;
  RenderBackendCapabilities capabilities() const override;
  std::unique_ptr<IRenderDevice> create_device() override;
  IShaderCompiler* compiler() override;
  IShaderCache* shader_cache() override;
  std::unique_ptr<IFrameGraph> create_frame_graph() override;
  std::string last_error() const override { return m_lastError; }

  bool bind_scene_snapshot(vkpt::scene::RenderSceneSnapshot::Ptr snapshot);
  bool bind_latest_snapshot(vkpt::scene::SnapshotRing& snapshots,
                            std::uint32_t reader_id = vkpt::scene::SnapshotRing::kInvalidReader) {
    auto snapshot = reader_id == vkpt::scene::SnapshotRing::kInvalidReader
        ? snapshots.current()
        : snapshots.current(reader_id);
    return bind_scene_snapshot(std::move(snapshot));
  }
  const VulkanSnapshotBindingState& snapshot_state() const { return m_snapshotState; }
  VulkanTileBatchDiagnostics submit_tile_batch(
      std::span<const vkpt::pathtracer::RenderTile> tiles,
      const VulkanTileBatchConfig& config = VulkanTileBatchConfig{});
  VulkanTimelineDiagnostics timeline_diagnostics() const { return m_timeline; }

 private:
  bool m_initialized = false;
  bool m_simulated = true;
  std::unique_ptr<VulkanShaderCompiler> m_compiler;
  std::unique_ptr<VulkanShaderCache> m_cache;
  vkpt::scene::RenderSceneSnapshot::Ptr m_boundSnapshot;
  VulkanSnapshotBindingState m_snapshotState{};
  VulkanTimelineDiagnostics m_timeline{};
  std::uint32_t m_nextCommandBuffer = 0u;
  std::string m_lastError;
};

/// Execute a minimal graph/allocator smoke test against a Vulkan-compatible backend.
bool RunVulkanComputeSmoke(vkpt::render::IRenderBackend& backend);

/// Simulated Vulkan BVH/pathtrace pass result.
struct VulkanBVHPassResult {
  bool success = false;
  uint32_t vertex_buffer_count = 0;   // vertices uploaded
  uint32_t index_buffer_count  = 0;   // indices uploaded
  uint32_t instance_count      = 0;   // BLAS instances
  uint32_t bvh_node_estimate   = 0;   // SAH-estimated node count
  std::string error;
};

/// Upload PathTracerSceneSnapshot, simulate BVH build, and execute a pathtrace graph.
///
/// No Vulkan SDK objects are required in this stub; all operations are performed
/// through the simulated allocator and frame-graph contracts.
VulkanBVHPassResult RunVulkanBVHPass(
    vkpt::render::VulkanComputeBackend& backend,
    const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
    uint32_t width,
    uint32_t height);

}  // namespace vkpt::render

