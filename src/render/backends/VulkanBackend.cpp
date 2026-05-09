#include "render/backends/VulkanBackend.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <deque>
#include <limits>
#include <random>
#include <cmath>

#include "core/log/Log.h"
#include "core/metrics/Metrics.h"

namespace vkpt::render {

namespace {

constexpr std::uint64_t kTelemetrySamplePeriodNs = 1000000000ull;

std::uint64_t ElapsedUs(std::chrono::steady_clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
}

bool SnapshotGeometryStorageReused(const vkpt::scene::RenderSceneSnapshot& previous,
                                   const vkpt::scene::RenderSceneSnapshot& current) {
  return current.vertices.shares_storage_with(previous.vertices) &&
         current.texcoords.shares_storage_with(previous.texcoords) &&
         current.indices.shares_storage_with(previous.indices) &&
         current.local_vertices.shares_storage_with(previous.local_vertices) &&
         current.local_indices.shares_storage_with(previous.local_indices) &&
         current.tessellation_requests.shares_storage_with(previous.tessellation_requests) &&
         current.sdf_primitives.shares_storage_with(previous.sdf_primitives) &&
         current.environment_map.shares_storage_with(previous.environment_map);
}

VulkanSnapshotTransitionKind ClassifySnapshotTransition(
    const vkpt::scene::RenderSceneSnapshot* previous,
    const vkpt::scene::RenderSceneSnapshot& current) {
  if (previous == nullptr) {
    return VulkanSnapshotTransitionKind::InitialUpload;
  }

  const bool topologyChanged = previous->topology_revision != current.topology_revision;
  const bool transformChanged = previous->transform_revision != current.transform_revision;
  const bool cameraChanged = previous->camera_revision != current.camera_revision;
  const bool materialChanged = previous->material_revision != current.material_revision;
  if (!topologyChanged && !transformChanged && !cameraChanged && !materialChanged) {
    return VulkanSnapshotTransitionKind::Continue;
  }
  if (topologyChanged) {
    return VulkanSnapshotTransitionKind::RebuildScene;
  }
  if (transformChanged && !cameraChanged && !materialChanged &&
      (!current.instance_motion.empty() || current.acceleration.transform_refit_descriptor)) {
    return VulkanSnapshotTransitionKind::RefitTransforms;
  }
  if (cameraChanged && !transformChanged && !materialChanged) {
    return VulkanSnapshotTransitionKind::ReprojectCamera;
  }
  if (materialChanged && !transformChanged && !cameraChanged) {
    return VulkanSnapshotTransitionKind::ReshadeMaterials;
  }
  return VulkanSnapshotTransitionKind::RebuildScene;
}

bool TransitionResetsAccumulation(VulkanSnapshotTransitionKind transition) {
  return transition == VulkanSnapshotTransitionKind::InitialUpload ||
         transition == VulkanSnapshotTransitionKind::RebuildScene;
}

bool TransitionRebuildsTileSchedule(VulkanSnapshotTransitionKind transition) {
  return transition != VulkanSnapshotTransitionKind::None &&
         transition != VulkanSnapshotTransitionKind::Continue;
}

}  // namespace

bool VulkanShaderCompiler::supports_feature(std::string_view feature) const {
  return feature == "compute" || feature == "storage-buffers" || feature == "storage-textures";
}

vkpt::core::Status VulkanShaderCompiler::compile_compute_shader(const ComputePipelineDesc& desc,
                                                                std::string& out_artifact,
                                                                std::string* diagnostics) {
  const auto compileStart = std::chrono::steady_clock::now();
  if (desc.source_path.empty()) {
    if (diagnostics) {
      *diagnostics = "missing source path";
    }
    return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument,
                                     "missing source path");
  }
  std::string defines;
  for (const auto& define : desc.defines) {
    if (!defines.empty()) {
      defines.push_back(',');
    }
    defines += define;
  }
  out_artifact = "vulkan-compute:" + desc.source_path + ":" + desc.entry_point + ":" + defines;
  VKP_LOG(Info,
          "gpu",
          "shader_compiled",
          "backend",
          "vulkan",
          "shader",
          desc.debug_label.empty() ? desc.source_path : desc.debug_label,
          "bytes",
          static_cast<std::uint64_t>(out_artifact.size()),
          "compile_us",
          ElapsedUs(compileStart),
          "entry",
          desc.entry_point);
  return vkpt::core::Status::ok();
}

