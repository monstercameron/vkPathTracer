#include "render/backends/BackendFactory.h"

#include <algorithm>
#include <cctype>

#include "render/backends/AdapterBackends.h"
#if defined(PT_ENABLE_D3D12)
#include "render/backends/D3D12Backend.h"
#endif
#include "render/backends/NullBackend.h"
#include "render/backends/VulkanBackend.h"

namespace vkpt::render {

namespace {

std::string NormalizeInternal(std::string_view name) {
  std::string normalized;
  normalized.reserve(name.size());
  for (const char ch : name) {
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower != ' ' && lower != '_') {
      normalized.push_back(lower);
    }
  }
  return normalized;
}

BackendCandidateDesc MakeCandidate(std::string name,
                                   BackendKind kind,
                                   std::uint32_t priority,
                                   bool compiled,
                                   bool available,
                                   bool adapter_skeleton,
                                   bool experimental,
                                   bool compute,
                                   bool presentation,
                                   bool ray_tracing,
                                   std::string unavailable_reason = {}) {
  BackendCandidateDesc desc;
  desc.name = std::move(name);
  desc.kind = kind;
  desc.selection_priority = priority;
  desc.compiled = compiled;
  desc.available = available;
  desc.adapter_skeleton = adapter_skeleton;
  desc.experimental = experimental;
  desc.supports_compute = compute;
  desc.supports_presentation = presentation;
  desc.supports_ray_tracing = ray_tracing;
  desc.unavailable_reason = std::move(unavailable_reason);
  return desc;
}

bool CandidateMatches(const BackendCandidateDesc& candidate, std::string_view normalized_name) {
  return NormalizeBackendName(candidate.name) == normalized_name;
}

bool CandidateMeetsRequest(const BackendCandidateDesc& candidate,
                           const BackendSelectionRequest& request,
                           BackendSelectionSource source,
                           std::string* reason) {
  // Keep rejection reasons close to the predicate so backend selection reports
  // the same contract checks that factory creation will enforce.
  if (!candidate.compiled) {
    if (reason) {
      *reason = candidate.unavailable_reason.empty() ? "backend not compiled into this build" : candidate.unavailable_reason;
    }
    return false;
  }
  if (!candidate.available) {
    if (reason) {
      *reason = candidate.unavailable_reason.empty() ? "backend unavailable" : candidate.unavailable_reason;
    }
    return false;
  }
  if (candidate.adapter_skeleton && !request.allow_adapter_skeleton) {
    if (reason) {
      *reason = "adapter skeletons are disabled by selection policy";
    }
    return false;
  }
  if (candidate.experimental && !request.allow_experimental) {
    if (reason) {
      *reason = "experimental backends are disabled by selection policy";
    }
    return false;
  }
  if (request.require_compute && !candidate.supports_compute) {
    if (reason) {
      *reason = "compute support required";
    }
    return false;
  }
  if (request.require_presentation && !candidate.supports_presentation) {
    if (reason) {
      *reason = "presentation support required";
    }
    return false;
  }
  if (request.require_ray_tracing && !candidate.supports_ray_tracing) {
    if (reason) {
      *reason = "hardware ray tracing support required";
    }
    return false;
  }
  if (candidate.kind == BackendKind::Null && source != BackendSelectionSource::Explicit && !request.allow_null_fallback) {
    if (reason) {
      *reason = "null backend fallback is disabled for render workloads";
    }
    return false;
  }
  return true;
}

bool TrySelectNamed(const std::vector<BackendCandidateDesc>& candidates,
                    std::string_view name,
                    const BackendSelectionRequest& request,
                    BackendSelectionSource source,
                    BackendSelectionDecision& decision,
                    bool terminal_failure) {
  if (name.empty()) {
    return false;
  }
  const std::string normalized = NormalizeBackendName(name);
  const auto it = std::find_if(candidates.begin(), candidates.end(), [&](const BackendCandidateDesc& candidate) {
    return CandidateMatches(candidate, normalized);
  });
  if (it == candidates.end()) {
    decision.diagnostics.push_back(std::string(BackendSelectionSourceToString(source)) + " backend '" +
                                   std::string(name) + "' is unknown");
    if (terminal_failure) {
      decision.reason = "requested backend is unknown: " + std::string(name);
    }
    return false;
  }

  std::string rejection;
  if (!CandidateMeetsRequest(*it, request, source, &rejection)) {
    decision.diagnostics.push_back(std::string(BackendSelectionSourceToString(source)) + " backend '" +
                                   it->name + "' rejected: " + rejection);
    if (terminal_failure) {
      decision.reason = "requested backend rejected: " + rejection;
    }
    return false;
  }

  decision.selected = true;
  decision.selected_backend = it->name;
  decision.selected_kind = it->kind;
  decision.source = source;
  decision.reason = "selected by " + std::string(BackendSelectionSourceToString(source));
  return true;
}

#if defined(PT_ENABLE_D3D12)
bool HasD3D12RayTracingAccelerator() {
  static const bool has_dxr = []() {
    const auto accelerators = EnumerateD3D12Accelerators(false, false);
    return std::any_of(accelerators.begin(), accelerators.end(), [](const AcceleratorCapabilities& accel) {
      return accel.available && accel.compute && accel.ray_tracing && !accel.warp;
    });
  }();
  return has_dxr;
}
#endif

}  // namespace

