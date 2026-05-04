#include "render/backends/BackendFactory.h"

#include <cctype>

#include "render/backends/D3D12Backend.h"
#include "render/backends/NullBackend.h"
#include "render/backends/VulkanBackend.h"

namespace vkpt::render {

namespace {

std::string NormalizeInternal(std::string_view name) {
  std::string normalized;
  normalized.reserve(name.size());
  for (const char ch : name) {
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower != ' ') {
      normalized.push_back(lower);
    }
  }
  return normalized;
}

}  // namespace

std::string NormalizeBackendName(std::string_view backend_name) {
  if (backend_name.empty()) {
    return "auto";
  }
  auto normalized = NormalizeInternal(backend_name);
  std::string out{normalized};
  if (out == "auto" || out == "default") {
    out = "auto";
  }
  if (out == "vulkancompute") {
    out = "vulkan";
  }
  if (out == "dxr" || out == "d3d12dxr") {
    out = "d3d12-dxr";
  }
  return out;
}

std::vector<std::string> AvailableBackendNames() {
  return {"auto", "null", "vulkan", "vulkan-compute", "d3d12", "d3d12-dxr"};
}

std::unique_ptr<IRenderBackend> CreateBackend(std::string_view backend_name) {
  const std::string normalized = NormalizeBackendName(backend_name);
  if (normalized == "null") {
    return std::make_unique<NullBackend>();
  }
  if (normalized == "vulkan" || normalized == "vulkan-compute") {
    return std::make_unique<VulkanComputeBackend>();
  }
  if (normalized == "d3d12" || normalized == "d3d12-dxr") {
    return std::make_unique<D3D12Backend>();
  }
  if (normalized == "auto" || normalized == "default") {
    auto backend = std::make_unique<VulkanComputeBackend>();
    backend->initialize();
    return backend;
  }
  return {};
}

}  // namespace vkpt::render
