#include "jobs/JobSystem.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/Logging.h"
#include "core/metrics/Metrics.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vkpt::jobs {

// Cache line size for false-sharing avoidance. Use the standard hint when
// available, otherwise assume 64 bytes which matches every mainstream x86_64
// and AArch64 target this project ships on.
#ifdef __cpp_lib_hardware_interference_size
inline constexpr std::size_t kJobsCacheLineBytes = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kJobsCacheLineBytes = 64u;
#endif

struct JobSystem::JobState {
  // Hot atomic on its own cache line: workers fetch_sub on `pending` from
  // multiple cores while the wait path inspects the adjacent mutex/cv on its
  // owning thread. Padding prevents that ping-pong (audit J3).
  alignas(kJobsCacheLineBytes) std::atomic<std::size_t> pending = 1u;
  // Pad out the rest of the cache line so the cold metadata below cannot
  // be pulled into the same line as `pending`.
  char pad_after_pending[kJobsCacheLineBytes - sizeof(std::atomic<std::size_t>)];

  vkpt::core::JobHandle id = 0u;
  std::mutex mutex;
  std::condition_variable cv;
  JobFunction job;
  std::weak_ptr<JobState> parent;
  std::vector<vkpt::core::JobHandle> children;
  std::exception_ptr failure;
  std::uint64_t submitted_ns = 0u;
  std::uint64_t started_ns = 0u;
};

namespace {

std::size_t iteration_count(std::size_t begin, std::size_t end, std::size_t step) {
  if (begin >= end) {
    return 0u;
  }
  const std::size_t stride = step == 0u ? 1u : step;
  return ((end - begin) + stride - 1u) / stride;
}

std::uint64_t steady_now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::size_t job_priority_index(JobPriority priority) {
  switch (priority) {
    case JobPriority::High:
      return 0u;
    case JobPriority::Normal:
      return 1u;
    case JobPriority::Low:
      return 2u;
  }
  return 1u;
}

std::size_t main_queue_depth(
    const std::array<std::deque<vkpt::core::JobHandle>, 3>& queues) {
  std::size_t total = 0u;
  for (const auto& queue : queues) {
    total += queue.size();
  }
  return total;
}

vkpt::core::Status JobStatusError(vkpt::core::StatusCode code, std::string message) {
  return vkpt::core::Status::error(code, std::move(message));
}

}  // namespace

bool JobSystemHasQueuedWork(const JobSystemStatus& status) {
  if (status.queue_depth_total > 0u || status.main_thread_queue_depth > 0u) {
    return true;
  }
  return std::any_of(status.queue_depth_per_worker.begin(),
                     status.queue_depth_per_worker.end(),
                     [](std::size_t depth) { return depth > 0u; });
}

vkpt::core::health::Report EvaluateJobSystemHealth(const JobSystemStatus& status) {
  using vkpt::core::health::Report;
  using vkpt::core::health::Status;
  using vkpt::core::contracts::ComponentLifecycle;

  if (status.lifecycle == ComponentLifecycle::Failed) {
    return Report{Status::Failed,
                  status.last_error.empty() ? "job system failed" : status.last_error};
  }
  constexpr std::uint64_t kStarvationThresholdUs = 2'000'000u;
  const bool queue_exceeds_workers = status.queue_depth_total > status.worker_count;
  const bool starvation_window_elapsed =
      status.queue_starved || status.oldest_pending_us >= kStarvationThresholdUs;
  if (status.workers_busy == 0u && queue_exceeds_workers && starvation_window_elapsed) {
    return Report{
        Status::Failed,
        "jobs worker starvation: workers_busy=0 queue_depth_total=" +
            std::to_string(status.queue_depth_total) +
            " worker_count=" + std::to_string(status.worker_count) +
            " oldest_pending_us=" + std::to_string(status.oldest_pending_us)};
  }
  if (queue_exceeds_workers && starvation_window_elapsed) {
    return Report{Status::Degraded, "jobs queue depth has exceeded worker count for more than 2s"};
  }
  return Report{Status::Ok, "ok"};
}

void ApplyCurrentThreadPriority(WorkerThreadPriority priority) {
  if (priority == WorkerThreadPriority::Normal) {
    return;
  }
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
  (void)priority;
#endif
}