std::string NormalizeBackendName(std::string_view backend_name) {
  if (backend_name.empty()) {
    return "auto";
  }
  auto normalized = NormalizeInternal(backend_name);
  std::string out{normalized};
  if (out == "auto" || out == "default") {
    out = "auto";
  } else if (out == "vulkan" || out == "vulkancompute" || out == "vulkan-compute") {
    out = "vulkan";
  } else if (out == "vulkanrt" || out == "vulkan-rt") {
    out = "vulkan-rt";
  } else if (out == "dxr" || out == "d3d12dxr" || out == "d3d12-dxr") {
    out = "d3d12-dxr";
  } else if (out == "metalrt" || out == "metal-rt") {
    out = "metal";
  } else if (out == "webgpuwgsl" || out == "webgpu-wgsl") {
    out = "webgpu";
  } else if (out == "opengl" || out == "gl" || out == "openglexperimental") {
    out = "opengl-experimental";
  }
  return out;
}

std::vector<BackendCandidateDesc> DescribeBackendCandidates() {
  std::vector<BackendCandidateDesc> candidates;
  candidates.push_back(MakeCandidate("vulkan", BackendKind::VulkanCompute, 10u, true, true, true, false, true, false, false));

#if defined(PT_ENABLE_D3D12)
  const bool has_d3d12_dxr = HasD3D12RayTracingAccelerator();
  candidates.push_back(MakeCandidate("d3d12", BackendKind::D3d12, 20u, true, true, true, false, true, false, false));
  candidates.push_back(MakeCandidate("d3d12-dxr", BackendKind::D3d12, 21u, true, has_d3d12_dxr, true, false, true, false,
                                     has_d3d12_dxr,
                                     has_d3d12_dxr ? "" : "No DXR-capable D3D12 accelerator found"));
#else
  candidates.push_back(MakeCandidate("d3d12", BackendKind::D3d12, 20u, false, false, true, false, true, false, false,
                                     "PT_ENABLE_D3D12 is disabled"));
  candidates.push_back(MakeCandidate("d3d12-dxr", BackendKind::D3d12, 21u, false, false, true, false, true, false, false,
                                     "PT_ENABLE_D3D12 is disabled"));
#endif

#if defined(PT_ENABLE_METAL)
  candidates.push_back(MakeCandidate("metal", BackendKind::Metal, 30u, true, true, true, false, true, false, false));
#else
  candidates.push_back(MakeCandidate("metal", BackendKind::Metal, 30u, false, false, true, false, true, false, false,
                                     "PT_ENABLE_METAL is disabled"));
#endif

#if defined(PT_ENABLE_WEBGPU)
  candidates.push_back(MakeCandidate("webgpu", BackendKind::WebGpu, 40u, true, true, true, false, true, false, false));
#else
  candidates.push_back(MakeCandidate("webgpu", BackendKind::WebGpu, 40u, false, false, true, false, true, false, false,
                                     "PT_ENABLE_WEBGPU is disabled"));
#endif

#if defined(PT_ENABLE_OPENGL_EXPERIMENTAL)
  candidates.push_back(MakeCandidate("opengl-experimental", BackendKind::OpenGLExperimental, 90u, true, true, true, true,
                                     true, false, false));
#else
  candidates.push_back(MakeCandidate("opengl-experimental", BackendKind::OpenGLExperimental, 90u, false, false, true, true,
                                     true, false, false, "PT_ENABLE_OPENGL_EXPERIMENTAL is disabled"));
#endif

  candidates.push_back(MakeCandidate("null", BackendKind::Null, 1000u, true, true, true, false, true, false, false));
  return candidates;
}

