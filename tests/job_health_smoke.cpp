#include "jobs/JobSystem.h"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "job_health_smoke: " << message << "\n";
    return false;
  }
  return true;
}

bool CheckPureHealthRules() {
  using vkpt::core::health::Status;

  vkpt::jobs::JobSystemStatus shallowQueue;
  shallowQueue.worker_count = 4u;
  shallowQueue.workers_busy = 0u;
  shallowQueue.queue_depth_total = 1u;
  shallowQueue.oldest_pending_us = 2'100'000u;
  const auto shallowReport = vkpt::jobs::EvaluateJobSystemHealth(shallowQueue);

  vkpt::jobs::JobSystemStatus youngQueue;
  youngQueue.worker_count = 2u;
  youngQueue.workers_busy = 0u;
  youngQueue.queue_depth_total = 3u;
  youngQueue.oldest_pending_us = 500'000u;
  const auto youngReport = vkpt::jobs::EvaluateJobSystemHealth(youngQueue);

  vkpt::jobs::JobSystemStatus failedStarvation;
  failedStarvation.worker_count = 2u;
  failedStarvation.workers_busy = 0u;
  failedStarvation.queue_depth_total = 3u;
  failedStarvation.oldest_pending_us = 2'100'000u;
  const auto failedReport = vkpt::jobs::EvaluateJobSystemHealth(failedStarvation);

  vkpt::jobs::JobSystemStatus busyWithQueue;
  busyWithQueue.worker_count = 2u;
  busyWithQueue.workers_busy = 1u;
  busyWithQueue.queue_depth_total = 3u;
  const auto busyReport = vkpt::jobs::EvaluateJobSystemHealth(busyWithQueue);

  vkpt::jobs::JobSystemStatus queueStarved;
  queueStarved.worker_count = 2u;
  queueStarved.workers_busy = 1u;
  queueStarved.queue_depth_total = 5u;
  queueStarved.queue_starved = true;
  const auto degradedReport = vkpt::jobs::EvaluateJobSystemHealth(queueStarved);

  return Check(shallowReport.status == Status::Ok,
               "shallow queued work should not trip starvation health") &&
         Check(youngReport.status == Status::Ok,
               "young queued work should wait for the starvation window") &&
         Check(failedReport.status == Status::Failed,
               "stale queue over worker count with zero busy workers should fail health") &&
         Check(failedReport.reason.find("queue_depth_total=3") !=
                   std::string_view::npos,
               "failed health should explain queued depth") &&
         Check(busyReport.status == Status::Ok,
               "queued work with an active worker should remain healthy") &&
         Check(degradedReport.status == Status::Degraded,
               "legacy queue starvation flag should degrade health");
}

bool CheckJobNamingContract() {
  static_assert(std::is_same_v<vkpt::jobs::JobsSubsystemStatus,
                               vkpt::jobs::JobSystemStatus>);

  vkpt::jobs::JobsSubsystemStatus status;
  return Check(vkpt::jobs::JobsSubsystemName() == "jobs",
               "jobs subsystem should expose a stable canonical name") &&
         Check(std::string_view(status.name) == vkpt::jobs::kJobsSubsystemName,
               "job status should carry the canonical subsystem name") &&
         Check(vkpt::jobs::kJobSystemNamingContract.status_type_name ==
                   "JobSystemStatus",
               "job naming contract should expose the status type name") &&
         Check(vkpt::jobs::kJobSystemNamingContract.health_probe_name ==
                   vkpt::jobs::kJobsSubsystemName,
               "job health probe name should match the subsystem name") &&
         Check(vkpt::jobs::kJobSystemNamingContract.queue_depth_field_name ==
                   "queue_depth_total",
               "job naming contract should pin queue depth field naming") &&
         Check(vkpt::jobs::kJobSystemNamingContract.job_system_contract ==
                   vkpt::jobs::kJobSystemContractName,
               "job system contract should be source-proofable");
}

bool CheckLiveProbe() {
  using vkpt::core::health::Status;

  vkpt::jobs::JobSystem jobs(1u);
  const auto probe = jobs.create_health_probe();
  if (!Check(static_cast<bool>(probe), "job health probe should be created") ||
      !Check(probe->name() == "jobs", "job health probe should be named jobs")) {
    jobs.shutdown();
    return false;
  }

  const auto idle = probe->check();
  std::atomic_bool ran = false;
  const auto job = jobs.submit_main_thread_job([&]() { ran.store(true); });
  const auto queued = probe->check();
  jobs.pump_main_thread();
  const bool waited = jobs.wait(job);
  const auto drained = probe->check();
  jobs.shutdown();

  return Check(idle.status == Status::Ok,
               "idle job system should be healthy") &&
         Check(queued.status == Status::Ok,
               "fresh main-thread queued work should not fail before the starvation window") &&
         Check(waited && ran.load(),
               "main-thread queued test job should run") &&
         Check(drained.status == Status::Ok,
               "drained job queue should recover health");
}

