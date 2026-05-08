#include "core/metrics/UiInputLatency.h"

#include <array>
#include <atomic>
#include <chrono>

namespace vkpt::core::metrics {

namespace {

constexpr std::size_t kRingSize = 256u;

struct RingSlot {
  std::atomic<std::uint64_t> generation{0u};
  std::atomic<std::uint64_t> input_ns{0u};
};

std::atomic<std::uint64_t> g_pending_input_ns{0u};
std::atomic<std::uint64_t> g_ring_pos{0u};
std::array<RingSlot, kRingSize> g_ring{};

}  // namespace

std::uint64_t UiInputLatencyNowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void RecordInputEvent(std::uint64_t input_ns) noexcept {
  g_pending_input_ns.store(input_ns, std::memory_order_release);
}

std::uint64_t ConsumePendingInputForPublish() noexcept {
  return g_pending_input_ns.exchange(0u, std::memory_order_acq_rel);
}

void RegisterPublishInput(std::uint64_t generation, std::uint64_t input_ns) noexcept {
  if (input_ns == 0u || generation == 0u) {
    return;
  }
  const auto pos = g_ring_pos.fetch_add(1u, std::memory_order_relaxed) % kRingSize;
  // Order matters: write input_ns first, then generation, so a reader that
  // observes the generation has a valid timestamp.
  g_ring[pos].input_ns.store(input_ns, std::memory_order_relaxed);
  g_ring[pos].generation.store(generation, std::memory_order_release);
}

std::uint64_t ResolveInputNsForGeneration(std::uint64_t generation) noexcept {
  if (generation == 0u) {
    return 0u;
  }
  for (auto& slot : g_ring) {
    if (slot.generation.load(std::memory_order_acquire) == generation) {
      return slot.input_ns.load(std::memory_order_relaxed);
    }
  }
  return 0u;
}

}  // namespace vkpt::core::metrics