BackendSelectionDecision SelectBackend(const BackendSelectionRequest& request) {
  BackendSelectionDecision decision;
  decision.candidates = DescribeBackendCandidates();

  // Explicit requests are terminal: callers asked for a specific backend, so an
  // unknown or incompatible explicit name should not silently fall through.
  if (TrySelectNamed(decision.candidates,
                     request.explicit_backend,
                     request,
                     BackendSelectionSource::Explicit,
                     decision,
                     true)) {
    return decision;
  }
  if (!request.explicit_backend.empty()) {
    return decision;
  }

  if (TrySelectNamed(decision.candidates,
                     request.config_backend,
                     request,
                     BackendSelectionSource::Config,
                     decision,
                     false)) {
    return decision;
  }

  if (TrySelectNamed(decision.candidates,
                     request.platform_preferred_backend,
                     request,
                     BackendSelectionSource::PlatformPreferred,
                     decision,
                     false)) {
    return decision;
  }

  auto sorted = decision.candidates;
  std::sort(sorted.begin(), sorted.end(), [](const BackendCandidateDesc& lhs, const BackendCandidateDesc& rhs) {
    return lhs.selection_priority < rhs.selection_priority;
  });
  // Lower priority numbers are preferred. Null is only considered here when the
  // request explicitly allows a simulated fallback.
  for (const auto& candidate : sorted) {
    if (candidate.kind == BackendKind::Null && !request.allow_null_fallback) {
      continue;
    }
    std::string rejection;
    if (!CandidateMeetsRequest(candidate, request, BackendSelectionSource::FirstCompatible, &rejection)) {
      decision.diagnostics.push_back(candidate.name + " rejected: " + rejection);
      continue;
    }
    decision.selected = true;
    decision.selected_backend = candidate.name;
    decision.selected_kind = candidate.kind;
    decision.source = candidate.kind == BackendKind::Null ? BackendSelectionSource::NullFallback
                                                          : BackendSelectionSource::FirstCompatible;
    decision.reason = candidate.kind == BackendKind::Null ? "selected null fallback"
                                                          : "selected first compatible backend";
    return decision;
  }

  decision.reason = "no compatible backend found";
  return decision;
}

std::vector<std::string> AvailableBackendNames() {
  std::vector<std::string> names = {"auto", "null", "vulkan", "vulkan-compute"};
#if defined(PT_ENABLE_D3D12)
  names.push_back("d3d12");
  names.push_back("d3d12-dxr");
#endif
#if defined(PT_ENABLE_METAL)
  names.push_back("metal");
#endif
#if defined(PT_ENABLE_WEBGPU)
  names.push_back("webgpu");
  names.push_back("webgpu-wgsl");
#endif
#if defined(PT_ENABLE_OPENGL_EXPERIMENTAL)
  names.push_back("opengl-experimental");
#endif
  return names;
}

std::unique_ptr<IRenderBackend> CreateBackend(std::string_view backend_name) {
  const std::string normalized = NormalizeBackendName(backend_name);
  if (normalized == "auto" || normalized == "default") {
    BackendSelectionRequest request;
    const auto decision = SelectBackend(request);
    if (!decision.selected) {
      return {};
    }
    return CreateBackend(decision.selected_backend);
  }
  if (normalized == "null") {
    return std::make_unique<NullBackend>();
  }
  if (normalized == "vulkan" || normalized == "vulkan-compute") {
    return std::make_unique<VulkanComputeBackend>();
  }
  if (normalized == "vulkan-rt") {
    return {};
  }
#if defined(PT_ENABLE_D3D12)
  if (normalized == "d3d12" || normalized == "d3d12-dxr") {
    return std::make_unique<D3D12Backend>();
  }
#endif
#if defined(PT_ENABLE_METAL)
  if (normalized == "metal") {
    return CreateMetalBackendSkeleton();
  }
#endif
#if defined(PT_ENABLE_WEBGPU)
  if (normalized == "webgpu") {
    return CreateWebGpuBackendSkeleton();
  }
#endif
#if defined(PT_ENABLE_OPENGL_EXPERIMENTAL)
  if (normalized == "opengl-experimental") {
    return CreateOpenGLExperimentalBackendSkeleton();
  }
#endif
  return {};
}

}  // namespace vkpt::render