bool VulkanShaderCache::query(std::string_view key, std::string& binary) {
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return false;
  }
  binary = it->second;
  VKP_LOG(Debug,
          "gpu",
          "shader_cached",
          "backend",
          "vulkan",
          "shader",
          key,
          "bytes",
          static_cast<std::uint64_t>(binary.size()),
          "compile_us",
          static_cast<std::uint64_t>(0));
  return true;
}

bool VulkanShaderCache::store(std::string_view key, const std::string& binary) {
  m_entries[std::string(key)] = binary;
  return true;
}

bool VulkanShaderCache::invalidate(std::string_view key) {
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    return false;
  }
  m_entries.erase(it);
  return true;
}

std::string VulkanShaderCache::explain_miss(std::string_view key) const {
  return "vulkan shader cache miss: " + std::string(key);
}

std::vector<CachedManifest> VulkanShaderCache::dump_manifest() const {
  std::vector<CachedManifest> out;
  out.reserve(m_entries.size());
  for (const auto& entry : m_entries) {
    ShaderManifest manifest;
    manifest.shader_family = "cached";
    manifest.entry_point = "main";
    manifest.backend = "vulkan";
    manifest.source_format = ShaderSourceFormat::Glsl;
    manifest.source_hash = MakeShaderManifestHash(entry.first);
    manifest.variant_hash = MakeShaderManifestHash(entry.second);
    manifest.cache_key = entry.first;
    manifest.artifact_path = entry.second;
    manifest.manifest_dump_path = BuildShaderManifestDumpPath("shader_cache", manifest);
    manifest.compile_success = true;
    manifest.validation_success = true;
    out.push_back({SerializeShaderManifest(manifest),
                   "vulkan",
                   manifest.cache_key,
                   manifest.artifact_path,
                   manifest.manifest_dump_path,
                   manifest.compile_success});
  }
  return out;
}

VulkanCommandContext::VulkanCommandContext(std::uint32_t command_buffer_index,
                                           std::uint64_t wait_timeline_value,
                                           std::uint64_t signal_timeline_value)
    : m_commandBufferIndex(command_buffer_index),
      m_waitTimelineValue(wait_timeline_value),
      m_signalTimelineValue(signal_timeline_value) {}

bool VulkanCommandContext::begin_frame() {
  if (m_frameOpen) {
    return false;
  }
  m_frameOpen = true;
  return true;
}

bool VulkanCommandContext::end_frame() {
  if (!m_frameOpen || m_passOpen) {
    return false;
  }
  m_frameOpen = false;
  return true;
}

bool VulkanCommandContext::begin_pass(PassType type, std::string_view label) {
  (void)type;
  (void)label;
  if (!m_frameOpen || m_passOpen) {
    return false;
  }
  m_passOpen = true;
  return true;
}

bool VulkanCommandContext::end_pass() {
  if (!m_passOpen) {
    return false;
  }
  m_passOpen = false;
  return true;
}

bool VulkanCommandContext::dispatch(uint32_t x, uint32_t y, uint32_t z) {
  if (!m_frameOpen || !m_passOpen || x == 0u || y == 0u || z == 0u) {
    return false;
  }
  ++m_dispatchCount;
  return true;
}

bool VulkanCommandContext::copy_buffer_to_texture(ResourceHandle source_buffer, ResourceHandle target_texture) {
  (void)source_buffer;
  (void)target_texture;
  return true;
}

bool VulkanCommandContext::barrier(ResourceHandle resource, std::uint32_t usage_before, std::uint32_t usage_after) {
  (void)resource;
  (void)usage_before;
  (void)usage_after;
  return true;
}

VulkanSwapchain::VulkanSwapchain(std::uint32_t width, std::uint32_t height) : m_width(width), m_height(height) {}

bool VulkanSwapchain::present() {
  return true;
}

std::uint32_t VulkanSwapchain::width() const {
  return m_width;
}

std::uint32_t VulkanSwapchain::height() const {
  return m_height;
}

bool VulkanSwapchain::resize(std::uint32_t width, std::uint32_t height) {
  m_width = width;
  m_height = height;
  return true;
}

ResourceHandle VulkanResourceAllocator::next_handle() {
  return m_nextHandle++;
}