JobSystem::JobSystem(std::size_t workerCount) {
  m_lifecycle.store(vkpt::core::contracts::ComponentLifecycle::Initializing,
                    std::memory_order_release);
  const auto requested = workerCount == 0u
      ? std::max<std::size_t>(1u, std::thread::hardware_concurrency())
      : workerCount;
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        "jobs",
        "config",
        {{"worker_count", std::to_string(requested)},
         {"waiting_thread_runs_jobs", m_waitingThreadRunsJobs ? "true" : "false"}});
  } catch (...) {
  }
  VKP_LIFECYCLE_CONFIG("jobs",
                       "worker_count",
                       static_cast<std::uint64_t>(requested),
                       "waiting_thread_runs_jobs",
                       m_waitingThreadRunsJobs,
                       "flow_id",
                       std::uint64_t{0});
  m_workerQueues.resize(requested);
  m_workers.reserve(requested);
  for (std::size_t i = 0; i < requested; ++i) {
    m_workers.emplace_back([this, i]() { worker_loop(i); });
  }
  m_lifecycle.store(vkpt::core::contracts::ComponentLifecycle::Ready,
                    std::memory_order_release);
  record_status_tick();
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        "jobs",
        "started",
        {{"worker_count", std::to_string(requested)}});
  } catch (...) {
  }
  VKP_LIFECYCLE_STARTED("jobs",
                        "worker_count",
                        static_cast<std::uint64_t>(requested),
                        "flow_id",
                        std::uint64_t{0});
}

JobSystem::JobSystem(JobSystemConfig config)
    : m_workerPriority(config.worker_priority),
      m_waitingThreadRunsJobs(config.waiting_thread_runs_jobs) {
  m_lifecycle.store(vkpt::core::contracts::ComponentLifecycle::Initializing,
                    std::memory_order_release);
  const auto requested = config.worker_count == 0u
      ? std::max<std::size_t>(1u, std::thread::hardware_concurrency())
      : config.worker_count;
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        "jobs",
        "config",
        {{"worker_count", std::to_string(requested)},
         {"waiting_thread_runs_jobs", m_waitingThreadRunsJobs ? "true" : "false"},
         {"worker_priority", m_workerPriority == WorkerThreadPriority::Background
                                 ? "background"
                                 : "normal"}});
  } catch (...) {
  }
  VKP_LIFECYCLE_CONFIG("jobs",
                       "worker_count",
                       static_cast<std::uint64_t>(requested),
                       "waiting_thread_runs_jobs",
                       m_waitingThreadRunsJobs,
                       "flow_id",
                       std::uint64_t{0});
  m_workerQueues.resize(requested);
  m_workers.reserve(requested);
  for (std::size_t i = 0; i < requested; ++i) {
    m_workers.emplace_back([this, i]() { worker_loop(i); });
  }
  m_lifecycle.store(vkpt::core::contracts::ComponentLifecycle::Ready,
                    std::memory_order_release);
  record_status_tick();
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        "jobs",
        "started",
        {{"worker_count", std::to_string(requested)}});
  } catch (...) {
  }
  VKP_LIFECYCLE_STARTED("jobs",
                        "worker_count",
                        static_cast<std::uint64_t>(requested),
                        "flow_id",
                        std::uint64_t{0});
}

JobSystem::~JobSystem() {
  shutdown();
}

vkpt::core::JobHandle JobSystem::submit_internal(JobFunction job,
                                                 std::shared_ptr<JobState> parent,
                                                 bool main_thread,
                                                 JobPriority main_thread_priority) {
  if (m_stopped.load(std::memory_order_acquire)) {
    return 0u;
  }

  auto state = std::make_shared<JobState>();
  state->id = m_nextJobId.fetch_add(1u, std::memory_order_relaxed);
  state->job = std::move(job);
  state->parent = std::move(parent);
  state->submitted_ns = steady_now_ns();
  if (auto parentState = state->parent.lock()) {
    std::scoped_lock lock(parentState->mutex);
    parentState->children.push_back(state->id);
  }
  {
    std::scoped_lock lock(m_jobsMutex);
    m_jobs.emplace(state->id, state);
  }
  {
    std::scoped_lock lock(m_queueMutex);
    if (main_thread) {
      m_mainQueues[job_priority_index(main_thread_priority)].push_back(state->id);
    } else if (!m_workerQueues.empty() && !m_deterministic.load(std::memory_order_acquire)) {
      const auto queue_index =
          m_nextWorkerQueue.fetch_add(1u, std::memory_order_relaxed) % m_workerQueues.size();
      m_workerQueues[queue_index].push_back(state->id);
    } else {
      m_queue.push_back(state->id);
    }
    record_queue_metrics_locked();
  }
  m_jobsSubmittedTotal.fetch_add(1u, std::memory_order_relaxed);
  VKP_METRIC_INC("vkp.jobs.submitted_total");
  if (!main_thread) {
    m_jobsCv.notify_one();
  }
  return state->id;
}

vkpt::core::JobHandle JobSystem::create_group(std::size_t pending_count) {
  if (m_stopped.load(std::memory_order_acquire)) {
    return 0u;
  }
  auto state = std::make_shared<JobState>();
  state->id = m_nextJobId.fetch_add(1u, std::memory_order_relaxed);
  state->pending.store(pending_count, std::memory_order_release);
  state->submitted_ns = steady_now_ns();
  if (pending_count == 0u) {
    // Already-completed groups are returned by chain()/submit_*_job() shims
    // when the user passes an empty job. complete_job() will never run on
    // them, so retire eagerly here under m_jobsMutex: record a CompletedResult
    // (so a later wait() can still observe success) and skip inserting into
    // m_jobs entirely (audit J5: bound m_jobs growth for fire-and-forget
    // call sites that never wait).
    {
      std::scoped_lock lock(m_jobsMutex);
      remember_completed_result_locked(state->id, CompletedResult{true, nullptr});
    }
    state->cv.notify_all();
    return state->id;
  }
  {
    std::scoped_lock lock(m_jobsMutex);
    m_jobs.emplace(state->id, state);
  }
  return state->id;
}

