#pragma once

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/Types.h"
#include "core/contracts/Determinism.h"
#include "core/contracts/Lifecycle.h"
#include "core/contracts/Result.h"
#include "core/health/Health.h"

namespace vkpt::jobs {

inline constexpr std::string_view kJobsSubsystemName = "jobs";
inline constexpr std::string_view kJobSystemStatusTypeName = "JobSystemStatus";
inline constexpr std::string_view kJobSystemContractName = "jobs.job_system.v1";

struct JobSystemNamingContract {
  std::string_view subsystem_name = kJobsSubsystemName;
  std::string_view status_type_name = kJobSystemStatusTypeName;
  std::string_view job_system_contract = kJobSystemContractName;
  std::string_view health_probe_name = kJobsSubsystemName;
  std::string_view lifecycle_field_name = "lifecycle";
  std::string_view last_error_field_name = "last_error";
  std::string_view flow_field_name = "current_flow_id";
  std::string_view queue_depth_field_name = "queue_depth_total";
};

inline constexpr JobSystemNamingContract kJobSystemNamingContract{};

[[nodiscard]] inline constexpr std::string_view JobsSubsystemName() noexcept {
  return kJobsSubsystemName;
}

using JobFunction = std::function<void()>;
using IndexedRangeJobFunction = std::function<void(std::size_t)>;

enum class WorkerThreadPriority {
  Normal,
  Background,
};

enum class JobPriority : std::uint8_t {
  High = 0u,
  Normal = 1u,
  Low = 2u,
};

struct JobSystemConfig {
  std::size_t worker_count = 0u;
  WorkerThreadPriority worker_priority = WorkerThreadPriority::Normal;
  bool waiting_thread_runs_jobs = true;
};

struct JobSystemStatus {
  std::string name = std::string(kJobsSubsystemName);
  vkpt::core::contracts::ComponentLifecycle lifecycle =
      vkpt::core::contracts::ComponentLifecycle::Uninitialized;
  std::string last_error;
  std::uint64_t last_tick_ns = 0u;
  std::uint64_t ticks_total = 0u;
  std::uint64_t errors_total = 0u;
  std::size_t worker_count = 0u;
  std::size_t workers_busy = 0u;
  std::size_t queue_depth_total = 0u;
  std::size_t main_thread_queue_depth = 0u;
  std::vector<std::size_t> queue_depth_per_worker;
  std::uint64_t oldest_pending_us = 0u;
  bool deterministic = false;
  std::uint64_t determinism_base_seed = 0u;
  vkpt::core::FrameIndex determinism_frame_index = 0u;
  std::string determinism_scenario_id;
  std::uint64_t current_flow_id = 0u;
  bool queue_starved = false;
  std::uint64_t jobs_submitted_total = 0u;
  std::uint64_t jobs_completed_total = 0u;
  std::uint64_t jobs_failed_total = 0u;
};

using JobsSubsystemStatus = JobSystemStatus;

struct JobWaitResult {
  vkpt::core::Status status = vkpt::core::Status::ok();
  std::exception_ptr exception;

  bool ok() const noexcept { return status.is_ok() && exception == nullptr; }
  explicit operator bool() const noexcept { return ok(); }
};

bool JobSystemHasQueuedWork(const JobSystemStatus& status);
vkpt::core::health::Report EvaluateJobSystemHealth(const JobSystemStatus& status);

void ApplyCurrentThreadPriority(WorkerThreadPriority priority);

class IJobSystem {
 public:
  virtual ~IJobSystem() = default;