bool VulkanResourceAllocator::copy_or_fill_resource(ResourceHandle handle, const void* data, std::size_t byte_count) {
  auto it = m_resources.find(handle);
  if (it == m_resources.end()) {
    return false;
  }
  const std::size_t to_copy = std::min(byte_count, it->second.size_bytes);
  if (data) {
    std::memcpy(it->second.data.data(), data, to_copy);
  } else {
    std::fill(it->second.data.begin(), it->second.data.begin() + static_cast<std::ptrdiff_t>(to_copy), 0u);
  }
  it->second.version++;
  return true;
}

ResourceHandle VulkanResourceAllocator::create_buffer(const BufferDesc& desc) {
  const auto handle = next_handle();
  ResourceRecord record;
  record.label = desc.debug_label;
  record.is_texture = false;
  record.size_bytes = static_cast<std::size_t>(desc.size_bytes);
  record.data.assign(record.size_bytes, 0u);
  m_resources.emplace(handle, std::move(record));
  return handle;
}

bool VulkanResourceAllocator::destroy_buffer(ResourceHandle handle) {
  return m_resources.erase(handle) > 0;
}

ResourceHandle VulkanResourceAllocator::create_texture(const TextureDesc& desc) {
  const auto handle = next_handle();
  ResourceRecord record;
  record.label = desc.debug_label;
  record.is_texture = true;
  const auto width = static_cast<std::size_t>(std::max<std::uint32_t>(1, desc.width));
  const auto height = static_cast<std::size_t>(std::max<std::uint32_t>(1, desc.height));
  const auto layers = static_cast<std::size_t>(std::max<std::uint32_t>(1, desc.array_layers));
  constexpr std::size_t kBytesPerTexel = 4u;
  if (height > std::numeric_limits<std::size_t>::max() / width) {
    return kInvalidHandle;
  }
  const auto pixels = width * height;
  if (layers > std::numeric_limits<std::size_t>::max() / pixels) {
    return kInvalidHandle;
  }
  const auto texels = pixels * layers;
  if (texels > std::numeric_limits<std::size_t>::max() / kBytesPerTexel) {
    return kInvalidHandle;
  }
  record.size_bytes = texels * kBytesPerTexel;
  record.data.assign(record.size_bytes, 0u);
  m_resources.emplace(handle, std::move(record));
  return handle;
}

bool VulkanResourceAllocator::destroy_texture(ResourceHandle handle) {
  return m_resources.erase(handle) > 0;
}

bool VulkanResourceAllocator::upload_data(ResourceHandle target, const void* data, std::size_t byte_count) {
  return copy_or_fill_resource(target, data, byte_count);
}

bool VulkanResourceAllocator::readback(ResourceHandle source, void* out_data, std::size_t out_size) const {
  const auto it = m_resources.find(source);
  if (it == m_resources.end()) {
    return false;
  }
  if (!out_data) {
    return false;
  }
  const auto to_copy = std::min(out_size, it->second.size_bytes);
  std::memcpy(out_data, it->second.data.data(), to_copy);
  return true;
}

VulkanDevice::VulkanDevice(std::unique_ptr<VulkanResourceAllocator> allocator,
                           std::unique_ptr<VulkanSwapchain> swapchain,
                           std::uint32_t command_buffer_count)
    : m_allocator(std::move(allocator)),
      m_swapchain(std::move(swapchain)),
      m_commandBufferCount(std::max(2u, command_buffer_count)) {}

bool VulkanDevice::begin() {
  m_running = true;
  m_nextCommandBuffer = 0u;
  return true;
}

bool VulkanDevice::end() {
  m_running = false;
  return true;
}

std::unique_ptr<IRenderCommandContext> VulkanDevice::create_command_context() {
  if (!m_running) {
    return nullptr;
  }
  const std::uint32_t slot = m_nextCommandBuffer++ % m_commandBufferCount;
  return std::make_unique<VulkanCommandContext>(slot);
}

IRenderSwapchain* VulkanDevice::swapchain() const {
  return m_swapchain.get();
}

IRenderResourceAllocator* VulkanDevice::allocator() {
  return m_allocator.get();
}

vkpt::core::Status VulkanComputeBackend::initialize() {
  if (m_initialized) {
    return vkpt::core::Status::ok();
  }
  m_compiler = std::make_unique<VulkanShaderCompiler>();
  m_cache = std::make_unique<VulkanShaderCache>();
  m_initialized = true;
  return vkpt::core::Status::ok();
}