vkpt::core::JobHandle JobSystem::create_completed_job() {
  return create_group(0u);
}

std::shared_ptr<JobSystem::JobState> JobSystem::find_job(vkpt::core::JobHandle id) const {
  std::scoped_lock lock(m_jobsMutex);
  const auto it = m_jobs.find(id);
  if (it == m_jobs.end()) {
    return {};
  }
  return it->second;
}

void JobSystem::record_status_tick() {
  m_lastTickNs.store(steady_now_ns(), std::memory_order_release);
  m_ticksTotal.fetch_add(1u, std::memory_order_relaxed);
}

void JobSystem::record_status_error(std::string error) {
  {
    std::scoped_lock lock(m_statusMutex);
    m_lastError = std::move(error);
  }
  m_errorsTotal.fetch_add(1u, std::memory_order_relaxed);
  record_status_tick();
}

bool JobSystem::find_completed_result(vkpt::core::JobHandle id,
                                      CompletedResult& result) const {
  std::scoped_lock lock(m_jobsMutex);
  const auto it = m_completedResults.find(id);
  if (it == m_completedResults.end()) {
    return false;
  }
  result = it->second;
  return true;
}

void JobSystem::remember_completed_result_locked(vkpt::core::JobHandle id,
                                                CompletedResult result) {
  constexpr std::size_t kMaxCompletedResults = 4096u;
  m_completedResults[id] = std::move(result);
  m_completedOrder.push_back(id);
  while (m_completedOrder.size() > kMaxCompletedResults) {
    const auto stale = m_completedOrder.front();
    m_completedOrder.pop_front();
    m_completedResults.erase(stale);
  }
}

void JobSystem::retire_completed_state(const std::shared_ptr<JobState>& state) {
  if (!state || state->pending.load(std::memory_order_acquire) != 0u) {
    return;
  }
  bool ok = true;
  std::exception_ptr failure;
  {
    std::scoped_lock state_lock(state->mutex);
    ok = state->failure == nullptr;
    failure = state->failure;
  }
  std::scoped_lock jobs_lock(m_jobsMutex);
  const auto it = m_jobs.find(state->id);
  if (it != m_jobs.end() && it->second->pending.load(std::memory_order_acquire) == 0u) {
    remember_completed_result_locked(state->id, CompletedResult{ok, failure});
    m_jobs.erase(it);
  }
}

void JobSystem::retire_job_tree(vkpt::core::JobHandle id) {
  auto state = find_job(id);
  if (!state || state->pending.load(std::memory_order_acquire) != 0u) {
    return;
  }

  std::vector<vkpt::core::JobHandle> children;
  {
    std::scoped_lock lock(state->mutex);
    if (state->pending.load(std::memory_order_acquire) != 0u) {
      return;
    }
    children = state->children;
  }

  for (const auto child : children) {
    retire_job_tree(child);
  }

  std::scoped_lock lock(m_jobsMutex);
  const auto it = m_jobs.find(id);
  if (it != m_jobs.end() && it->second->pending.load(std::memory_order_acquire) == 0u) {
    m_jobs.erase(it);
  }
}

void JobSystem::complete_job(const std::shared_ptr<JobState>& state, std::exception_ptr failure) {
  if (!state) {
    return;
  }

  std::shared_ptr<JobState> parent;
  {
    std::scoped_lock lock(state->mutex);
    if (state->pending.load(std::memory_order_acquire) == 0u) {
      if (failure && !state->failure) {
        state->failure = failure;
      }
      return;
    }
    if (failure && !state->failure) {
      state->failure = failure;
    }
    const auto old_pending = state->pending.fetch_sub(1u, std::memory_order_acq_rel);
    if (old_pending <= 1u) {
      state->pending.store(0u, std::memory_order_release);
      parent = state->parent.lock();
      m_jobsCompletedTotal.fetch_add(1u, std::memory_order_relaxed);
      VKP_METRIC_INC("vkp.jobs.completed_total");
      if (state->failure) {
        m_jobsFailedTotal.fetch_add(1u, std::memory_order_relaxed);
        VKP_METRIC_INC("vkp.jobs.failed_total");
        record_status_error("job_failed");
      } else {
        record_status_tick();
      }
    }
  }
  state->cv.notify_all();

  if (parent) {
    // Child jobs decrement the parent group exactly once so wait(group) observes
    // the whole dependency tree without enqueueing a separate completion job.
    complete_job(parent, failure);
  }
  // Eager retirement (audit J5): remove this state from m_jobs as soon as it
  // is finished and stash its CompletedResult. This bounds m_jobs even for
  // fire-and-forget submitters that never call wait(), without affecting
  // late waiters because find_completed_result() falls back on the bounded
  // m_completedResults LRU.
  retire_completed_state(state);
}