  // IJobSystem lifecycle contract:
  //
  // state\method     submit_*     wait/wait_group pump_main_thread status shutdown
  // Uninitialized    illegal      error           noop             ok     ok
  // Initializing      error        error           noop             ok     ->ShuttingDown
  // Ready             ok           ok              ok               ok     ->ShuttingDown
  // Busy              ok           ok              ok               ok     ->ShuttingDown
  // Failed            error        ok              ok               ok     ->ShuttingDown
  // ShuttingDown      error        ok              drain            ok     ok
  //
  // Bool-returning methods are compatibility wrappers. New call sites should
  // prefer the Result/Status variants so failure reason and thrown exceptions
  // are visible to callers and probes.
  virtual vkpt::core::JobHandle submit_job(JobFunction job) = 0;
  virtual vkpt::core::Result<vkpt::core::JobHandle> submit_job_result(JobFunction job) = 0;
  virtual vkpt::core::JobHandle submit_main_thread_job(JobFunction job) = 0;
  virtual vkpt::core::JobHandle submit_main_thread_job(JobFunction job, JobPriority priority) {
    (void)priority;
    return submit_main_thread_job(std::move(job));
  }
  virtual vkpt::core::Result<vkpt::core::JobHandle> submit_main_thread_job_result(
      JobFunction job,
      JobPriority priority = JobPriority::Normal) = 0;
  virtual vkpt::core::JobHandle chain(vkpt::core::JobHandle prev, JobFunction job) = 0;
  virtual vkpt::core::Result<vkpt::core::JobHandle> chain_result(vkpt::core::JobHandle prev,
                                                                JobFunction job) = 0;
  virtual vkpt::core::JobHandle submit_range_job(std::size_t begin, std::size_t end, std::size_t step, JobFunction job) = 0;
  virtual vkpt::core::Result<vkpt::core::JobHandle> submit_range_job_result(std::size_t begin,
                                                                            std::size_t end,
                                                                            std::size_t step,
                                                                            JobFunction job) = 0;
  virtual vkpt::core::JobHandle submit_indexed_range_job(std::size_t begin,
                                                        std::size_t end,
                                                        std::size_t step,
                                                        IndexedRangeJobFunction job) = 0;
  virtual vkpt::core::Result<vkpt::core::JobHandle> submit_indexed_range_job_result(
      std::size_t begin,
      std::size_t end,
      std::size_t step,
      IndexedRangeJobFunction job) = 0;
  virtual bool wait(vkpt::core::JobHandle id) = 0;
  virtual bool wait(vkpt::core::JobHandle id, std::stop_token stop) = 0;
  virtual JobWaitResult wait_result(vkpt::core::JobHandle id) = 0;
  virtual JobWaitResult wait_result(vkpt::core::JobHandle id, std::stop_token stop) = 0;
  virtual bool wait_group(const std::vector<vkpt::core::JobHandle>& ids) = 0;
  virtual bool wait_group(const std::vector<vkpt::core::JobHandle>& ids, std::stop_token stop) = 0;
  virtual vkpt::core::Status wait_group_status(const std::vector<vkpt::core::JobHandle>& ids) = 0;
  virtual vkpt::core::Status wait_group_status(const std::vector<vkpt::core::JobHandle>& ids,
                                              std::stop_token stop) = 0;
  virtual std::size_t worker_count() const = 0;
  virtual void pump_main_thread() = 0;
  virtual bool deterministic() const = 0;
  virtual void set_deterministic(bool enabled) = 0;
  virtual void set_determinism(const vkpt::core::DeterminismContext& context) {
    set_deterministic(context.enabled);
  }
  virtual bool waiting_thread_runs_jobs() const { return true; }
  virtual bool shutdown() = 0;
  virtual vkpt::core::Status shutdown_status() = 0;
  virtual JobSystemStatus status() const = 0;
};

class JobSystem final : public IJobSystem {
 public:
  explicit JobSystem(std::size_t workerCount = 0u);
  explicit JobSystem(JobSystemConfig config);
  ~JobSystem() override;

  vkpt::core::JobHandle submit_job(JobFunction job) override;
  vkpt::core::Result<vkpt::core::JobHandle> submit_job_result(JobFunction job) override;
  vkpt::core::JobHandle submit_main_thread_job(JobFunction job) override;
  vkpt::core::JobHandle submit_main_thread_job(JobFunction job, JobPriority priority) override;
  vkpt::core::Result<vkpt::core::JobHandle> submit_main_thread_job_result(
      JobFunction job,
      JobPriority priority = JobPriority::Normal) override;
  vkpt::core::JobHandle chain(vkpt::core::JobHandle prev, JobFunction job) override;
  vkpt::core::Result<vkpt::core::JobHandle> chain_result(vkpt::core::JobHandle prev,
                                                        JobFunction job) override;
  vkpt::core::JobHandle submit_range_job(std::size_t begin, std::size_t end, std::size_t step, JobFunction job) override;
  vkpt::core::Result<vkpt::core::JobHandle> submit_range_job_result(std::size_t begin,
                                                                    std::size_t end,
                                                                    std::size_t step,
                                                                    JobFunction job) override;
  vkpt::core::JobHandle submit_indexed_range_job(std::size_t begin,
                                                 std::size_t end,
                                                 std::size_t step,
                                                 IndexedRangeJobFunction job) override;
  vkpt::core::Result<vkpt::core::JobHandle> submit_indexed_range_job_result(
      std::size_t begin,
      std::size_t end,
      std::size_t step,
      IndexedRangeJobFunction job) override;
  bool wait(vkpt::core::JobHandle id) override;
  bool wait(vkpt::core::JobHandle id, std::exception_ptr* out_exception);
  bool wait(vkpt::core::JobHandle id, std::stop_token stop) override;
  JobWaitResult wait_result(vkpt::core::JobHandle id) override;
  JobWaitResult wait_result(vkpt::core::JobHandle id, std::stop_token stop) override;
  bool wait_group(const std::vector<vkpt::core::JobHandle>& ids) override;
  bool wait_group(const std::vector<vkpt::core::JobHandle>& ids, std::stop_token stop) override;
  vkpt::core::Status wait_group_status(const std::vector<vkpt::core::JobHandle>& ids) override;
  vkpt::core::Status wait_group_status(const std::vector<vkpt::core::JobHandle>& ids,
                                      std::stop_token stop) override;
  std::size_t worker_count() const override;
  void pump_main_thread() override;
  bool deterministic() const override;
  void set_deterministic(bool enabled) override;
  void set_determinism(const vkpt::core::DeterminismContext& context) override;
  bool waiting_thread_runs_jobs() const override;
  bool shutdown() override;
  vkpt::core::Status shutdown_status() override;
  JobSystemStatus status() const override;
  std::shared_ptr<vkpt::core::health::IHealthProbe> create_health_probe() const;