vkpt::core::Status VulkanComputeBackend::shutdown() {
  m_initialized = false;
  m_compiler.reset();
  m_cache.reset();
  m_boundSnapshot.reset();
  m_snapshotState = {};
  m_timeline = {};
  m_nextCommandBuffer = 0u;
  m_lastError.clear();
  return vkpt::core::Status::ok();
}

BackendKind VulkanComputeBackend::kind() const {
  return m_simulated ? BackendKind::VulkanCompute : BackendKind::VulkanRt;
}

std::string VulkanComputeBackend::name() const {
  return m_simulated ? "vulkan-compute (simulated)" : "vulkan";
}

RenderBackendCapabilities VulkanComputeBackend::capabilities() const {
  RenderBackendCapabilities caps;
  caps.backend_name = name();
  caps.compute = true;
  caps.storage_buffers = true;
  caps.storage_textures = true;
  caps.timestamp_queries = false;
  caps.timestamp_fallback_reason =
      "simulated Vulkan adapter does not create a VkQueryPool; timeline values are synthetic";
  caps.subgroups = false;
  caps.descriptor_indexing = false;
  caps.bindless_like_resources = false;
  caps.texture_formats = true;
  caps.ray_tracing = false;
  caps.ray_query = false;
  caps.ray_query_supported = false;
  caps.acceleration_structure_supported = false;
  caps.shader_group_handle_size = 0u;
  caps.max_as_size = 0u;
  caps.presentation = false;
  caps.readback = true;
  caps.is_simulated = true;
  caps.supports_present = false;
  caps.supports_multiqueue = true;
  caps.max_workgroup_size_x = 256u;
  caps.max_workgroup_size_y = 256u;
  caps.max_workgroup_size_z = 64u;
  caps.max_buffer_alignment = 256u;
  caps.memory_model = "simulated-glsl-timeline";
  caps.notes =
      "No external Vulkan SDK required in this gate; simulated backend path. "
      "Snapshot binding, timeline semaphore values, double-buffer command slots, "
      "and multi-tile in-flight submission are modeled for backend parity tests.";
  caps.platform.platform_name = "headless-vulkan-sim";
  caps.platform.headless = true;
  caps.platform.notes =
      "No VkInstance, VkDevice, VkSurfaceKHR, or native handles are exposed; "
      "present is represented by a synthetic timeline value.";
  caps.ray_tracing_caps.unsupported_reason =
      "VK_KHR_acceleration_structure, VK_KHR_ray_tracing_pipeline, and VK_KHR_ray_query are not probed by the simulated adapter";
  caps.shader.glsl = true;
  caps.shader.supported_source_formats = {"glsl"};
  caps.shader.notes = "GLSL compute contract only; SPIR-V compilation is handled by external build tooling when enabled.";
  caps.texture_formats_caps.rgba8_unorm = true;
  caps.texture_formats_caps.rgba16_float = true;
  caps.texture_formats_caps.rgba32_float = true;
  caps.texture_formats_caps.storage_texture_formats = true;
  caps.texture_formats_caps.sampled_texture_formats = true;
  caps.texture_formats_caps.guaranteed_formats = {"RGBA8", "RGBA16F", "RGBA32F"};
  caps.memory_budget.upload_alignment_bytes = 256u;
  caps.memory_budget.readback_alignment_bytes = 256u;
  caps.memory_budget.max_buffer_size_bytes = 1024ull * 1024ull * 1024ull;
  caps.memory_budget.budget_unavailable_reason =
      "VK_EXT_memory_budget is not available because the simulated adapter does not create a physical device";
  return caps;
}

std::unique_ptr<IRenderDevice> VulkanComputeBackend::create_device() {
  if (!m_initialized) {
    return {};
  }
  auto allocator = std::make_unique<VulkanResourceAllocator>();
  auto swapchain = std::make_unique<VulkanSwapchain>(0u, 0u);
  return std::make_unique<VulkanDevice>(
      std::move(allocator),
      std::move(swapchain),
      std::max(2u, m_timeline.command_buffer_count));
}

IShaderCompiler* VulkanComputeBackend::compiler() {
  return m_compiler.get();
}

IShaderCache* VulkanComputeBackend::shader_cache() {
  return m_cache.get();
}

std::unique_ptr<IFrameGraph> VulkanComputeBackend::create_frame_graph() {
  return std::make_unique<FrameGraph>();
}