void JobSystem::run_job(const std::shared_ptr<JobState>& state) {
  if (!state) {
    return;
  }

  const auto started_ns = steady_now_ns();
  state->started_ns = started_ns;
  if (state->submitted_ns != 0u && started_ns >= state->submitted_ns) {
    VKP_METRIC_OBSERVE("vkp.jobs.wait_us", (started_ns - state->submitted_ns) / 1000u);
  }
  // Relaxed: this counter is only consumed by status() which uses acquire on
  // its own load, and the load synchronizes with these stores via the atomic
  // (audit J2). No surrounding non-atomic state hangs off this ordering.
  m_workersBusy.fetch_add(1u, std::memory_order_relaxed);
  std::exception_ptr failure;
  try {
    if (state->job) {
      state->job();
    }
  } catch (const std::exception& ex) {
    try {
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Error,
          "jobs",
          "job failed with exception",
          {{"job_id", std::to_string(state->id)}, {"error", ex.what()}});
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Error,
          "jobs",
          "jobs.exception",
          {{"job_id", std::to_string(state->id)}, {"error", ex.what()}});
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Error,
          "jobs",
          "jobs.job_threw",
          {{"job_id", std::to_string(state->id)}, {"error", ex.what()}});
    } catch (...) {
    }
    VKP_METRIC_INC("vkp.jobs.exception_total");
    failure = std::current_exception();
  } catch (...) {
    try {
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Error,
          "jobs",
          "job failed with non-standard exception",
          {{"job_id", std::to_string(state->id)}});
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Error,
          "jobs",
          "jobs.exception",
          {{"job_id", std::to_string(state->id)}, {"error", "non-standard exception"}});
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Error,
          "jobs",
          "jobs.job_threw",
          {{"job_id", std::to_string(state->id)}, {"error", "non-standard exception"}});
    } catch (...) {
    }
    VKP_METRIC_INC("vkp.jobs.exception_total");
    failure = std::current_exception();
  }
  const auto finished_ns = steady_now_ns();
  if (finished_ns >= started_ns) {
    VKP_METRIC_OBSERVE("vkp.jobs.run_us", (finished_ns - started_ns) / 1000u);
  }
  m_workersBusy.fetch_sub(1u, std::memory_order_relaxed);
  complete_job(state, failure);
}

bool JobSystem::has_queued_work_locked() const {
  if (!m_queue.empty()) {
    return true;
  }
  return std::any_of(m_workerQueues.begin(), m_workerQueues.end(), [](const auto& queue) {
    return !queue.empty();
  });
}

void JobSystem::record_queue_metrics_locked() {
  const auto mainDepth = main_queue_depth(m_mainQueues);
  std::size_t total = m_queue.size() + mainDepth;
  for (std::size_t i = 0u; i < m_workerQueues.size(); ++i) {
    const auto depth = m_workerQueues[i].size();
    total += depth;
    const auto name = "vkp.jobs.queue_depth.worker" + std::to_string(i);
    vkpt::core::metrics::MetricsRegistry::instance().gauge(name).set(static_cast<double>(depth));
  }
  VKP_METRIC_SET("vkp.jobs.queue_depth.total", total);
  VKP_METRIC_SET("vkp.jobs.queue_depth.main_thread", mainDepth);
  VKP_METRIC_SET("vkp.jobs.queue_depth.main_thread.high", m_mainQueues[0].size());
  VKP_METRIC_SET("vkp.jobs.queue_depth.main_thread.normal", m_mainQueues[1].size());
  VKP_METRIC_SET("vkp.jobs.queue_depth.main_thread.low", m_mainQueues[2].size());
  VKP_METRIC_SET("vkp.jobs.workers_busy", m_workersBusy.load(std::memory_order_acquire));

  const auto now = steady_now_ns();
  if (total > m_workers.size()) {
    std::uint64_t expected = 0u;
    (void)m_queueOverWorkerSinceNs.compare_exchange_strong(expected,
                                                           now,
                                                           std::memory_order_acq_rel);
  } else {
    m_queueOverWorkerSinceNs.store(0u, std::memory_order_release);
  }
}

