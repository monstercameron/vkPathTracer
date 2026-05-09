// Smoke test for vkpt::sim::SimWorker.
//
// Step 1 coverage:
//   - Construction with null deps must succeed (it's just a channel
//     carrier at this stage).
//   - submit_input() must accept frames and report them through the
//     submission counter.
//   - start()/stop() lifecycle must be idempotent.
//   - status() must return a populated SubsystemStatus.
//   - latest_ui_mirror() must return a default-constructed mirror until
//     the worker has published one (Step 4 territory; for now we only
//     assert the call doesn't crash).

#include <cstdio>
#include <iostream>

#include "sim/SimWorker.h"

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "sim_worker_smoke: FAIL: " << message << "\n";
    return false;
  }
  return true;
}

int RunSmoke() {
  vkpt::sim::SimWorker::Deps deps{};
  vkpt::sim::SimWorker worker(deps);

  if (!Check(worker.inputs_submitted_total() == 0u,
             "fresh worker reports 0 inputs submitted")) {
    return 1;
  }
  if (!Check(worker.inputs_consumed_total() == 0u,
             "fresh worker reports 0 inputs consumed")) {
    return 1;
  }
  if (!Check(worker.inputs_dropped_total() == 0u,
             "fresh worker reports 0 inputs dropped")) {
    return 1;
  }

  // Pre-start submission must be accepted into the ring buffer.
  vkpt::sim::SimInputFrame frame{};
  frame.sequence = 1u;
  frame.dt = 1.0f / 60.0f;
  frame.frame_index = 7u;
  frame.keys_down = {65, 66};  // 'A', 'B'
  frame.viewport_focused = true;
  frame.play_mode = false;
  frame.fps_mode = true;

  if (!Check(worker.submit_input(frame),
             "submit_input accepts a frame on a fresh ring")) {
    return 1;
  }
  if (!Check(worker.inputs_submitted_total() == 1u,
             "inputs_submitted_total increments on each submit")) {
    return 1;
  }

  // Start/stop should be idempotent.
  worker.start();
  worker.start();  // second call is a no-op
  worker.stop();
  worker.stop();   // idempotent

  // Status should be readable after stop().
  const auto status = worker.status();
  if (!Check(status.name == "sim_worker", "status name is sim_worker")) {
    return 1;
  }
  if (!Check(status.status == vkpt::core::contracts::SubsystemHealth::Ok,
             "status health is Ok at rest")) {
    return 1;
  }

  // Mirror reads must not crash before any publish has happened.
  const auto mirror = worker.latest_ui_mirror();
  if (!Check(mirror.sim_frame == 0u,
             "mirror sim_frame defaults to zero before any publish")) {
    return 1;
  }

  // Submit-after-stop should still enqueue safely (no consumer to drain,
  // but the ring is independent of thread state).
  if (!Check(worker.submit_input(frame),
             "submit_input still enqueues after stop")) {
    return 1;
  }

  std::cout << "sim_worker_smoke: OK\n";
  return 0;
}

}  // namespace

int main() {
  try {
    return RunSmoke();
  } catch (const std::exception& error) {
    std::cerr << "sim_worker_smoke: exception: " << error.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "sim_worker_smoke: unknown exception\n";
    return 1;
  }
}
