#include "jobs/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace vkpt::jobs {

struct JobSystem::JobState {
  vkpt::core::JobHandle id = 0u;
  std::atomic<std::size_t> pending = 1u;
  std::mutex mutex;
  std::condition_variable cv;
  JobFunction job;
  std::weak_ptr<JobState> parent;
  std::vector<vkpt::core::JobHandle> children;
  std::exception_ptr failure;
};

namespace {

std::size_t iteration_count(std::size_t begin, std::size_t end, std::size_t step) {
  if (begin >= end) {
    return 0u;
  }
  const std::size_t stride = step == 0u ? 1u : step;
  return ((end - begin) + stride - 1u) / stride;
}

}  // namespace

JobSystem::JobSystem(std::size_t workerCount) {
  const auto requested = workerCount == 0u ? std::max<std::size_t>(1u, std::thread::hardware_concurrency()) : workerCount;
  m_workers.reserve(requested);
  for (std::size_t i = 0; i < requested; ++i) {
    m_workers.emplace_back([this]() { worker_loop(); });
  }
}

JobSystem::~JobSystem() {
  shutdown();
}

vkpt::core::JobHandle JobSystem::submit_internal(JobFunction job,
                                                 std::shared_ptr<JobState> parent,
                                                 bool main_thread) {
  if (m_stopped.load(std::memory_order_acquire)) {
    return 0u;
  }

  auto state = std::make_shared<JobState>();
  state->id = m_nextJobId.fetch_add(1u, std::memory_order_relaxed);
  state->job = std::move(job);
  state->parent = std::move(parent);
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
      m_mainQueue.push_back(state->id);
    } else {
      m_queue.push_back(state->id);
    }
  }
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
  {
    std::scoped_lock lock(m_jobsMutex);
    m_jobs.emplace(state->id, state);
  }
  if (pending_count == 0u) {
    state->cv.notify_all();
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
    }
  }
  state->cv.notify_all();

  if (parent) {
    complete_job(parent, failure);
  }
}

void JobSystem::run_job(const std::shared_ptr<JobState>& state) {
  if (!state) {
    return;
  }

  std::exception_ptr failure;
  try {
    if (state->job) {
      state->job();
    }
  } catch (...) {
    failure = std::current_exception();
  }
  complete_job(state, failure);
}

bool JobSystem::try_run_one_queued_job() {
  std::unique_lock<std::recursive_mutex> serial;
  if (m_deterministic.load(std::memory_order_acquire)) {
    serial = std::unique_lock<std::recursive_mutex>(m_serialMutex);
  }

  vkpt::core::JobHandle id = 0u;
  {
    std::scoped_lock lock(m_queueMutex);
    if (m_queue.empty()) {
      return false;
    }
    id = m_queue.front();
    m_queue.pop_front();
  }

  run_job(find_job(id));
  return true;
}

vkpt::core::JobHandle JobSystem::submit_job(JobFunction job) {
  return submit_internal(std::move(job));
}

vkpt::core::JobHandle JobSystem::submit_main_thread_job(JobFunction job) {
  return submit_internal(std::move(job), {}, true);
}

vkpt::core::JobHandle JobSystem::submit_range_job(std::size_t begin, std::size_t end, std::size_t step, JobFunction job) {
  if (!job) {
    return create_completed_job();
  }
  return submit_indexed_range_job(begin, end, step, [job = std::move(job)](std::size_t) mutable {
    job();
  });
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

bool JobSystem::wait(vkpt::core::JobHandle id) {
  return wait(id, {});
}

bool JobSystem::wait(vkpt::core::JobHandle id, std::stop_token stop) {
  auto target = find_job(id);
  if (!target) {
    return false;
  }

  while (target->pending.load(std::memory_order_acquire) != 0u) {
    if (try_run_one_queued_job()) {
      continue;
    }
    std::unique_lock lock(target->mutex);
    target->cv.wait_for(lock, std::chrono::milliseconds(1), [&]() {
      return target->pending.load(std::memory_order_acquire) == 0u;
    });
  }

  bool ok = false;
  {
    std::scoped_lock lock(target->mutex);
    ok = target->failure == nullptr;
  }
  target.reset();
  retire_job_tree(id);
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

std::size_t JobSystem::worker_count() const {
  return m_workers.size();
}

void JobSystem::pump_main_thread() {
  while (true) {
    vkpt::core::JobHandle next = 0u;
    {
      std::scoped_lock lock(m_queueMutex);
      if (m_mainQueue.empty()) {
        return;
      }
      next = m_mainQueue.front();
      m_mainQueue.pop_front();
    }
    run_job(find_job(next));
  }
}

bool JobSystem::deterministic() const {
  return m_deterministic.load(std::memory_order_acquire);
}

void JobSystem::set_deterministic(bool enabled) {
  m_deterministic.store(enabled, std::memory_order_release);
  m_jobsCv.notify_all();
}

bool JobSystem::shutdown() {
  const bool was_stopped = m_stopped.exchange(true, std::memory_order_acq_rel);
  m_jobsCv.notify_all();
  if (was_stopped) {
    return true;
  }

  for (auto& worker : m_workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  return true;
}

void JobSystem::worker_loop() {
  while (true) {
    {
      std::unique_lock lock(m_queueMutex);
      m_jobsCv.wait(lock, [this]() { return m_stopped.load(std::memory_order_acquire) || !m_queue.empty(); });
      if (m_stopped.load(std::memory_order_acquire) && m_queue.empty()) {
        return;
      }
    }

    if (!try_run_one_queued_job()) {
      std::this_thread::yield();
    }
  }
}

}  // namespace vkpt::jobs
