#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "render/interface/RenderContracts.h"

namespace vkpt::render {

std::vector<std::string> AvailableBackendNames();
std::unique_ptr<IRenderBackend> CreateBackend(std::string_view backend_name);
std::string NormalizeBackendName(std::string_view backend_name);

}  // namespace vkpt::render