bool JobSystem::try_pop_queued_job(vkpt::core::JobHandle& id, std::optional<std::size_t> worker_index) {
  std::scoped_lock lock(m_queueMutex);
  if (worker_index && *worker_index < m_workerQueues.size()) {
    auto& own = m_workerQueues[*worker_index];
    if (!own.empty()) {
      id = own.front();
      own.pop_front();
      record_queue_metrics_locked();
      return true;
    }
    for (std::size_t offset = 1u; offset < m_workerQueues.size(); ++offset) {
      auto& victim = m_workerQueues[(*worker_index + offset) % m_workerQueues.size()];
      if (!victim.empty()) {
        id = victim.back();
        victim.pop_back();
        record_queue_metrics_locked();
        return true;
      }
    }
  }
  if (!m_queue.empty()) {
    id = m_queue.front();
    m_queue.pop_front();
    record_queue_metrics_locked();
    return true;
  }
  for (auto& queue : m_workerQueues) {
    if (!queue.empty()) {
      id = queue.back();
      queue.pop_back();
      record_queue_metrics_locked();
      return true;
    }
  }
  return false;
}

bool JobSystem::try_run_one_queued_job(std::optional<std::size_t> worker_index) {
  std::unique_lock<std::recursive_mutex> serial;
  if (m_deterministic.load(std::memory_order_acquire)) {
    serial = std::unique_lock<std::recursive_mutex>(m_serialMutex);
  }

  vkpt::core::JobHandle id = 0u;
  if (!try_pop_queued_job(id, worker_index)) {
    return false;
  }

  run_job(find_job(id));
  return true;
}

vkpt::core::JobHandle JobSystem::submit_job(JobFunction job) {
  return submit_internal(std::move(job));
}

vkpt::core::Result<vkpt::core::JobHandle> JobSystem::submit_job_result(JobFunction job) {
  const auto id = submit_internal(std::move(job));
  if (id == 0u) {
    record_status_error("submit_job rejected: shutting_down");
    return vkpt::core::Result<vkpt::core::JobHandle>::error(vkpt::core::ErrorCode::Cancelled);
  }
  return vkpt::core::Result<vkpt::core::JobHandle>::ok(id);
}

vkpt::core::JobHandle JobSystem::submit_main_thread_job(JobFunction job) {
  return submit_main_thread_job(std::move(job), JobPriority::Normal);
}

vkpt::core::JobHandle JobSystem::submit_main_thread_job(JobFunction job, JobPriority priority) {
  return submit_internal(std::move(job), {}, true, priority);
}

vkpt::core::Result<vkpt::core::JobHandle> JobSystem::submit_main_thread_job_result(
    JobFunction job,
    JobPriority priority) {
  const auto id = submit_internal(std::move(job), {}, true, priority);
  if (id == 0u) {
    record_status_error("submit_main_thread_job rejected: shutting_down");
    return vkpt::core::Result<vkpt::core::JobHandle>::error(vkpt::core::ErrorCode::Cancelled);
  }
  return vkpt::core::Result<vkpt::core::JobHandle>::ok(id);
}

vkpt::core::JobHandle JobSystem::chain(vkpt::core::JobHandle prev, JobFunction job) {
  if (!job) {
    return create_completed_job();
  }
  return submit_job([this, prev, job = std::move(job)]() mutable {
    std::exception_ptr failure;
    if (!wait(prev, &failure)) {
      if (failure) {
        std::rethrow_exception(failure);
      }
      throw std::runtime_error("chained job predecessor failed");
    }
    job();
  });
}

vkpt::core::Result<vkpt::core::JobHandle> JobSystem::chain_result(vkpt::core::JobHandle prev,
                                                                  JobFunction job) {
  const auto id = chain(prev, std::move(job));
  if (id == 0u) {
    record_status_error("chain rejected: shutting_down");
    return vkpt::core::Result<vkpt::core::JobHandle>::error(vkpt::core::ErrorCode::Cancelled);
  }
  return vkpt::core::Result<vkpt::core::JobHandle>::ok(id);
}

vkpt::core::JobHandle JobSystem::submit_range_job(std::size_t begin, std::size_t end, std::size_t step, JobFunction job) {
  if (!job) {
    return create_completed_job();
  }
  return submit_indexed_range_job(begin, end, step, [job = std::move(job)](std::size_t) mutable {
    job();
  });
}

vkpt::core::Result<vkpt::core::JobHandle> JobSystem::submit_range_job_result(std::size_t begin,
                                                                             std::size_t end,
                                                                             std::size_t step,
                                                                             JobFunction job) {
  const auto id = submit_range_job(begin, end, step, std::move(job));
  if (id == 0u) {
    record_status_error("submit_range_job rejected: shutting_down");
    return vkpt::core::Result<vkpt::core::JobHandle>::error(vkpt::core::ErrorCode::Cancelled);
  }
  return vkpt::core::Result<vkpt::core::JobHandle>::ok(id);
}

