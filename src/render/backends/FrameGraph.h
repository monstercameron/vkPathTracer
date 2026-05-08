#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "core/Types.h"
#include "render/interface/RenderContracts.h"

namespace vkpt::render {

/// Default frame-graph implementation used by simulated and adapter backends.
///
/// Pass ids are dense indices assigned at insertion/build time. Dependencies are
/// directed edges from producer/ordering predecessor to consumer/successor.
class FrameGraph final : public IFrameGraph {
 public:
  /// Add a pass and return the dense pass id assigned by this graph.
  std::uint32_t add_pass(std::string_view name,
                         PassType type,
                         std::vector<ResourceHandle> reads,
                         std::vector<ResourceHandle> writes) override;

  /// Replace the graph contents from a descriptor and optionally validate hazards.
  bool build(const FrameGraphDesc& desc, std::vector<std::string>* diagnostics = nullptr) override;
  /// Add an ordering edge from `from` to `to`; rejects invalid ids and duplicates.
  bool add_dependency(std::uint32_t from, std::uint32_t to) override;
  /// Validate dependency ids, cycles, and resource read/write hazards.
  bool validate(std::vector<std::string>* diagnostics) const override;
  /// Execute the graph with an empty frame context.
  FrameGraphResult execute(
      IRenderCommandContext& context,
      const std::vector<std::uint32_t>* execution_order = nullptr) const override;
  /// Execute passes in topological order unless a checked explicit order is supplied.
  FrameGraphResult execute(
      IRenderCommandContext& context,
      const FrameContext& frame,
      const std::vector<std::uint32_t>* execution_order = nullptr) const override;

  const std::vector<Pass>& passes() const override { return m_passes; }
  const std::vector<std::pair<std::uint32_t, std::uint32_t>>& dependencies() const override { return m_dependencies; }

 private:
  /// Return true when an ordering path exists from `from` to `to`.
  bool has_path(std::uint32_t from, std::uint32_t to, std::vector<bool>& visited) const;
  /// Compute Kahn topological order and report cycles or invalid dependency ids.
  bool topo_sort(std::vector<std::uint32_t>& out, std::vector<std::string>* diagnostics) const;

  std::vector<Pass> m_passes;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> m_dependencies;
};

}  // namespace vkpt::render
