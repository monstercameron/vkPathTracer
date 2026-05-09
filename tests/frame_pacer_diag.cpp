#include "core/pacing/FramePacer.h"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
  using clock = std::chrono::steady_clock;
  vkpt::core::pacing::FramePacer pacer{std::chrono::microseconds(16667)};
  const auto stats0 = pacer.stats();
  std::printf("high_resolution_timer=%s target_us=%llu\n",
              stats0.high_resolution_timer ? "true" : "false",
              static_cast<unsigned long long>(stats0.target_us));

  // Microbenchmark: 10 sleeps of 16.67ms via SleepUntilHighResolution.
  long long worst = 0, total = 0;
  for (int i = 0; i < 60; ++i) {
    const auto target = clock::now() + std::chrono::microseconds(16667);
    vkpt::core::pacing::SleepUntilHighResolution(target);
    const auto delta = std::chrono::duration_cast<std::chrono::microseconds>(
        clock::now() - target).count();
    if (delta > worst) worst = delta;
    total += delta;
  }
  std::printf("60-frame avg overshoot: %lldus, worst: %lldus\n", total / 60, worst);

  // Compare against std::this_thread::sleep_for(16ms).
  long long worst_naive = 0, total_naive = 0;
  for (int i = 0; i < 60; ++i) {
    const auto target = clock::now() + std::chrono::microseconds(16667);
    const auto remaining = target - clock::now();
    std::this_thread::sleep_for(remaining);
    const auto delta = std::chrono::duration_cast<std::chrono::microseconds>(
        clock::now() - target).count();
    if (delta > worst_naive) worst_naive = delta;
    total_naive += delta;
  }
  std::printf("60-frame naive sleep_for avg overshoot: %lldus, worst: %lldus\n",
              total_naive / 60, worst_naive);
  return 0;
}