vkpt::core::JobHandle JobSystem::submit_indexed_range_job(std::size_t begin,
                                                          std::size_t end,
                                                          std::size_t step,
                                                          IndexedRangeJobFunction job) {
  if (!job) {
    return create_completed_job();
  }

  const std::size_t stride = step == 0u ? 1u : step;
  const std::size_t iterations = iteration_count(begin, end, stride);
  if (iterations == 0u) {
    return create_completed_job();
  }

  if (m_deterministic.load(std::memory_order_acquire) || worker_count() <= 1u || iterations == 1u) {
    // Deterministic mode preserves submission order by collapsing a range into
    // one queued job instead of racing chunks across workers.
    return submit_internal([begin, stride, iterations, job = std::move(job)]() mutable {
      for (std::size_t ordinal = 0u; ordinal < iterations; ++ordinal) {
        job(begin + ordinal * stride);
      }
    });
  }

  const std::size_t target_chunks = std::max<std::size_t>(1u, worker_count() * 4u);
  const std::size_t chunk_count = std::min(iterations, target_chunks);
  const std::size_t chunk_size = (iterations + chunk_count - 1u) / chunk_count;
  const auto group_id = create_group(chunk_count);
  auto group = find_job(group_id);
  if (!group) {
    return 0u;
  }

  for (std::size_t chunk = 0u; chunk < chunk_count; ++chunk) {
    const std::size_t first_ordinal = chunk * chunk_size;
    const std::size_t last_ordinal = std::min(iterations, first_ordinal + chunk_size);
    submit_internal([begin, stride, first_ordinal, last_ordinal, job]() mutable {
      for (std::size_t ordinal = first_ordinal; ordinal < last_ordinal; ++ordinal) {
        job(begin + ordinal * stride);
      }
    }, group);
  }

  return group_id;
}

vkpt::core::Result<vkpt::core::JobHandle> JobSystem::submit_indexed_range_job_result(
    std::size_t begin,
    std::size_t end,
    std::size_t step,
    IndexedRangeJobFunction job) {
  const auto id = submit_indexed_range_job(begin, end, step, std::move(job));
  if (id == 0u) {
    record_status_error("submit_indexed_range_job rejected: shutting_down");
    return vkpt::core::Result<vkpt::core::JobHandle>::error(vkpt::core::ErrorCode::Cancelled);
  }
  return vkpt::core::Result<vkpt::core::JobHandle>::ok(id);
}

bool JobSystem::wait(vkpt::core::JobHandle id) {
  return wait(id, {}, nullptr);
}

bool JobSystem::wait(vkpt::core::JobHandle id, std::exception_ptr* out_exception) {
  return wait(id, {}, out_exception);
}

bool JobSystem::wait(vkpt::core::JobHandle id, std::stop_token stop) {
  return wait(id, stop, nullptr);
}

JobWaitResult JobSystem::wait_result(vkpt::core::JobHandle id) {
  return wait_result(id, {});
}

JobWaitResult JobSystem::wait_result(vkpt::core::JobHandle id, std::stop_token stop) {
  std::exception_ptr failure;
  const bool completed = wait(id, stop, &failure);
  if (failure) {
    record_status_error("job_failed");
    return JobWaitResult{
        JobStatusError(vkpt::core::StatusCode::InternalError, "job threw an exception"),
        failure};
  }
  if (stop.stop_requested()) {
    record_status_error("wait cancelled");
    return JobWaitResult{
        JobStatusError(vkpt::core::StatusCode::Cancelled, "wait cancelled"),
        nullptr};
  }
  if (!completed) {
    record_status_error("job not found or incomplete");
    return JobWaitResult{
        JobStatusError(vkpt::core::StatusCode::NotReady, "job not found or incomplete"),
        nullptr};
  }
  return JobWaitResult{vkpt::core::Status::ok(), nullptr};
}

bool JobSystem::wait(vkpt::core::JobHandle id,
                     std::stop_token stop,
                     std::exception_ptr* out_exception) {
  const auto wait_start_ns = steady_now_ns();
  auto target = find_job(id);
  if (!target) {
    CompletedResult completed;
    const bool found = find_completed_result(id, completed);
    if (out_exception != nullptr && found) {
      *out_exception = completed.failure;
    }
    const auto wait_end_ns = steady_now_ns();
    if (wait_end_ns >= wait_start_ns) {
      VKP_METRIC_OBSERVE("vkp.jobs.wait_block_us", (wait_end_ns - wait_start_ns) / 1000u);
    }
    return found && completed.ok && !stop.stop_requested();
  }

  while (target->pending.load(std::memory_order_acquire) != 0u) {
    if (m_waitingThreadRunsJobs && try_run_one_queued_job()) {
      continue;
    }
    std::unique_lock lock(target->mutex);
    // complete_job() always notifies the per-job CV under the same mutex once
    // pending hits zero, so wait() blocks until that signal instead of polling
    // every 1ms (audit J1). A 100ms safety net keeps the waiter alive across
    // exotic tear-down sequences where a notify could be racing with shutdown
    // bookkeeping; the predicate re-checks `pending` on each spurious wake.
    target->cv.wait_for(lock, std::chrono::milliseconds(100), [&]() {
      return target->pending.load(std::memory_order_acquire) == 0u;
    });
  }

  bool ok = false;
  std::exception_ptr failure;
  {
    std::scoped_lock lock(target->mutex);
    ok = target->failure == nullptr;
    failure = target->failure;
  }
  if (out_exception != nullptr) {
    *out_exception = failure;
  }
  target.reset();
  retire_job_tree(id);
  const auto wait_end_ns = steady_now_ns();
  if (wait_end_ns >= wait_start_ns) {
    VKP_METRIC_OBSERVE("vkp.jobs.wait_block_us", (wait_end_ns - wait_start_ns) / 1000u);
  }
  return ok && !stop.stop_requested();
}