std::string_view VulkanSnapshotTransitionKindToString(VulkanSnapshotTransitionKind kind) {
  switch (kind) {
    case VulkanSnapshotTransitionKind::None:
      return "none";
    case VulkanSnapshotTransitionKind::InitialUpload:
      return "initial-upload";
    case VulkanSnapshotTransitionKind::Continue:
      return "continue";
    case VulkanSnapshotTransitionKind::ReprojectCamera:
      return "reproject-camera";
    case VulkanSnapshotTransitionKind::RefitTransforms:
      return "refit-transforms";
    case VulkanSnapshotTransitionKind::ReshadeMaterials:
      return "reshade-materials";
    case VulkanSnapshotTransitionKind::RebuildScene:
      return "rebuild-scene";
    default:
      return "unknown";
  }
}

bool VulkanComputeBackend::bind_scene_snapshot(vkpt::scene::RenderSceneSnapshot::Ptr snapshot) {
  if (!m_initialized) {
    m_lastError = "bind_scene_snapshot before initialize";
    return false;
  }
  if (!snapshot) {
    m_lastError = "bind_scene_snapshot received null snapshot";
    return false;
  }

  VulkanSnapshotBindingState state;
  state.snapshot_bound = true;
  state.generation = snapshot->generation;
  state.topology_revision = snapshot->topology_revision;
  state.transform_revision = snapshot->transform_revision;
  state.camera_revision = snapshot->camera_revision;
  state.material_revision = snapshot->material_revision;
  state.bind_count = m_snapshotState.bind_count + 1u;
  state.last_transition = ClassifySnapshotTransition(m_boundSnapshot.get(), *snapshot);
  state.geometry_storage_reused_from_previous =
      m_boundSnapshot != nullptr && SnapshotGeometryStorageReused(*m_boundSnapshot, *snapshot);
  state.acceleration_reused_from_previous = snapshot->acceleration.reused_from_previous;
  state.transform_refit_descriptor =
      snapshot->acceleration.transform_refit_descriptor || !snapshot->instance_motion.empty();
  state.reset_accumulation = TransitionResetsAccumulation(state.last_transition);
  state.rebuild_tile_schedule = TransitionRebuildsTileSchedule(state.last_transition);

  m_boundSnapshot = std::move(snapshot);
  m_snapshotState = state;
  m_lastError.clear();
  return true;
}

