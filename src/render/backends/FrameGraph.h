#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "core/Types.h"
#include "render/interface/RenderContracts.h"

namespace vkpt::render {

class FrameGraph final : public IFrameGraph {
 public:
  std::uint32_t add_pass(std::string_view name,
                         PassType type,
                         std::vector<ResourceHandle> reads,
                         std::vector<ResourceHandle> writes) override;

  bool add_dependency(std::uint32_t from, std::uint32_t to) override;
  bool validate(std::vector<std::string>* diagnostics) const override;
  bool execute(IRenderCommandContext& context,
               const std::vector<std::uint32_t>* execution_order = nullptr) const override;

  const std::vector<Pass>& passes() const override { return m_passes; }
  const std::vector<std::pair<std::uint32_t, std::uint32_t>>& dependencies() const override { return m_dependencies; }

 private:
 bool has_path(std::uint32_t from, std::uint32_t to, std::vector<bool>& visited) const;
  bool topo_sort(std::vector<std::uint32_t>& out, std::vector<std::string>* diagnostics) const;

  std::vector<Pass> m_passes;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> m_dependencies;
};

}  // namespace vkpt::render