 private:
  struct JobState;
  struct CompletedResult {
    bool ok = true;
    std::exception_ptr failure;
  };
  vkpt::core::JobHandle submit_internal(JobFunction job,
                                        std::shared_ptr<JobState> parent = {},
                                        bool main_thread = false,
                                        JobPriority main_thread_priority = JobPriority::Normal);
  vkpt::core::JobHandle create_group(std::size_t pending_count);
  vkpt::core::JobHandle create_completed_job();
  std::shared_ptr<JobState> find_job(vkpt::core::JobHandle id) const;
  bool wait(vkpt::core::JobHandle id, std::stop_token stop, std::exception_ptr* out_exception);
  bool find_completed_result(vkpt::core::JobHandle id, CompletedResult& result) const;
  void record_status_tick();
  void record_status_error(std::string error);
  void remember_completed_result_locked(vkpt::core::JobHandle id, CompletedResult result);
  void retire_completed_state(const std::shared_ptr<JobState>& state);
  void retire_job_tree(vkpt::core::JobHandle id);
  bool has_queued_work_locked() const;
  void record_queue_metrics_locked();
  bool try_pop_queued_job(vkpt::core::JobHandle& id, std::optional<std::size_t> worker_index);
  bool try_run_one_queued_job(std::optional<std::size_t> worker_index = std::nullopt);
  void run_job(const std::shared_ptr<JobState>& state);
  void complete_job(const std::shared_ptr<JobState>& state, std::exception_ptr failure = {});
  void worker_loop(std::size_t worker_index);

 private:
  std::unordered_map<vkpt::core::JobHandle, std::shared_ptr<JobState>> m_jobs;
  std::unordered_map<vkpt::core::JobHandle, CompletedResult> m_completedResults;
  std::deque<vkpt::core::JobHandle> m_completedOrder;
  mutable std::mutex m_jobsMutex;
  std::condition_variable m_jobsCv;
  mutable std::mutex m_queueMutex;
  mutable std::mutex m_determinismMutex;
  mutable std::mutex m_statusMutex;
  std::recursive_mutex m_serialMutex;
  std::deque<vkpt::core::JobHandle> m_queue;
  std::vector<std::deque<vkpt::core::JobHandle>> m_workerQueues;
  std::array<std::deque<vkpt::core::JobHandle>, 3> m_mainQueues;
  std::vector<std::thread> m_workers;
  WorkerThreadPriority m_workerPriority = WorkerThreadPriority::Normal;
  vkpt::core::DeterminismContext m_determinismContext{};
  bool m_waitingThreadRunsJobs = true;
  std::atomic_bool m_stopped{false};
  std::atomic<vkpt::core::contracts::ComponentLifecycle> m_lifecycle{
      vkpt::core::contracts::ComponentLifecycle::Uninitialized};
  std::atomic_bool m_deterministic{false};
  std::atomic<vkpt::core::JobHandle> m_nextJobId{1u};
  std::atomic<std::size_t> m_nextWorkerQueue{0u};
  std::atomic<std::size_t> m_workersBusy{0u};
  std::atomic<std::uint64_t> m_jobsSubmittedTotal{0u};
  std::atomic<std::uint64_t> m_jobsCompletedTotal{0u};
  std::atomic<std::uint64_t> m_jobsFailedTotal{0u};
  std::atomic<std::uint64_t> m_queueOverWorkerSinceNs{0u};
  std::atomic<std::uint64_t> m_lastTickNs{0u};
  std::atomic<std::uint64_t> m_ticksTotal{0u};
  std::atomic<std::uint64_t> m_errorsTotal{0u};
  std::string m_lastError;
};

}  // namespace vkpt::jobs
