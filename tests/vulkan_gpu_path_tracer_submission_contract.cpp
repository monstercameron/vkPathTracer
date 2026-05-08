#include "gpu/VulkanGpuPathTracer.h"

#include <iostream>
#include <string_view>
#include <type_traits>
#include <utility>

// This target deliberately links only the native Vulkan path tracer and its ABI
// dependencies. Keep the observability implementation available here so adding
// GPU telemetry to VulkanGpuPathTracer does not require broadening the target.
#include "core/log/Log.cpp"
#include "core/metrics/Metrics.cpp"

namespace {

bool Check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "vulkan_gpu_path_tracer_submission_contract: " << message << '\n';
    return false;
  }
  return true;
}

class FakeGpuBackend final : public vkpt::gpu::IGpuBackendIntrospect {
 public:
  vkpt::gpu::GpuBackendIntrospection introspect() const override {
    vkpt::gpu::GpuBackendIntrospection info;
    info.adapter_name = "fake";
    info.vram_bytes_used = 91u;
    info.vram_bytes_total = 100u;
    info.current_flow_id = 42u;
    return info;
  }
};

}  // namespace

int main() {
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::pathtracer::IPathTracer&>()
                             .request_film_readback()),
                vkpt::pathtracer::FilmReadbackToken>);
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::pathtracer::IPathTracer&>()
                             .poll_film(vkpt::pathtracer::FilmReadbackToken{})),
                vkpt::pathtracer::FilmReadbackResult>);
  static_assert(std::is_same_v<
                decltype(std::declval<vkpt::gpu::VulkanGpuPathTracer&>().status()),
                vkpt::pathtracer::PathTracerStatus>);

  const auto contract = vkpt::gpu::VulkanGpuPathTracer::submission_contract();

  if (!Check(sizeof(vkpt::gpu::PathTracePushConstants) == 128u,
             "push constants must remain shader ABI compatible") ||
      !Check(vkpt::gpu::kPathTracePushConstantByteSize == 128u,
             "push constants should derive their ABI size from the shared schema") ||
      !Check(vkpt::gpu::kPathTracePushConstantWordCount == 32u,
             "shared push-constant schema should stay at 32 dwords") ||
      !Check(contract.command_buffer_count >= 2u,
             "native Vulkan path tracer should keep multiple command buffers") ||
      !Check(contract.max_tiles_in_flight == contract.command_buffer_count,
             "in-flight tile limit should match the command-buffer ring") ||
      !Check(contract.workgroup_size_x == 8u && contract.workgroup_size_y == 8u,
             "contract should match pathtrace.comp local_size") ||
      !Check(contract.tile_height_rows >= contract.workgroup_size_y,
             "tile height should contain at least one workgroup") ||
      !Check(contract.tile_height_rows % contract.workgroup_size_y == 0u,
             "tile height should be workgroup aligned") ||
      !Check(contract.uses_timeline_semaphore,
             "native Vulkan path tracer should use timeline semaphore submission") ||
      !Check(contract.uses_dispatch_base_tiles,
             "native Vulkan path tracer should use dispatch-base row tiles") ||
      !Check(contract.waits_once_after_tile_batch,
             "CPU resolve contract should wait after the tile batch, not per tile") ||
      !Check(contract.row_tiles_are_workgroup_aligned,
             "row tile contract should document workgroup alignment") ||
      !Check(contract.emits_shader_compiled_event,
             "native Vulkan path tracer should emit shader compile telemetry") ||
      !Check(contract.emits_dispatch_events,
             "native Vulkan path tracer should emit dispatch submit/complete telemetry") ||
      !Check(contract.records_fence_wait_histogram,
             "native Vulkan path tracer should record fence wait histogram samples")) {
    return 1;
  }
  if (!Check(contract.records_device_memory_gauge,
             "native Vulkan path tracer should publish GPU memory telemetry") ||
      !Check(contract.exposes_introspection,
             "native Vulkan path tracer should expose backend introspection") ||
      !Check(contract.exposes_health_probe_contract,
             "native Vulkan path tracer should expose GPU health contract") ||
      !Check(contract.exposes_split_film_readback &&
                 contract.film_readback_token_carries_timeline,
             "native Vulkan path tracer should expose token/poll film readback") ||
      !Check(contract.init_device_returns_status,
             "native Vulkan path tracer init_device should return Status")) {
    return 1;
  }
  vkpt::gpu::GpuBackendIntrospection info;
  info.adapter_name = "contract";
  info.vram_bytes_used = 91u;
  info.vram_bytes_total = 100u;
  info.current_flow_id = 7u;
  if (!Check(vkpt::gpu::GpuHealthReportFromIntrospection(info).status ==
                 vkpt::core::health::Status::Degraded,
             "GPU health contract should degrade above 90 percent VRAM")) {
    return 1;
  }
  info.lifecycle = vkpt::core::contracts::ComponentLifecycle::Failed;
  info.last_error = "device lost";
  if (!Check(vkpt::gpu::GpuHealthReportFromIntrospection(info).status ==
                 vkpt::core::health::Status::Failed,
             "GPU health contract should fail on backend lifecycle failure")) {
    return 1;
  }
  FakeGpuBackend fakeBackend;
  const auto gpuProbe = vkpt::gpu::CreateGpuBackendHealthProbe(fakeBackend);
  if (!Check(gpuProbe && gpuProbe->name() == "gpu",
             "GPU health probe factory should produce a named probe") ||
      !Check(gpuProbe->check().status == vkpt::core::health::Status::Degraded,
             "GPU health probe should use backend introspection")) {
    return 1;
  }

  std::cout << "vulkan_gpu_path_tracer_submission_contract: ok\n";
  return 0;
}
