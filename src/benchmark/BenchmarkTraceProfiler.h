#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "benchmark/BenchmarkSchema.h"

namespace vkpt::benchmark::ptbench {

class TraceProfiler final : public vkpt::benchmark::IProfiler {
 public:
  vkpt::benchmark::ProfilerEventHandle begin_event(vkpt::benchmark::ProfilerEventKind kind,
                                                   std::string_view name,
                                                   std::string_view category,
                                                   uint32_t thread_id) override;
  void end_event(vkpt::benchmark::ProfilerEventHandle handle) override;
  std::string emit_trace() const override;
  void reset_frame() override;
  vkpt::benchmark::ProfilerCapabilities describe_capabilities() const override;

 private:
  struct ActiveEvent {
    vkpt::benchmark::ProfilerEventHandle handle = 0;
    vkpt::benchmark::ProfilerEvent event;
  };

  double elapsed_ms() const;

  vkpt::benchmark::ProfilerEventHandle m_nextHandle = 1;
  std::chrono::steady_clock::time_point m_origin = std::chrono::steady_clock::now();
  std::vector<ActiveEvent> m_active;
  std::vector<vkpt::benchmark::ProfilerEvent> m_events;
};

}  // namespace vkpt::benchmark::ptbench
