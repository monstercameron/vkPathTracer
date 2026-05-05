#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jobs/JobSystem.h"

namespace vkpt::jobs {

struct TaskExecutionSample {
  vkpt::core::RuntimeHandle task_id = 0u;
  std::string task_name;
  uint64_t start_ns = 0u;
  uint64_t end_ns = 0u;
  uint64_t duration_ns() const { return (end_ns >= start_ns) ? (end_ns - start_ns) : 0u; }
};

class TaskGraph {
 public:
  using Task = std::function<void()>;

  struct Node {
    vkpt::core::RuntimeHandle id = 0u;
    std::string name;
    Task task;
  };

  explicit TaskGraph(std::size_t reserved = 0u);
  vkpt::core::RuntimeHandle add_task(std::string_view name, Task task);
  bool add_dependency(vkpt::core::RuntimeHandle from, vkpt::core::RuntimeHandle to);
  bool validate(std::vector<std::string>* diagnostics) const;
  bool execute(IJobSystem* job_system = nullptr, bool force_sequential = false);
  std::vector<TaskExecutionSample> execution_timing() const;
  std::vector<vkpt::core::RuntimeHandle> stable_topological_order() const;
  void clear();

 private:
  bool topo_sort(std::vector<vkpt::core::RuntimeHandle>& order,
                 std::vector<std::string>* diagnostics) const;

 private:
  mutable std::vector<Node> m_nodes;
  std::vector<std::pair<vkpt::core::RuntimeHandle, vkpt::core::RuntimeHandle>> m_dependencies;
  mutable std::vector<TaskExecutionSample> m_timing;
  vkpt::core::RuntimeHandle m_nextId = 1u;
};

namespace task_graph_detail {

inline uint64_t now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
}

}  // namespace task_graph_detail

inline TaskGraph::TaskGraph(std::size_t reserved) {
  m_nodes.reserve(reserved);
  m_dependencies.reserve(reserved);
}

inline vkpt::core::RuntimeHandle TaskGraph::add_task(std::string_view name, Task task) {
  const auto id = m_nextId++;
  m_nodes.push_back(Node{id, std::string(name), std::move(task)});
  return id;
}

inline bool TaskGraph::add_dependency(vkpt::core::RuntimeHandle from, vkpt::core::RuntimeHandle to) {
  if (from == 0u || to == 0u || from == to) {
    return false;
  }
  const auto has_node = [&](vkpt::core::RuntimeHandle id) {
    return std::any_of(m_nodes.begin(), m_nodes.end(), [id](const Node& node) { return node.id == id; });
  };
  if (!has_node(from) || !has_node(to)) {
    return false;
  }
  const auto dep = std::pair{from, to};
  if (std::find(m_dependencies.begin(), m_dependencies.end(), dep) != m_dependencies.end()) {
    return true;
  }
  m_dependencies.push_back(dep);
  std::vector<vkpt::core::RuntimeHandle> order;
  if (!topo_sort(order, nullptr)) {
    m_dependencies.pop_back();
    return false;
  }
  return true;
}

inline bool TaskGraph::validate(std::vector<std::string>* diagnostics) const {
  std::vector<vkpt::core::RuntimeHandle> order;
  return topo_sort(order, diagnostics);
}

inline bool TaskGraph::execute(IJobSystem* job_system, bool force_sequential) {
  std::vector<vkpt::core::RuntimeHandle> order;
  if (!topo_sort(order, nullptr)) {
    m_timing.clear();
    return false;
  }

  std::unordered_map<vkpt::core::RuntimeHandle, std::size_t> order_index;
  order_index.reserve(order.size());
  for (std::size_t i = 0u; i < order.size(); ++i) {
    order_index[order[i]] = i;
  }

  auto node_for = [&](vkpt::core::RuntimeHandle id) -> const Node* {
    const auto it = std::find_if(m_nodes.begin(), m_nodes.end(), [id](const Node& node) { return node.id == id; });
    return it == m_nodes.end() ? nullptr : &(*it);
  };

  m_timing.clear();
  m_timing.resize(order.size());

  auto run_one = [&](vkpt::core::RuntimeHandle id) -> bool {
    const auto* node = node_for(id);
    if (!node) {
      return false;
    }
    TaskExecutionSample sample;
    sample.task_id = id;
    sample.task_name = node->name;
    sample.start_ns = task_graph_detail::now_ns();
    try {
      if (node->task) {
        node->task();
      }
    } catch (...) {
      sample.end_ns = task_graph_detail::now_ns();
      m_timing[order_index[id]] = std::move(sample);
      return false;
    }
    sample.end_ns = task_graph_detail::now_ns();
    m_timing[order_index[id]] = std::move(sample);
    return true;
  };

  if (force_sequential || job_system == nullptr || job_system->deterministic()) {
    for (const auto id : order) {
      if (!run_one(id)) {
        return false;
      }
    }
    return true;
  }

  std::unordered_map<vkpt::core::RuntimeHandle, std::size_t> remaining_deps;
  std::unordered_map<vkpt::core::RuntimeHandle, std::vector<vkpt::core::RuntimeHandle>> children;
  remaining_deps.reserve(order.size());
  children.reserve(order.size());
  for (const auto id : order) {
    remaining_deps[id] = 0u;
    children[id] = {};
  }
  for (const auto& [from, to] : m_dependencies) {
    ++remaining_deps[to];
    children[from].push_back(to);
  }
  for (auto& [id, list] : children) {
    std::sort(list.begin(), list.end(), [&](vkpt::core::RuntimeHandle lhs, vkpt::core::RuntimeHandle rhs) {
      return order_index[lhs] < order_index[rhs];
    });
  }

  std::vector<vkpt::core::RuntimeHandle> ready;
  ready.reserve(order.size());
  for (const auto id : order) {
    if (remaining_deps[id] == 0u) {
      ready.push_back(id);
    }
  }

  std::vector<std::exception_ptr> failures(order.size());
  std::size_t completed = 0u;
  while (!ready.empty()) {
    std::sort(ready.begin(), ready.end(), [&](vkpt::core::RuntimeHandle lhs, vkpt::core::RuntimeHandle rhs) {
      return order_index[lhs] < order_index[rhs];
    });
    const auto wave = ready;
    ready.clear();

    std::vector<vkpt::core::JobHandle> handles;
    handles.reserve(wave.size());
    for (const auto id : wave) {
      const auto* node = node_for(id);
      if (!node) {
        return false;
      }
      const auto sample_index = order_index[id];
      handles.push_back(job_system->submit_job([&, id, sample_index, node]() {
        TaskExecutionSample sample;
        sample.task_id = id;
        sample.task_name = node->name;
        sample.start_ns = task_graph_detail::now_ns();
        try {
          if (node->task) {
            node->task();
          }
        } catch (...) {
          failures[sample_index] = std::current_exception();
        }
        sample.end_ns = task_graph_detail::now_ns();
        m_timing[sample_index] = std::move(sample);
      }));
    }

    if (!job_system->wait_group(handles)) {
      return false;
    }
    if (std::any_of(failures.begin(), failures.end(), [](const std::exception_ptr& failure) { return failure != nullptr; })) {
      return false;
    }

    completed += wave.size();
    for (const auto id : wave) {
      for (const auto child : children[id]) {
        const auto left = --remaining_deps[child];
        if (left == 0u) {
          ready.push_back(child);
        }
      }
    }
  }

  return completed == order.size();
}