VulkanTileBatchDiagnostics VulkanComputeBackend::submit_tile_batch(
    std::span<const vkpt::pathtracer::RenderTile> tiles,
    const VulkanTileBatchConfig& config) {
  VulkanTileBatchDiagnostics result;
  if (!m_initialized) {
    result.error = "submit_tile_batch before initialize";
    m_lastError = result.error;
    return result;
  }
  if (!m_snapshotState.snapshot_bound && !tiles.empty()) {
    result.error = "submit_tile_batch before binding a scene snapshot";
    m_lastError = result.error;
    return result;
  }

  const std::uint32_t commandBufferCount = std::max(2u, config.command_buffer_count);
  const std::uint32_t maxTilesInFlight = std::max(1u, config.max_tiles_in_flight);
  m_timeline.command_buffer_count = commandBufferCount;
  m_timeline.double_buffered_command_buffers = commandBufferCount >= 2u;
  m_timeline.max_tiles_in_flight = maxTilesInFlight;

  struct PendingSubmission {
    std::uint32_t command_buffer_index = 0u;
    std::uint64_t signal_value = 0u;
  };
  std::deque<PendingSubmission> pending;
  std::vector<std::uint64_t> commandBufferSignals(commandBufferCount, 0u);
  std::uint64_t completed = m_timeline.last_completed_value;
  const auto complete_one = [&]() {
    if (pending.empty()) {
      return;
    }
    const auto finished = pending.front();
    pending.pop_front();
    VKP_METRIC_OBSERVE("vkp.gpu.fence_wait_us", 0u);
    completed = std::max(completed, finished.signal_value);
    if (finished.command_buffer_index < commandBufferSignals.size() &&
        commandBufferSignals[finished.command_buffer_index] == finished.signal_value) {
      commandBufferSignals[finished.command_buffer_index] = 0u;
    }
    VKP_LOG_SAMPLED(kTelemetrySamplePeriodNs,
                    Info,
                    "gpu",
                    "dispatch_completed",
                    "backend",
                    "vulkan",
                    "timeline",
                    finished.signal_value,
                    "fence_wait_us",
                    static_cast<std::uint64_t>(0),
                    "pending",
                    static_cast<std::uint64_t>(pending.size()));
  };

  if (!tiles.empty()) {
    result.first_timeline_value = m_timeline.last_submitted_value + 1u;
  }
  result.submissions.reserve(tiles.size());

  for (const auto& tile : tiles) {
    const auto submitStart = std::chrono::steady_clock::now();
    const std::uint32_t slot = m_nextCommandBuffer % commandBufferCount;
    while (!pending.empty() &&
           (pending.size() >= maxTilesInFlight || commandBufferSignals[slot] > completed)) {
      complete_one();
    }

    const std::uint64_t waitValue = completed;
    const std::uint64_t signalValue = m_timeline.last_submitted_value + 1u;
    m_timeline.last_submitted_value = signalValue;
    ++m_timeline.total_tile_submissions;

    commandBufferSignals[slot] = signalValue;
    pending.push_back({slot, signalValue});
    m_nextCommandBuffer = (m_nextCommandBuffer + 1u) % commandBufferCount;

    VulkanTileSubmissionRecord record;
    record.tile_id = tile.tile_id;
    record.gpu_id = tile.gpu_id;
    record.command_buffer_index = slot;
    record.sample_index = tile.sample_index;
    record.frame_index = config.frame_index;
    record.snapshot_generation = m_snapshotState.generation;
    record.wait_timeline_value = waitValue;
    record.signal_timeline_value = signalValue;
    record.in_flight_after_submit = static_cast<std::uint32_t>(pending.size());
    result.max_observed_in_flight =
        std::max(result.max_observed_in_flight, record.in_flight_after_submit);
    result.submissions.push_back(record);
    VKP_LOG_SAMPLED(kTelemetrySamplePeriodNs,
                    Info,
                    "gpu",
                    "dispatch_submitted",
                    "backend",
                    "vulkan",
                    "tile_id",
                    static_cast<std::uint64_t>(tile.tile_id),
                    "frame",
                    config.frame_index,
                    "groups_x",
                    static_cast<std::uint64_t>((tile.width + 7u) / 8u),
                    "groups_y",
                    static_cast<std::uint64_t>((tile.height + 7u) / 8u),
                    "timeline",
                    signalValue,
                    "submit_us",
                    ElapsedUs(submitStart));
  }

  while (!pending.empty()) {
    complete_one();
  }

  result.submitted_tiles = static_cast<std::uint32_t>(result.submissions.size());
  result.last_submitted_timeline_value = m_timeline.last_submitted_value;
  result.completed_timeline_value = completed;
  if (config.present_after_batch && result.submitted_tiles > 0u) {
    m_timeline.last_present_value = completed;
    ++m_timeline.total_presents;
  }
  result.present_timeline_value = m_timeline.last_present_value;
  result.success = true;

  m_timeline.last_completed_value = completed;
  m_timeline.current_in_flight = 0u;
  m_timeline.max_observed_in_flight =
      std::max(m_timeline.max_observed_in_flight, result.max_observed_in_flight);
  m_timeline.cpu_gpu_overlap_observed =
      m_timeline.cpu_gpu_overlap_observed || result.max_observed_in_flight > 1u;
  m_lastError.clear();
  return result;
}

