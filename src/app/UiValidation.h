#pragma once

#include "editor/UiModels.h"

#include <vector>

namespace vkpt::app {

bool RunUiModelSmokeTests();
std::vector<vkpt::editor::UiReleaseGateItem> BuildUiReleaseGateEvidence();
bool RunUiReleaseGateCheck(bool json);

}  // namespace vkpt::app
