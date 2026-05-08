#include "render/backends/FrameGraph.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <utility>

namespace vkpt::render {
namespace {

FrameGraphResult FrameGraphFailure(vkpt::core::StatusCode code, std::string message) {
  FrameGraphResult result;
  result.overall = vkpt::core::Status::error(code, std::move(message));
  return result;
}

}  // namespace

std::uint32_t FrameGraph::add_pass(std::string_view name,
                                   PassType type,
                                   std::vector<ResourceHandle> reads,
                                   std::vector<ResourceHandle> writes) {
  Pass pass;
  pass.id = static_cast<std::uint32_t>(m_passes.size());
  pass.name = std::string(name);
  pass.type = type;
  pass.reads = std::move(reads);
  pass.writes = std::move(writes);
  m_passes.push_back(std::move(pass));
  return static_cast<std::uint32_t>(m_passes.size() - 1);
}

bool FrameGraph::build(const FrameGraphDesc& desc, std::vector<std::string>* diagnostics) {
  if (diagnostics) {
    diagnostics->clear();
  }
  m_passes.clear();
  m_dependencies.clear();
  m_passes.reserve(desc.passes.size());
  m_dependencies.reserve(desc.dependencies.size());

  // Re-number descriptor passes into this graph's dense id space; dependency ids
  // are expected to reference the descriptor order.
  for (const auto& pass : desc.passes) {
    (void)pass.id;
    add_pass(pass.name, pass.type, pass.reads, pass.writes);
  }

  for (const auto& dep : desc.dependencies) {
    if (!add_dependency(dep.first, dep.second)) {
      if (diagnostics) {
        diagnostics->push_back("invalid dependency in frame graph desc: " +
                               std::to_string(dep.first) + " -> " + std::to_string(dep.second));
      }
      return false;
    }
  }

  if (!desc.validate_hazards) {
    return true;
  }
  return validate(diagnostics);
}

bool FrameGraph::add_dependency(std::uint32_t from, std::uint32_t to) {
  if (from >= m_passes.size() || to >= m_passes.size() || from == to) {
    return false;
  }
  const auto duplicate = std::find_if(
      m_dependencies.begin(), m_dependencies.end(),
      [&](const auto& edge) { return edge.first == from && edge.second == to; });
  if (duplicate != m_dependencies.end()) {
    return false;
  }
  m_dependencies.emplace_back(from, to);
  return true;
}

bool FrameGraph::has_path(std::uint32_t from, std::uint32_t to, std::vector<bool>& visited) const {
  if (from == to) {
    return true;
  }
  if (visited[from]) {
    return false;
  }
  visited[from] = true;
  for (const auto& dep : m_dependencies) {
    if (dep.first != from) {
      continue;
    }
    if (has_path(dep.second, to, visited)) {
      return true;
    }
  }
  return false;
}

bool FrameGraph::topo_sort(std::vector<std::uint32_t>& out, std::vector<std::string>* diagnostics) const {
  out.clear();
  if (m_passes.empty()) {
    return true;
  }
  out.reserve(m_passes.size());

  std::vector<std::uint32_t> indegree(m_passes.size(), 0);
  for (const auto& dep : m_dependencies) {
    if (dep.first >= m_passes.size() || dep.second >= m_passes.size()) {
      if (diagnostics) {
        diagnostics->push_back("dependency references unknown pass");
      }
      return false;
    }
    ++indegree[dep.second];
  }

  std::queue<std::uint32_t> ready;
  for (std::uint32_t i = 0; i < m_passes.size(); ++i) {
    if (indegree[i] == 0) {
      ready.push(i);
    }
  }

  while (!ready.empty()) {
    const auto id = ready.front();
    ready.pop();
    out.push_back(id);
    for (const auto& dep : m_dependencies) {
      if (dep.first == id) {
        if (--indegree[dep.second] == 0) {
          ready.push(dep.second);
        }
      }
    }
  }

  // Kahn's algorithm leaves nodes unvisited only when every remaining node is
  // blocked by a cycle.
  if (out.size() != m_passes.size()) {
    if (diagnostics) {
      diagnostics->push_back("frame graph has dependency cycle");
    }
    return false;
  }
  return true;
}

bool FrameGraph::validate(std::vector<std::string>* diagnostics) const {
  if (diagnostics) {
    diagnostics->clear();
  }
  std::vector<std::uint32_t> order;
  if (!topo_sort(order, diagnostics)) {
    return false;
  }

  std::unordered_map<ResourceHandle, std::uint32_t> lastWriter;
  std::unordered_map<ResourceHandle, std::vector<std::uint32_t>> lastReaders;
  std::size_t resourceRefCount = 0u;
  for (const auto& pass : m_passes) {
    resourceRefCount += pass.reads.size() + pass.writes.size();
  }
  lastWriter.reserve(resourceRefCount);
  lastReaders.reserve(resourceRefCount);
  std::vector<bool> visited(m_passes.size(), false);

  // Hazard validation walks topological order while remembering the most recent
  // writer and still-unordered readers for each resource. Every read-after-write,
  // write-after-write, and write-after-read relationship must have an explicit
  // dependency path.
  for (std::size_t passIndex = 0; passIndex < order.size(); ++passIndex) {
    const auto currentId = order[passIndex];
    const auto& current = m_passes[currentId];

    for (const auto read : current.reads) {
      const auto writerIt = lastWriter.find(read);
      if (writerIt != lastWriter.end()) {
        std::fill(visited.begin(), visited.end(), false);
        if (!has_path(writerIt->second, currentId, visited)) {
          if (diagnostics) {
            diagnostics->push_back("hazard: pass '" + current.name + "' reads resource without write dependency from pass " +
                                  std::to_string(writerIt->second));
          }
          return false;
        }
      }
      lastReaders[read].push_back(currentId);
    }

    for (const auto write : current.writes) {
      const auto writerIt = lastWriter.find(write);
      if (writerIt != lastWriter.end() && writerIt->second != currentId) {
        std::fill(visited.begin(), visited.end(), false);
        if (!has_path(writerIt->second, currentId, visited)) {
          if (diagnostics) {
            diagnostics->push_back("hazard: pass '" + current.name + "' writes same resource as pass " +
                                  std::to_string(writerIt->second) + " without dependency");
          }
          return false;
        }
      }

      const auto readersIt = lastReaders.find(write);
      if (readersIt != lastReaders.end()) {
        for (const auto reader : readersIt->second) {
          std::fill(visited.begin(), visited.end(), false);
          if (!has_path(reader, currentId, visited)) {
            if (diagnostics) {
              diagnostics->push_back("hazard: pass '" + current.name + "' writes resource with pending read from pass " +
                                    std::to_string(reader));
            }
            return false;
          }
        }
      }

      lastWriter[write] = currentId;
      lastReaders[write].clear();
    }
  }

  return true;
}

FrameGraphResult FrameGraph::execute(
    IRenderCommandContext& context,
    const std::vector<std::uint32_t>* execution_order) const {
  FrameContext frame;
  return execute(context, frame, execution_order);
}

FrameGraphResult FrameGraph::execute(
    IRenderCommandContext& context,
    const FrameContext& frame,
    const std::vector<std::uint32_t>* execution_order) const {
  std::vector<std::string> diagnostics;
  if (!validate(&diagnostics)) {
    return FrameGraphFailure(
        vkpt::core::StatusCode::InvalidArgument,
        diagnostics.empty() ? "frame graph validation failed" : diagnostics.front());
  }

  std::vector<std::uint32_t> order;
  if (execution_order && !execution_order->empty()) {
    order.reserve(execution_order->size());
    for (const auto id : *execution_order) {
      if (id >= m_passes.size()) {
        return FrameGraphFailure(vkpt::core::StatusCode::InvalidArgument,
                                 "frame graph execution order references unknown pass");
      }
      order.push_back(id);
    }
  } else {
    if (!topo_sort(order, nullptr)) {
      return FrameGraphFailure(vkpt::core::StatusCode::InvalidArgument,
                               "frame graph topological sort failed");
    }
  }

  FrameGraphResult result;
  result.per_pass.reserve(order.size());
  if (!context.begin_frame()) {
    result.overall = vkpt::core::Status::error(vkpt::core::StatusCode::InternalError,
                                               "frame graph begin_frame failed");
    return result;
  }
  for (const auto pass_id : order) {
    const auto& pass = m_passes[pass_id];
    FrameGraphPassResult pass_result;
    pass_result.pass_id = pass.id;
    pass_result.pass_name = pass.name;
    if (!context.begin_pass(pass.type, pass.name)) {
      pass_result.status = vkpt::core::Status::error(
          vkpt::core::StatusCode::InternalError,
          "frame graph begin_pass failed: " + pass.name);
      result.per_pass.push_back(std::move(pass_result));
      result.overall = result.per_pass.back().status;
      return result;
    }
    if (pass.type == PassType::Compute) {
      // The interface-level executor uses an 8x8 workgroup convention for
      // skeleton backends; native backends can replace this at command recording.
      const auto dispatchX = std::max<std::uint32_t>(1u, (frame.viewport_width + 7u) / 8u);
      const auto dispatchY = std::max<std::uint32_t>(1u, (frame.viewport_height + 7u) / 8u);
      if (!context.dispatch(dispatchX, dispatchY, 1)) {
        pass_result.status = vkpt::core::Status::error(
            vkpt::core::StatusCode::InternalError,
            "frame graph dispatch failed: " + pass.name);
        result.per_pass.push_back(std::move(pass_result));
        result.overall = result.per_pass.back().status;
        return result;
      }
    }
    if (!context.end_pass()) {
      pass_result.status = vkpt::core::Status::error(
          vkpt::core::StatusCode::InternalError,
          "frame graph end_pass failed: " + pass.name);
      result.per_pass.push_back(std::move(pass_result));
      result.overall = result.per_pass.back().status;
      return result;
    }
    result.per_pass.push_back(std::move(pass_result));
  }
  if (!context.end_frame()) {
    result.overall = vkpt::core::Status::error(vkpt::core::StatusCode::InternalError,
                                               "frame graph end_frame failed");
    return result;
  }
  result.overall = vkpt::core::Status::ok("frame graph executed");
  return result;
}

}  // namespace vkpt::render
