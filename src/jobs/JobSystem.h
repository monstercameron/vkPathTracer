#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/Types.h"

namespace vkpt::jobs {

using JobFunction = std::function<void()>;

class IJobSystem {
 public:
  virtual ~IJobSystem() = default;
  virtual vkpt::core::RuntimeHandle submit_job(JobFunction job) = 0;
  virtual vkpt::core::RuntimeHandle submit_range_job(std::size_t begin, std::size_t end, std::size_t step, JobFunction job) = 0;
  virtual bool wait(vkpt::core::RuntimeHandle id) = 0;
  virtual bool wait_group(const std::vector<vkpt::core::RuntimeHandle>& ids) = 0;
  virtual std::size_t worker_count() const = 0;
  virtual void pump_main_thread() = 0;
  virtual bool deterministic() const = 0;
  virtual void set_deterministic(bool enabled) = 0;
  virtual bool shutdown() = 0;
};

class JobSystem final : public IJobSystem {
 public:
  explicit JobSystem(std::size_t workerCount = 0u);
  ~JobSystem() override;

  vkpt::core::RuntimeHandle submit_job(JobFunction job) override;
  vkpt::core::RuntimeHandle submit_range_job(std::size_t begin, std::size_t end, std::size_t step, JobFunction job) override;
  bool wait(vkpt::core::RuntimeHandle id) override;
  bool wait_group(const std::vector<vkpt::core::RuntimeHandle>& ids) override;
  std::size_t worker_count() const override;
  void pump_main_thread() override;
  bool deterministic() const override;
  void set_deterministic(bool enabled) override;
  bool shutdown() override;

 private:
  struct JobEntry;
  vkpt::core::RuntimeHandle submit_internal(JobFunction job);
  void worker_loop();

 private:
  std::vector<std::unique_ptr<JobEntry>> m_jobs;
  mutable std::mutex m_jobsMutex;
  std::condition_variable m_jobsCv;
  std::mutex m_queueMutex;
  std::condition_variable m_mainCv;
  std::mutex m_mainMutex;
  std::mutex m_serialMutex;  // serializes job execution in deterministic mode
  std::deque<vkpt::core::RuntimeHandle> m_queue;
  std::deque<vkpt::core::RuntimeHandle> m_mainQueue;
  std::vector<std::thread> m_workers;
  std::size_t m_nextWorkerCount = 0u;
  bool m_stopped = false;
  bool m_deterministic = false;
  std::atomic<vkpt::core::RuntimeHandle> m_nextJobId{1u};
};

}  // namespace vkpt::jobs
