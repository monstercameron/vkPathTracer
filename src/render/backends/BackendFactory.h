#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "render/interface/RenderContracts.h"

namespace vkpt::render {

/// Return user-facing backend aliases accepted by the render CLI/config layer.
std::vector<std::string> AvailableBackendNames();
/// Describe every known backend, including unavailable or uncompiled candidates.
std::vector<BackendCandidateDesc> DescribeBackendCandidates();
/// Select a backend according to explicit/config/platform preferences and policy flags.
BackendSelectionDecision SelectBackend(const BackendSelectionRequest& request);
/// Create a backend instance by alias; returns null when no compatible backend exists.
std::unique_ptr<IRenderBackend> CreateBackend(std::string_view backend_name);
/// Canonicalize a backend alias for selection and factory matching.
std::string NormalizeBackendName(std::string_view backend_name);

}  // namespace vkpt::render

