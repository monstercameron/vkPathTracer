#include "render/backends/D3D12Backend.h"

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace vkpt::render {

bool RunD3D12ComputeSmoke(vkpt::render::IRenderBackend& backend) {
  auto device = backend.create_device();
  if (!device || !device->begin()) {
    return false;
  }
  auto* renderDevice = dynamic_cast<D3D12Device*>(device.get());
  if (!renderDevice) {
    device->end();
    return false;
  }
  auto* allocator = static_cast<D3D12ResourceAllocator*>(renderDevice->allocator());
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
  std::mt19937 rng(456u);
  for (auto& v : pattern) {
    v = rng();
  }
  if (!allocator->upload_data(sourceBuffer, pattern.data(), pattern.size() * sizeof(std::uint32_t))) {
    allocator->destroy_buffer(sourceBuffer);
    device->end();
    return false;
  }

  // The smoke graph exercises copy, compute, and readback pass ordering through
  // the backend-neutral frame graph before native D3D12 command recording exists.
  FrameGraphDesc graphDesc;
  graphDesc.debug_label = "d3d12_compute_smoke";
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


}  // namespace vkpt::render
