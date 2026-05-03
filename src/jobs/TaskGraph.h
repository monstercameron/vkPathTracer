#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Types.h"

namespace vkpt::jobs {

class IJobSystem;

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

}  // namespace vkpt::jobs
