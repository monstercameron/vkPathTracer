#include "jobs/JobSystem.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <thread>

namespace vkpt::jobs {

namespace {

struct JobState {
  vkpt::core::RuntimeHandle id = 0u;
  std::atomic<int> pending = 1;
  std::mutex mutex;
  std::condition_variable cv;
  JobFunction job;
};

}  // namespace

struct JobSystem::JobEntry {
  JobState state;
  bool is_main_thread_job = false;
};

static void RunJob(JobState& state) {
  if (state.job) {
    state.job();
  }
  {
    std::scoped_lock lock(state.mutex);
    state.pending = 0;
  }
  state.cv.notify_all();
}

JobSystem::JobSystem(std::size_t workerCount) {
  const auto requested = workerCount == 0u ? std::max<std::size_t>(1u, std::thread::hardware_concurrency()) : workerCount;
  m_nextWorkerCount = requested;
  m_workers.reserve(requested);
  for (std::size_t i = 0; i < requested; ++i) {
    m_workers.emplace_back([this]() { worker_loop(); });
  }
}

JobSystem::~JobSystem() {
  shutdown();
}

vkpt::core::RuntimeHandle JobSystem::submit_internal(JobFunction job) {
  auto id = m_nextJobId.fetch_add(1, std::memory_order_relaxed);
  {
    std::scoped_lock lock(m_jobsMutex);
    m_jobs.push_back(std::make_unique<JobEntry>());
    auto& entry = *m_jobs.back();
    entry.state.id = id;
    entry.state.job = std::move(job);
  }
  {
    std::scoped_lock lock(m_queueMutex);
    m_queue.push_back(id);
  }
  m_jobsCv.notify_one();
  return id;
}

vkpt::core::RuntimeHandle JobSystem::submit_job(JobFunction job) {
  const auto id = submit_internal(std::move(job));
  return id;
}

vkpt::core::RuntimeHandle JobSystem::submit_range_job(std::size_t begin, std::size_t end, std::size_t step, JobFunction job) {
  auto wrapped = [begin, end, step, job = std::move(job)]() {
    for (std::size_t i = begin; i < end; i += (step == 0u ? 1u : step)) {
      (void)i;
      if (job) {
        job();
      }
    }
  };
  const auto id = submit_internal(std::move(wrapped));
  return id;
}

bool JobSystem::wait(vkpt::core::RuntimeHandle id) {
  JobState* target = nullptr;
  {
    std::scoped_lock lock(m_jobsMutex);
    auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [id](const std::unique_ptr<JobEntry>& e) { return e->state.id == id; });
    if (it == m_jobs.end()) {
      return false;
    }
    target = &(*it)->state;
  }
  std::unique_lock lock(target->mutex);
  target->cv.wait(lock, [&]() { return target->pending.load(std::memory_order_acquire) == 0; });
  return true;
}

bool JobSystem::wait_group(const std::vector<vkpt::core::RuntimeHandle>& ids) {
  for (const auto id : ids) {
    if (!wait(id)) {
      return false;
    }
  }
  return true;
}

std::size_t JobSystem::worker_count() const {
  return m_workers.size();
}

void JobSystem::pump_main_thread() {
  while (true) {
    vkpt::core::RuntimeHandle next = 0u;
    {
      std::scoped_lock lock(m_mainMutex);
      if (m_mainQueue.empty()) {
        return;
      }
      next = m_mainQueue.front();
      m_mainQueue.pop_front();
    }
    JobState* target = nullptr;
    {
      std::scoped_lock lock(m_jobsMutex);
      auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [next](const std::unique_ptr<JobEntry>& e) { return e->state.id == next; });
      if (it == m_jobs.end()) {
        continue;
      }
      target = &(*it)->state;
    }
    RunJob(*target);
  }
}

bool JobSystem::deterministic() const {
  return m_deterministic;
}

void JobSystem::set_deterministic(bool enabled) {
  m_deterministic = enabled;
}

bool JobSystem::shutdown() {
  {
    std::scoped_lock lock(m_queueMutex);
    m_stopped = true;
  }
  m_jobsCv.notify_all();

  for (auto& worker : m_workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  return true;
}

void JobSystem::worker_loop() {
  while (true) {
    std::unique_lock lock(m_queueMutex);
    m_jobsCv.wait(lock, [this]() { return m_stopped || !m_queue.empty(); });
    if (m_stopped && m_queue.empty()) {
      return;
    }
    if (!m_deterministic && !m_queue.empty()) {
      const auto id = m_queue.front();
      m_queue.pop_front();
      lock.unlock();

      JobState* target = nullptr;
      {
        std::scoped_lock jobsLock(m_jobsMutex);
        auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [id](const std::unique_ptr<JobEntry>& e) { return e->state.id == id; });
        if (it == m_jobs.end()) {
          continue;
        }
        target = &(*it)->state;
      }
      if (target && target->job) {
        RunJob(*target);
      }
      continue;
    }

    if (m_deterministic && !m_queue.empty()) {
      const auto id = m_queue.front();
      m_queue.pop_front();
      lock.unlock();

      JobState* target = nullptr;
      {
        std::scoped_lock jobsLock(m_jobsMutex);
        auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [id](const std::unique_ptr<JobEntry>& e) { return e->state.id == id; });
        if (it == m_jobs.end()) {
          continue;
        }
        target = &(*it)->state;
      }
      if (target && target->job) {
        // serialize: only one worker executes at a time in deterministic mode
        std::scoped_lock serial(m_serialMutex);
        RunJob(*target);
      }
    }
  }
}

}  // namespace vkpt::jobs
