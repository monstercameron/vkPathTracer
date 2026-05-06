#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/Types.h"

namespace vkpt::jobs {

using JobFunction = std::function<void()>;
using IndexedRangeJobFunction = std::function<void(std::size_t)>;

enum class WorkerThreadPriority {
  Normal,
  Background,
};

struct JobSystemConfig {
  std::size_t worker_count = 0u;
  WorkerThreadPriority worker_priority = WorkerThreadPriority::Normal;
  bool waiting_thread_runs_jobs = true;
};

void ApplyCurrentThreadPriority(WorkerThreadPriority priority);

class IJobSystem {
 public:
  virtual ~IJobSystem() = default;
  virtual vkpt::core::JobHandle submit_job(JobFunction job) = 0;
  virtual vkpt::core::JobHandle submit_main_thread_job(JobFunction job) = 0;
  virtual vkpt::core::JobHandle submit_range_job(std::size_t begin, std::size_t end, std::size_t step, JobFunction job) = 0;
  virtual vkpt::core::JobHandle submit_indexed_range_job(std::size_t begin,
                                                        std::size_t end,
                                                        std::size_t step,
                                                        IndexedRangeJobFunction job) = 0;
  virtual bool wait(vkpt::core::JobHandle id) = 0;
  virtual bool wait(vkpt::core::JobHandle id, std::stop_token stop) = 0;
  virtual bool wait_group(const std::vector<vkpt::core::JobHandle>& ids) = 0;
  virtual bool wait_group(const std::vector<vkpt::core::JobHandle>& ids, std::stop_token stop) = 0;
  virtual std::size_t worker_count() const = 0;
  virtual void pump_main_thread() = 0;
  virtual bool deterministic() const = 0;
  virtual void set_deterministic(bool enabled) = 0;
  virtual bool shutdown() = 0;
};

class JobSystem final : public IJobSystem {
 public:
  explicit JobSystem(std::size_t workerCount = 0u);
  explicit JobSystem(JobSystemConfig config);
  ~JobSystem() override;

  vkpt::core::JobHandle submit_job(JobFunction job) override;
  vkpt::core::JobHandle submit_main_thread_job(JobFunction job) override;
  vkpt::core::JobHandle submit_range_job(std::size_t begin, std::size_t end, std::size_t step, JobFunction job) override;
  vkpt::core::JobHandle submit_indexed_range_job(std::size_t begin,
                                                 std::size_t end,
                                                 std::size_t step,
                                                 IndexedRangeJobFunction job) override;
  bool wait(vkpt::core::JobHandle id) override;
  bool wait(vkpt::core::JobHandle id, std::stop_token stop) override;
  bool wait_group(const std::vector<vkpt::core::JobHandle>& ids) override;
  bool wait_group(const std::vector<vkpt::core::JobHandle>& ids, std::stop_token stop) override;
  std::size_t worker_count() const override;
  void pump_main_thread() override;
  bool deterministic() const override;
  void set_deterministic(bool enabled) override;
  bool shutdown() override;

 private:
  struct JobState;
  vkpt::core::JobHandle submit_internal(JobFunction job,
                                        std::shared_ptr<JobState> parent = {},
                                        bool main_thread = false);
  vkpt::core::JobHandle create_group(std::size_t pending_count);
  vkpt::core::JobHandle create_completed_job();
  std::shared_ptr<JobState> find_job(vkpt::core::JobHandle id) const;
  void retire_job_tree(vkpt::core::JobHandle id);
  bool try_run_one_queued_job();
  void run_job(const std::shared_ptr<JobState>& state);
  void complete_job(const std::shared_ptr<JobState>& state, std::exception_ptr failure = {});
  void worker_loop();

 private:
  std::unordered_map<vkpt::core::JobHandle, std::shared_ptr<JobState>> m_jobs;
  mutable std::mutex m_jobsMutex;
  std::condition_variable m_jobsCv;
  std::mutex m_queueMutex;
  std::recursive_mutex m_serialMutex;
  std::deque<vkpt::core::JobHandle> m_queue;
  std::deque<vkpt::core::JobHandle> m_mainQueue;
  std::vector<std::thread> m_workers;
  WorkerThreadPriority m_workerPriority = WorkerThreadPriority::Normal;
  bool m_waitingThreadRunsJobs = true;
  std::atomic_bool m_stopped{false};
  std::atomic_bool m_deterministic{false};
  std::atomic<vkpt::core::JobHandle> m_nextJobId{1u};
};

}  // namespace vkpt::jobs