bool JobSystem::wait_group(const std::vector<vkpt::core::JobHandle>& ids) {
  return wait_group(ids, {});
}

bool JobSystem::wait_group(const std::vector<vkpt::core::JobHandle>& ids, std::stop_token stop) {
  bool ok = true;
  for (const auto id : ids) {
    ok = wait(id, stop) && ok;
  }
  return ok;
}

vkpt::core::Status JobSystem::wait_group_status(const std::vector<vkpt::core::JobHandle>& ids) {
  return wait_group_status(ids, {});
}

vkpt::core::Status JobSystem::wait_group_status(const std::vector<vkpt::core::JobHandle>& ids,
                                                std::stop_token stop) {
  for (const auto id : ids) {
    const auto result = wait_result(id, stop);
    if (!result.status.is_ok()) {
      return result.status;
    }
  }
  return vkpt::core::Status::ok();
}

std::size_t JobSystem::worker_count() const {
  return m_workers.size();
}

void JobSystem::pump_main_thread() {
  while (true) {
    vkpt::core::JobHandle next = 0u;
    {
      std::scoped_lock lock(m_queueMutex);
      for (auto& queue : m_mainQueues) {
        if (!queue.empty()) {
          next = queue.front();
          queue.pop_front();
          break;
        }
      }
      if (next == 0u) {
        return;
      }
      record_queue_metrics_locked();
    }
    run_job(find_job(next));
  }
}

bool JobSystem::deterministic() const {
  return m_deterministic.load(std::memory_order_acquire);
}

void JobSystem::set_deterministic(bool enabled) {
  vkpt::core::DeterminismContext context;
  {
    std::scoped_lock lock(m_determinismMutex);
    context = m_determinismContext;
    context.enabled = enabled;
  }
  set_determinism(context);
}

void JobSystem::set_determinism(const vkpt::core::DeterminismContext& context) {
  vkpt::core::DeterminismContext previous_context;
  {
    std::scoped_lock lock(m_determinismMutex);
    previous_context = m_determinismContext;
    m_determinismContext = context;
  }

  const bool previous = m_deterministic.exchange(context.enabled, std::memory_order_acq_rel);
  VKP_METRIC_SET("vkp.jobs.deterministic", context.enabled ? 1.0 : 0.0);
  if (previous != context.enabled) {
    try {
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Info,
          "jobs",
          "jobs.deterministic_mode",
          {{"enabled", context.enabled ? "true" : "false"}});
    } catch (...) {
    }
  }
  if (previous_context != context) {
    try {
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Info,
          "jobs",
          "jobs.determinism_changed",
          {{"enabled", context.enabled ? "true" : "false"},
           {"base_seed", std::to_string(context.base_seed)},
           {"frame_index", std::to_string(context.frame_index)},
           {"scenario_id", context.scenario_id},
           {"flow_id", std::to_string(context.frame_index)}});
    } catch (...) {
    }
    VKP_LOG(Info,
            "jobs",
            "determinism_changed",
            "enabled",
            context.enabled,
            "base_seed",
            context.base_seed,
            "flow_id",
            static_cast<std::uint64_t>(context.frame_index),
            "scenario_id",
            context.scenario_id);
  }
  m_jobsCv.notify_all();
}

bool JobSystem::waiting_thread_runs_jobs() const {
  return m_waitingThreadRunsJobs;
}

