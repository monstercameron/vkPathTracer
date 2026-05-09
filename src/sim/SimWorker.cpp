#include "sim/SimWorker.h"

#include <chrono>
#include <utility>

#include "core/contracts/Lifecycle.h"

namespace vkpt::sim {

namespace {
constexpr std::size_t kInputRingCapacity = 1024u;

std::uint64_t NowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
}  // namespace

SimWorker::SimWorker(Deps deps)
    : m_deps(deps),
      m_input_ring(kInputRingCapacity) {}

SimWorker::~SimWorker() {
  stop();
}

void SimWorker::start() {
  // Step 1 skeleton: do not spawn the worker thread yet. Mark as running so
  // status() reports the Ready lifecycle and capture the start timestamp,
  // which downstream observers expect once start() has been called.
  bool expected = false;
  if (!m_running.compare_exchange_strong(expected, true)) {
    return;
  }
  m_started_at_ns.store(NowNs(), std::memory_order_relaxed);
  m_stop_requested.store(false, std::memory_order_relaxed);
  // No std::thread spawn yet. Steps 2+ will replace this with:
  //   m_thread = std::thread([this] { run_loop(); });
}

void SimWorker::stop() {
  m_stop_requested.store(true, std::memory_order_relaxed);
  if (m_thread.joinable()) {
    m_thread.join();
  }
  m_running.store(false, std::memory_order_relaxed);
}

bool SimWorker::submit_input(const SimInputFrame& frame) {
  m_inputs_submitted.fetch_add(1u, std::memory_order_relaxed);
  return m_input_ring.try_push(frame);
}

UiSimMirror SimWorker::latest_ui_mirror() const {
  if (auto latest = m_ui_mirror.take()) {
    std::lock_guard<std::mutex> guard(m_mirror_cache_mutex);
    m_mirror_cache = std::move(*latest);
    return m_mirror_cache;
  }
  std::lock_guard<std::mutex> guard(m_mirror_cache_mutex);
  return m_mirror_cache;
}

vkpt::core::contracts::SubsystemStatus SimWorker::status() const {
  using vkpt::core::contracts::MakeSubsystemStatus;
  using vkpt::core::contracts::SubsystemHealth;
  auto out = MakeSubsystemStatus("sim_worker", SubsystemHealth::Ok);
  out.started_at_ns = m_started_at_ns.load(std::memory_order_relaxed);
  out.last_tick_ns = m_last_tick_ns.load(std::memory_order_relaxed);
  out.ticks_total = m_ticks_total.load(std::memory_order_relaxed);
  out.errors_total = m_errors_total.load(std::memory_order_relaxed);
  out.set_custom("inputs_submitted",
                 std::to_string(m_inputs_submitted.load(std::memory_order_relaxed)));
  out.set_custom("inputs_consumed",
                 std::to_string(m_inputs_consumed.load(std::memory_order_relaxed)));
  out.set_custom("inputs_dropped",
                 std::to_string(m_input_ring.dropped_total()));
  out.set_custom("running",
                 m_running.load(std::memory_order_relaxed) ? "true" : "false");
  return out;
}

std::uint64_t SimWorker::inputs_submitted_total() const noexcept {
  return m_inputs_submitted.load(std::memory_order_relaxed);
}

std::uint64_t SimWorker::inputs_consumed_total() const noexcept {
  return m_inputs_consumed.load(std::memory_order_relaxed);
}

std::uint64_t SimWorker::inputs_dropped_total() const noexcept {
  return m_input_ring.dropped_total();
}

void SimWorker::run_loop() {
  // Step 1 skeleton placeholder. The body is intentionally empty until
  // Step 2 wires physics, then Step 3 wires scripts, then Step 4 wires
  // snapshot publish + UI mirror update. Keep the method present so
  // subsequent steps only edit one method body instead of also adjusting
  // the class shape.
  while (!m_stop_requested.load(std::memory_order_relaxed)) {
    SimInputFrame frame;
    while (m_input_ring.try_pop(frame)) {
      m_inputs_consumed.fetch_add(1u, std::memory_order_relaxed);
    }
    m_last_tick_ns.store(NowNs(), std::memory_order_relaxed);
    m_ticks_total.fetch_add(1u, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::microseconds(m_deps.tick_target));
  }
}

}  // namespace vkpt::sim
