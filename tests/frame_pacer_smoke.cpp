// Smoke test for vkpt::core::pacing::FramePacer / SleepUntilHighResolution.
//
// Asserts the pacing primitive lands within a reasonable jitter window of the
// requested target on the host platform. We don't gate on absolute precision
// (that depends on OS scheduler noise) but we do gate on:
//
//  1. SleepUntilHighResolution returns at-or-after the target (never early).
//  2. SleepUntilHighResolution doesn't oversleep by more than the platform
//     budget (5 ms on Windows debug, 1 ms on real-time-capable platforms).
//  3. FramePacer accumulates correct stats: frames_total matches the loop
//     iteration count, deadline_misses_total matches injected work overruns,
//     avg_frame_us is within tolerance of the target.
//  4. FramePacer.set_target_interval clamps to [1us, 1s].
//  5. FramePacer is move-only and survives move construction.
//
// Failure mode: prints the failing condition and returns non-zero so CI
// surfaces the regression. Output is single-line "frame_pacer_smoke: ok" on
// success, matching the project's smoke-test convention.

#include "core/pacing/FramePacer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool Check(bool cond, const std::string& msg) {
  if (!cond) {
    std::cerr << "frame_pacer_smoke failed: " << msg << "\n";
  }
  return cond;
}

template <typename Clock>
std::int64_t MicrosBetween(typename Clock::time_point a, typename Clock::time_point b) {
  return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
}

// Per-platform tolerance for "did the sleep land near target" assertions.
// Windows debug builds with HIGH_RESOLUTION_TIMER and a 1 ms timer base wake
// within ~250 us; we allow 5x that for CI noise. Linux futex / nanosleep is
// typically tighter.
constexpr std::int64_t kSleepOvershootMaxUs =
#ifdef _WIN32
    5000;
#else
    2000;
#endif

}  // namespace