bool RunVulkanComputeSmoke(vkpt::render::IRenderBackend& backend) {
  auto device = backend.create_device();
  if (!device || !device->begin()) {
    return false;
  }
  auto* renderDevice = dynamic_cast<VulkanDevice*>(device.get());
  if (!renderDevice) {
    device->end();
    return false;
  }
  auto* allocator = static_cast<VulkanResourceAllocator*>(renderDevice->allocator());
  if (!allocator) {
    device->end();
    return false;
  }

  BufferDesc patternDesc;
  patternDesc.debug_label = "smoke_compute_pattern";
  patternDesc.size_bytes = 64u;
  patternDesc.usage = ResourceBindingUsage::Storage | ResourceBindingUsage::Read | ResourceBindingUsage::Write;
  const auto sourceBuffer = allocator->create_buffer(patternDesc);
  if (sourceBuffer == kInvalidHandle) {
    device->end();
    return false;
  }

  std::array<std::uint32_t, 16> pattern{};
  std::mt19937 rng(123u);
  for (auto& v : pattern) {
    v = rng();
  }
  if (!allocator->upload_data(sourceBuffer, pattern.data(), pattern.size() * sizeof(std::uint32_t))) {
    allocator->destroy_buffer(sourceBuffer);
    device->end();
    return false;
  }

  // The smoke graph covers write, compute, and readback pass types so the common
  // frame-graph contract is exercised before native Vulkan command recording.
  FrameGraphDesc graphDesc;
  graphDesc.debug_label = "vulkan_compute_smoke";
  graphDesc.passes = {
      {0u, "write", PassType::Copy, {}, {sourceBuffer}},
      {1u, "compute", PassType::Compute, {sourceBuffer}, {sourceBuffer}},
      {2u, "readback", PassType::Readback, {sourceBuffer}, {}},
  };
  graphDesc.dependencies = {{0u, 1u}, {1u, 2u}};
  auto frameGraph = backend.create_frame_graph();
  std::vector<std::string> buildDiagnostics;
  if (!frameGraph->build(graphDesc, &buildDiagnostics)) {
    allocator->destroy_buffer(sourceBuffer);
    device->end();
    return false;
  }
  FrameContext frame;
  frame.frame_index = 0u;
  frame.debug_label = graphDesc.debug_label;
  auto context = device->create_command_context();
  if (!context || !frameGraph->validate(nullptr)) {
    allocator->destroy_buffer(sourceBuffer);
    device->end();
    return false;
  }
  if (!frameGraph->execute(*context, frame)) {
    allocator->destroy_buffer(sourceBuffer);
    device->end();
    return false;
  }
  allocator->destroy_buffer(sourceBuffer);
  device->end();
  return true;
}

// ---------------------------------------------------------------------------
// C10  Vulkan BVH pass (simulated)
// ---------------------------------------------------------------------------