bool CheckDependenciesAndMainThreadPriorities() {
  vkpt::jobs::JobSystem jobs(1u);

  std::vector<int> order;
  const auto low = jobs.submit_main_thread_job(
      [&]() { order.push_back(3); }, vkpt::jobs::JobPriority::Low);
  const auto high = jobs.submit_main_thread_job(
      [&]() { order.push_back(1); }, vkpt::jobs::JobPriority::High);
  const auto normal = jobs.submit_main_thread_job(
      [&]() { order.push_back(2); }, vkpt::jobs::JobPriority::Normal);
  jobs.pump_main_thread();
  const bool prioritiesWaited = jobs.wait(low) && jobs.wait(high) && jobs.wait(normal);

  std::vector<int> chainOrder;
  const auto first = jobs.submit_job([&]() { chainOrder.push_back(1); });
  const auto second = jobs.chain(first, [&]() { chainOrder.push_back(2); });
  const bool chainWaited = jobs.wait(second);
  jobs.shutdown();

  return Check(prioritiesWaited,
               "priority main-thread jobs should complete") &&
         Check(order.size() == 3u &&
                   order[0] == 1 &&
                   order[1] == 2 &&
                   order[2] == 3,
               "main-thread queue should run high, normal, then low priority") &&
         Check(chainWaited,
               "chained job should complete after its predecessor") &&
         Check(chainOrder.size() == 2u &&
                   chainOrder[0] == 1 &&
                   chainOrder[1] == 2,
               "chained job should run after the predecessor");
}

bool CheckDeterminismContextPropagation() {
  vkpt::jobs::JobSystem jobs(1u);
  const auto context =
      vkpt::core::MakeDeterminismContext(true, 0x5678u, 11u, "job-health-smoke");

  jobs.set_determinism(context);
  const auto status = jobs.status();
  jobs.shutdown();

  return Check(jobs.deterministic(),
               "job system should enable deterministic mode from context") &&
         Check(status.deterministic &&
                   status.determinism_base_seed == context.base_seed &&
                   status.determinism_frame_index == context.frame_index &&
                   status.determinism_scenario_id == context.scenario_id &&
                   status.current_flow_id == context.frame_index,
               "job status should retain DeterminismContext fields and flow id");
}

bool CheckResultStatusAndInterfaceContract() {
  using vkpt::core::contracts::ComponentLifecycle;

  vkpt::jobs::JobSystem jobs(1u);
  vkpt::jobs::IJobSystem* iface = &jobs;
  const auto initial = iface->status();

  std::atomic_bool ran = false;
  const auto submitted = iface->submit_job_result([&]() { ran.store(true); });
  if (!Check(static_cast<bool>(submitted),
             "IJobSystem submit_job_result should return a typed handle result")) {
    jobs.shutdown();
    return false;
  }
  const auto waited = iface->wait_result(submitted.value());

  const auto failing = iface->submit_job_result([]() {
    throw std::runtime_error("typed wait failure");
  });
  if (!Check(static_cast<bool>(failing),
             "IJobSystem submit_job_result should accept failing job bodies")) {
    jobs.shutdown();
    return false;
  }
  const auto failed_wait = iface->wait_result(failing.value());
  const auto failed_status = iface->status();

  const auto shutdown = iface->shutdown_status();
  const auto stopped = iface->status();

  return Check(initial.lifecycle == ComponentLifecycle::Ready,
               "job status should expose Ready lifecycle after construction") &&
         Check(waited.status.is_ok() && ran.load(),
               "wait_result should report successful job completion") &&
         Check(!failed_wait.status.is_ok() && failed_wait.exception != nullptr,
               "wait_result should preserve thrown job exception details") &&
         Check(failed_status.errors_total >= 1u && !failed_status.last_error.empty(),
               "job status should count and retain the last job error") &&
         Check(shutdown.is_ok(),
               "shutdown_status should expose typed shutdown success") &&
         Check(stopped.lifecycle == ComponentLifecycle::ShuttingDown,
               "job status should expose ShuttingDown lifecycle after shutdown");
}

}  // namespace

int main() {
  if (!CheckJobNamingContract()) {
    return 1;
  }
  if (!CheckPureHealthRules()) {
    return 1;
  }
  if (!CheckLiveProbe()) {
    return 1;
  }
  if (!CheckDependenciesAndMainThreadPriorities()) {
    return 1;
  }
  if (!CheckDeterminismContextPropagation()) {
    return 1;
  }
  if (!CheckResultStatusAndInterfaceContract()) {
    return 1;
  }
  std::cout << "job_health_smoke: ok\n";
  return 0;
}