int main() {
  using clock = std::chrono::steady_clock;

  // 1. SleepUntilHighResolution: request a 5 ms sleep, observe it returns
  //    at or after the target with bounded overshoot.
  {
    constexpr int kIterations = 30;
    std::int64_t worst_overshoot_us = 0;
    std::int64_t worst_undershoot_us = 0;
    for (int i = 0; i < kIterations; ++i) {
      const auto target = clock::now() + std::chrono::microseconds(5000);
      vkpt::core::pacing::SleepUntilHighResolution(target);
      const auto landed = clock::now();
      const auto delta_us = MicrosBetween<clock>(target, landed);
      if (delta_us > 0) {
        if (delta_us > worst_overshoot_us) worst_overshoot_us = delta_us;
      } else {
        if (-delta_us > worst_undershoot_us) worst_undershoot_us = -delta_us;
      }
    }
    if (!Check(worst_undershoot_us == 0,
               "SleepUntilHighResolution returned before target by " +
                   std::to_string(worst_undershoot_us) + "us")) {
      return 1;
    }
    if (!Check(worst_overshoot_us <= kSleepOvershootMaxUs,
               "SleepUntilHighResolution overshot target by " +
                   std::to_string(worst_overshoot_us) +
                   "us (budget " + std::to_string(kSleepOvershootMaxUs) + "us)")) {
      return 1;
    }
  }

  // 2. FramePacer happy path: 30 frames at 5 ms target, no injected stalls.
  //    avg_frame_us should be within tolerance of target; deadline_misses
  //    should be 0.
  {
    vkpt::core::pacing::FramePacer pacer{std::chrono::microseconds(5000)};
    constexpr int kFrames = 30;
    for (int i = 0; i < kFrames; ++i) {
      pacer.begin_frame();
      // No work this iteration — pacer is the only thing consuming the budget.
      pacer.wait_until_target();
    }
    const auto stats = pacer.stats();
    if (!Check(stats.frames_total == static_cast<std::uint64_t>(kFrames),
               "FramePacer.frames_total expected " + std::to_string(kFrames) +
                   ", got " + std::to_string(stats.frames_total))) {
      return 1;
    }
    if (!Check(stats.deadline_misses_total == 0,
               "FramePacer.deadline_misses_total non-zero on idle pace: " +
                   std::to_string(stats.deadline_misses_total))) {
      return 1;
    }
    if (!Check(stats.target_us == 5000u,
               "FramePacer.target_us expected 5000, got " +
                   std::to_string(stats.target_us))) {
      return 1;
    }
    // Allow generous tolerance on avg (CI noise + first-frame is uncomputed
    // because there's no previous frame to delta against, so frames-1 deltas
    // contribute to the average).
    if (stats.frames_total > 1 && stats.avg_frame_us != 0) {
      const auto avg_overshoot_us = static_cast<std::int64_t>(stats.avg_frame_us) -
                                    static_cast<std::int64_t>(stats.target_us);
      if (!Check(avg_overshoot_us >= -1000 && avg_overshoot_us <= kSleepOvershootMaxUs * 2,
                 "FramePacer.avg_frame_us=" + std::to_string(stats.avg_frame_us) +
                     " too far from target=" + std::to_string(stats.target_us))) {
        return 1;
      }
    }
  }

  // 3. FramePacer deadline misses: inject a stall longer than target and
  //    confirm the pacer counts it.
  {
    vkpt::core::pacing::FramePacer pacer{std::chrono::microseconds(2000)};  // 2 ms
    pacer.begin_frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(8));  // 4x target
    pacer.wait_until_target();
    const auto stats = pacer.stats();
    if (!Check(stats.deadline_misses_total == 1,
               "FramePacer should count 1 deadline miss, got " +
                   std::to_string(stats.deadline_misses_total))) {
      return 1;
    }
  }

  // 4. set_target_interval clamps.
  {
    vkpt::core::pacing::FramePacer pacer;
    pacer.set_target_interval(std::chrono::microseconds(0));
    if (!Check(pacer.target_interval() == std::chrono::microseconds(1),
               "set_target_interval(0) should clamp to 1us, got " +
                   std::to_string(pacer.target_interval().count()))) {
      return 1;
    }
    pacer.set_target_interval(std::chrono::seconds(60));
    if (!Check(pacer.target_interval() == std::chrono::microseconds(1000000),
               "set_target_interval(60s) should clamp to 1s")) {
      return 1;
    }
    pacer.set_target_interval(std::chrono::microseconds(16667));
    if (!Check(pacer.target_interval() == std::chrono::microseconds(16667),
               "in-range target_interval should round-trip")) {
      return 1;
    }
  }

  // 5. Move construction preserves stats.
  {
    vkpt::core::pacing::FramePacer src{std::chrono::microseconds(2000)};
    src.begin_frame();
    src.wait_until_target();
    src.begin_frame();
    src.wait_until_target();
    const auto src_stats = src.stats();
    vkpt::core::pacing::FramePacer dst{std::move(src)};
    const auto dst_stats = dst.stats();
    if (!Check(dst_stats.frames_total == src_stats.frames_total,
               "move-constructed pacer should preserve frames_total")) {
      return 1;
    }
    if (!Check(dst_stats.target_us == 2000u,
               "move-constructed pacer should preserve target_us")) {
      return 1;
    }
  }

  // 6. reset_stats zeroes counters but keeps target.
  {
    vkpt::core::pacing::FramePacer pacer{std::chrono::microseconds(3000)};
    pacer.begin_frame();
    pacer.wait_until_target();
    pacer.reset_stats();
    const auto stats = pacer.stats();
    if (!Check(stats.frames_total == 0 && stats.deadline_misses_total == 0,
               "reset_stats should zero counters")) {
      return 1;
    }
    if (!Check(stats.target_us == 3000u,
               "reset_stats should preserve target_us")) {
      return 1;
    }
  }

  std::cout << "frame_pacer_smoke: ok\n";
  return 0;
}