inline std::vector<TaskExecutionSample> TaskGraph::execution_timing() const {
  return m_timing;
}

inline std::vector<vkpt::core::RuntimeHandle> TaskGraph::stable_topological_order() const {
  std::vector<vkpt::core::RuntimeHandle> order;
  topo_sort(order, nullptr);
  return order;
}

inline void TaskGraph::clear() {
  m_nodes.clear();
  m_dependencies.clear();
  m_timing.clear();
  m_nextId = 1u;
}

inline bool TaskGraph::topo_sort(std::vector<vkpt::core::RuntimeHandle>& order,
                                 std::vector<std::string>* diagnostics) const {
  order.clear();
  auto report = [&](std::string message) {
    if (diagnostics) {
      diagnostics->push_back(std::move(message));
    }
  };

  std::vector<vkpt::core::RuntimeHandle> ids;
  ids.reserve(m_nodes.size());
  std::unordered_set<vkpt::core::RuntimeHandle> seen;
  seen.reserve(m_nodes.size());
  for (const auto& node : m_nodes) {
    if (node.id == 0u) {
      report("task id is zero");
      continue;
    }
    if (!seen.insert(node.id).second) {
      report("duplicate task id " + std::to_string(node.id));
      continue;
    }
    ids.push_back(node.id);
  }
  std::sort(ids.begin(), ids.end());
  if (ids.size() != m_nodes.size()) {
    return false;
  }

  std::unordered_map<vkpt::core::RuntimeHandle, std::size_t> indegree;
  std::unordered_map<vkpt::core::RuntimeHandle, std::vector<vkpt::core::RuntimeHandle>> outgoing;
  indegree.reserve(ids.size());
  outgoing.reserve(ids.size());
  for (const auto id : ids) {
    indegree[id] = 0u;
    outgoing[id] = {};
  }

  std::vector<std::pair<vkpt::core::RuntimeHandle, vkpt::core::RuntimeHandle>> dependency_keys;
  dependency_keys.reserve(m_dependencies.size());
  bool valid = true;
  for (const auto& [from, to] : m_dependencies) {
    if (!seen.contains(from) || !seen.contains(to)) {
      report("dependency references missing task " + std::to_string(from) + " -> " + std::to_string(to));
      valid = false;
      continue;
    }
    if (from == to) {
      report("task depends on itself " + std::to_string(from));
      valid = false;
      continue;
    }
    const auto key = std::pair{from, to};
    if (std::find(dependency_keys.begin(), dependency_keys.end(), key) != dependency_keys.end()) {
      continue;
    }
    dependency_keys.push_back(key);
    outgoing[from].push_back(to);
    ++indegree[to];
  }
  if (!valid) {
    return false;
  }

  for (auto& [id, list] : outgoing) {
    std::sort(list.begin(), list.end());
  }

  std::vector<vkpt::core::RuntimeHandle> ready;
  ready.reserve(ids.size());
  for (const auto id : ids) {
    if (indegree[id] == 0u) {
      ready.push_back(id);
    }
  }

  while (!ready.empty()) {
    std::sort(ready.begin(), ready.end());
    const auto id = ready.front();
    ready.erase(ready.begin());
    order.push_back(id);
    for (const auto child : outgoing[id]) {
      const auto left = --indegree[child];
      if (left == 0u) {
        ready.push_back(child);
      }
    }
  }

  if (order.size() != ids.size()) {
    report("task graph contains a dependency cycle");
    order.clear();
    return false;
  }
  return true;
}

}  // namespace vkpt::jobs