VulkanBVHPassResult RunVulkanBVHPass(
    vkpt::render::VulkanComputeBackend& backend,
    const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
    uint32_t width,
    uint32_t height) {

  VulkanBVHPassResult result;

  if (!backend.initialize().is_ok()) {
    result.error = "backend initialize failed";
    return result;
  }

  auto device = backend.create_device();
  if (!device || !device->begin()) {
    result.error = "device create/begin failed";
    return result;
  }

  auto* raw = dynamic_cast<VulkanDevice*>(device.get());
  if (!raw) {
    device->end();
    result.error = "device cast failed";
    return result;
  }
  auto* allocator = static_cast<VulkanResourceAllocator*>(raw->allocator());

  // -- Upload vertex buffer --------------------------------------------------
  const auto vertexBytes = scene.vertices.size() * sizeof(vkpt::pathtracer::Vec3);
  BufferDesc vbDesc;
  vbDesc.debug_label = "bvh_vertices";
  vbDesc.size_bytes  = std::max<std::size_t>(vertexBytes, 4u);
  vbDesc.usage       = ResourceBindingUsage::Storage | ResourceBindingUsage::Read;
  const auto vertexBuf = allocator->create_buffer(vbDesc);
  if (vertexBuf == kInvalidHandle) {
    device->end();
    result.error = "vertex buffer alloc failed";
    return result;
  }
  if (!scene.vertices.empty()) {
    allocator->upload_data(vertexBuf, scene.vertices.data(), vertexBytes);
  }

  // -- Upload index buffer ---------------------------------------------------
  const auto indexBytes = scene.indices.size() * sizeof(uint32_t);
  BufferDesc ibDesc;
  ibDesc.debug_label = "bvh_indices";
  ibDesc.size_bytes  = std::max<std::size_t>(indexBytes, 4u);
  ibDesc.usage       = ResourceBindingUsage::Storage | ResourceBindingUsage::Read;
  const auto indexBuf = allocator->create_buffer(ibDesc);
  if (indexBuf == kInvalidHandle) {
    allocator->destroy_buffer(vertexBuf);
    device->end();
    result.error = "index buffer alloc failed";
    return result;
  }
  if (!scene.indices.empty()) {
    allocator->upload_data(indexBuf, scene.indices.data(), indexBytes);
  }

  // -- Simulate BVH build ----------------------------------------------------
  // Estimate SAH BVH node count: for N triangles, worst-case is 2N-1 nodes.
  const auto triangleCount = static_cast<uint32_t>(scene.indices.size() / 3);
  const uint32_t bvhNodes  = triangleCount > 0 ? 2 * triangleCount - 1 : 0;

  BufferDesc bvhDesc;
  bvhDesc.debug_label = "bvh_nodes";
  bvhDesc.size_bytes  = std::max<std::size_t>(bvhNodes * 32u, 4u);  // 32 bytes/node (AABB + child ids)
  bvhDesc.usage       = ResourceBindingUsage::Storage | ResourceBindingUsage::Read;
  const auto bvhBuf = allocator->create_buffer(bvhDesc);
  if (bvhBuf == kInvalidHandle) {
    allocator->destroy_buffer(indexBuf);
    allocator->destroy_buffer(vertexBuf);
    device->end();
    result.error = "bvh node buffer alloc failed";
    return result;
  }
  // Fill with zeros to initialise the simulated BVH buffer.
  {
    std::vector<uint8_t> zeros(bvhDesc.size_bytes, 0u);
    allocator->upload_data(bvhBuf, zeros.data(), zeros.size());
  }

  // -- Allocate output film texture ------------------------------------------
  TextureDesc filmDesc;
  filmDesc.debug_label  = "pt_film";
  filmDesc.width        = std::max<uint32_t>(width, 1u);
  filmDesc.height       = std::max<uint32_t>(height, 1u);
  filmDesc.array_layers = 1u;
  filmDesc.usage        = ResourceBindingUsage::Storage | ResourceBindingUsage::Write;
  const auto filmTex = allocator->create_texture(filmDesc);
  if (filmTex == kInvalidHandle) {
    allocator->destroy_buffer(bvhBuf);
    allocator->destroy_buffer(indexBuf);
    allocator->destroy_buffer(vertexBuf);
    device->end();
    result.error = "film texture alloc failed";
    return result;
  }

  // -- Build frame graph and execute -----------------------------------------
  // The simulated BVH path records the intended pass ordering that a native
  // Vulkan backend will later lower into transfer, compute, and readback queues.
  FrameGraphDesc graphDesc;
  graphDesc.debug_label = "vulkan_bvh_pathtrace";
  graphDesc.target_width = std::max<uint32_t>(width, 1u);
  graphDesc.target_height = std::max<uint32_t>(height, 1u);
  graphDesc.passes = {
      {0u, "bvh_upload", PassType::Copy, {}, {vertexBuf, indexBuf, bvhBuf}},
      {1u, "bvh_build", PassType::Compute, {vertexBuf, indexBuf}, {bvhBuf}},
      {2u, "pathtracer", PassType::Compute, {bvhBuf, vertexBuf, indexBuf}, {filmTex}},
      {3u, "film_resolve", PassType::Readback, {filmTex}, {}},
  };
  graphDesc.dependencies = {{0u, 1u}, {1u, 2u}, {2u, 3u}};
  auto fg = backend.create_frame_graph();
  std::vector<std::string> validateDiagnostics;
  if (!fg->build(graphDesc, &validateDiagnostics)) {
    allocator->destroy_texture(filmTex);
    allocator->destroy_buffer(bvhBuf);
    allocator->destroy_buffer(indexBuf);
    allocator->destroy_buffer(vertexBuf);
    device->end();
    result.error = "frame graph validation failed";
    if (!validateDiagnostics.empty()) { result.error += ": " + validateDiagnostics.front(); }
    return result;
  }

  auto ctx = device->create_command_context();
  FrameContext frame;
  frame.frame_index = 0u;
  frame.viewport_width = graphDesc.target_width;
  frame.viewport_height = graphDesc.target_height;
  frame.output_target = filmTex;
  frame.readback_target = filmTex;
  frame.debug_label = graphDesc.debug_label;
  if (!ctx || !fg->execute(*ctx, frame)) {
    allocator->destroy_texture(filmTex);
    allocator->destroy_buffer(bvhBuf);
    allocator->destroy_buffer(indexBuf);
    allocator->destroy_buffer(vertexBuf);
    device->end();
    result.error = "frame graph execute failed";
    return result;
  }

  // -- Populate result -------------------------------------------------------
  result.success             = true;
  result.vertex_buffer_count = static_cast<uint32_t>(scene.vertices.size());
  result.index_buffer_count  = static_cast<uint32_t>(scene.indices.size());
  result.instance_count      = static_cast<uint32_t>(scene.instances.size());
  result.bvh_node_estimate   = bvhNodes;

  allocator->destroy_texture(filmTex);
  allocator->destroy_buffer(bvhBuf);
  allocator->destroy_buffer(indexBuf);
  allocator->destroy_buffer(vertexBuf);
  device->end();
  return result;
}

}  // namespace vkpt::render