JobSystemStatus JobSystem::status() const {
  JobSystemStatus out;
  out.lifecycle = m_lifecycle.load(std::memory_order_acquire);
  out.worker_count = m_workers.size();
  out.workers_busy = m_workersBusy.load(std::memory_order_acquire);
  if (out.lifecycle == vkpt::core::contracts::ComponentLifecycle::Ready &&
      out.workers_busy > 0u) {
    out.lifecycle = vkpt::core::contracts::ComponentLifecycle::Busy;
  }
  out.last_tick_ns = m_lastTickNs.load(std::memory_order_acquire);
  out.ticks_total = m_ticksTotal.load(std::memory_order_acquire);
  out.errors_total = m_errorsTotal.load(std::memory_order_acquire);
  {
    std::scoped_lock lock(m_statusMutex);
    out.last_error = m_lastError;
  }
  {
    std::scoped_lock lock(m_determinismMutex);
    out.deterministic = m_determinismContext.enabled;
    out.determinism_base_seed = m_determinismContext.base_seed;
    out.determinism_frame_index = m_determinismContext.frame_index;
    out.determinism_scenario_id = m_determinismContext.scenario_id;
    out.current_flow_id = m_determinismContext.frame_index;
  }
  out.jobs_submitted_total = m_jobsSubmittedTotal.load(std::memory_order_acquire);
  out.jobs_completed_total = m_jobsCompletedTotal.load(std::memory_order_acquire);
  out.jobs_failed_total = m_jobsFailedTotal.load(std::memory_order_acquire);

  std::uint64_t oldest_submitted_ns = 0u;
  {
    std::scoped_lock locks(m_jobsMutex, m_queueMutex);
    out.main_thread_queue_depth = main_queue_depth(m_mainQueues);
    out.queue_depth_total = m_queue.size() + out.main_thread_queue_depth;
    out.queue_depth_per_worker.reserve(m_workerQueues.size());
    for (const auto& queue : m_workerQueues) {
      out.queue_depth_per_worker.push_back(queue.size());
      out.queue_depth_total += queue.size();
    }
    auto inspect = [&](vkpt::core::JobHandle id) {
      const auto it = m_jobs.find(id);
      if (it == m_jobs.end() || !it->second) {
        return;
      }
      const auto submitted = it->second->submitted_ns;
      if (submitted != 0u && (oldest_submitted_ns == 0u || submitted < oldest_submitted_ns)) {
        oldest_submitted_ns = submitted;
      }
    };
    for (const auto id : m_queue) {
      inspect(id);
    }
    for (const auto& queue : m_mainQueues) {
      for (const auto id : queue) {
        inspect(id);
      }
    }
    for (const auto& queue : m_workerQueues) {
      for (const auto id : queue) {
        inspect(id);
      }
    }
  }

  if (oldest_submitted_ns != 0u) {
    const auto now = steady_now_ns();
    if (now >= oldest_submitted_ns) {
      out.oldest_pending_us = (now - oldest_submitted_ns) / 1000u;
    }
  }

  const auto over_since = m_queueOverWorkerSinceNs.load(std::memory_order_acquire);
  if (over_since != 0u) {
    const auto now = steady_now_ns();
    out.queue_starved = now > over_since && (now - over_since) > 2'000'000'000ull;
  }
  return out;
}

std::shared_ptr<vkpt::core::health::IHealthProbe> JobSystem::create_health_probe() const {
  class JobSystemHealthProbe final : public vkpt::core::health::IHealthProbe {
   public:
    explicit JobSystemHealthProbe(const JobSystem* jobs) : m_jobs(jobs) {}

    std::string name() const override { return std::string(kJobsSubsystemName); }

    vkpt::core::health::Report check() override {
      if (m_jobs == nullptr) {
        return {vkpt::core::health::Status::Failed, "job system unavailable"};
      }
      return EvaluateJobSystemHealth(m_jobs->status());
    }

   private:
    const JobSystem* m_jobs = nullptr;
  };

  return std::make_shared<JobSystemHealthProbe>(this);
}

bool JobSystem::shutdown() {
  m_lifecycle.store(vkpt::core::contracts::ComponentLifecycle::ShuttingDown,
                    std::memory_order_release);
  const bool was_stopped = m_stopped.exchange(true, std::memory_order_acq_rel);
  m_jobsCv.notify_all();
  if (was_stopped) {
    return true;
  }

  pump_main_thread();
  for (auto& worker : m_workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  pump_main_thread();

  // Audit J10: don't lie about dropped jobs. Once workers have joined and the
  // main-thread queues have been pumped, anything still resident in m_queue
  // or m_workerQueues is being abandoned. Count it under m_queueMutex so the
  // log reflects reality (SYSTEM.md §6.5: "Drain m_queue on shutdown, or
  // document the drop").
  std::size_t dropped = 0u;
  {
    std::scoped_lock lock(m_queueMutex);
    dropped = m_queue.size();
    for (const auto& queue : m_workerQueues) {
      dropped += queue.size();
    }
    for (const auto& queue : m_mainQueues) {
      dropped += queue.size();
    }
  }
  record_status_tick();
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        "jobs",
        "stopped",
        {{"dropped_count", std::to_string(dropped)}});
  } catch (...) {
  }
  return true;
}

vkpt::core::Status JobSystem::shutdown_status() {
  return shutdown() ? vkpt::core::Status::ok()
                    : JobStatusError(vkpt::core::StatusCode::InternalError,
                                     "job system shutdown failed");
}

void JobSystem::worker_loop(std::size_t worker_index) {
  ApplyCurrentThreadPriority(m_workerPriority);
  while (true) {
    {
      std::unique_lock lock(m_queueMutex);
      m_jobsCv.wait(lock, [this]() {
        return m_stopped.load(std::memory_order_acquire) || has_queued_work_locked();
      });
      if (m_stopped.load(std::memory_order_acquire) && !has_queued_work_locked()) {
        return;
      }
    }

    if (!try_run_one_queued_job(worker_index)) {
      std::this_thread::yield();
    }
  }
}

}  // namespace vkpt::jobs
