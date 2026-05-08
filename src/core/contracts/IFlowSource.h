#pragma once

#include <cstdint>

namespace vkpt::core::contracts {

class IFlowSource {
 public:
  virtual ~IFlowSource() = default;
  virtual std::uint64_t current_flow_id() const noexcept = 0;
};

}  // namespace vkpt::core::contracts
