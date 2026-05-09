#pragma once

// Real-time frame pacing primitives.
//
// Why a dedicated module: std::this_thread::sleep_for on Windows wakes
// 0.5-1.5 ms late even with timeBeginPeriod(1) active. Compounded across a
// pace loop that runs every frame, this pushes a 60 Hz target to ~50 Hz on
// idle ticks. The fix is the platform-specific high-precision timer:
// CreateWaitableTimerExW with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION on
// Windows 10 1803+, futex-based wakeups on Linux. Both can sleep within
// ~50 us of a target on a typical desktop.
//
// State machine for FramePacer:
//
//   state\method      begin_frame  wait_until_target  set_target  stats  reset
//   Constructed       ->InFrame    noop (deadline_miss++)  ok       ok     ok
//   InFrame           ->InFrame    ->Constructed     ok          ok     ok
//
// (begin_frame called twice in a row records the first as a miss and
// restarts; this is intentional for the case where the caller bails out of
// a frame early without waiting.)

#include <chrono>
#include <cstdint>
#include <memory>

namespace vkpt::core::pacing {

// Sleep until the given target steady_clock time using the platform's most
// precise sleep primitive. Always non-blocking once the target is reached.
//
// Implementation:
// - Windows: SetWaitableTimer on a HIGH_RESOLUTION_TIMER handle for the bulk
//   of the wait, then a sub-ms YieldProcessor/yield spin to land on target.
// - Other platforms: std::this_thread::sleep_for + spin (kernel timer
//   resolution typically already permits ~100us precision).
//
// Drop-in replacement for naive sleep loops. Does NOT process external event
// queues (Qt, message pumps); the caller must drain those themselves before
// calling. This function exists exactly so the caller can do that draining
// once at the top of the frame instead of inside the pace loop.
//
// Thread-safe (each call uses thread-local timer state).
void SleepUntilHighResolution(std::chrono::steady_clock::time_point target) noexcept;

// Snapshot of FramePacer counters. All values are accumulated since
// construction or the last reset_stats() call. All fields are POD; the
// struct can be safely copied across threads or logged as JSON.
struct FramePacingStats {
  // Total frames begin_frame() has been called for.
  std::uint64_t frames_total = 0;
  // Frames where wait_until_target() observed steady_clock::now() >= target
  // before pacing — i.e., the work between begin_frame and wait_until_target
  // already exceeded the target interval.
  std::uint64_t deadline_misses_total = 0;
  // Current target interval in microseconds (live; reflects set_target_interval).
  std::uint64_t target_us = 0;
  // Average actual frame interval (begin_frame to begin_frame) since reset.
  std::uint64_t avg_frame_us = 0;
  // Worst-case frame interval since reset.
  std::uint64_t max_frame_us = 0;
  // Average time wait_until_target() actually slept since reset.
  std::uint64_t avg_pacing_us = 0;
  // Worst overshoot past target across all wait_until_target() calls (always
  // 0 if the platform timer is precise; non-zero indicates the spin phase
  // didn't catch the wake jitter and the frame was late).
  std::uint64_t max_pacing_overshoot_us = 0;
  // True when the platform-specific high-precision timer is in use. False
  // means the fallback sleep_for + spin path is active. Same effective API
  // either way, but stats will show wider jitter on the fallback.
  bool high_resolution_timer = false;
};

class FramePacer {
 public:
  // Default target: 60 Hz.
  FramePacer();
  // Target interval is clamped to [1us, 1s]. Smaller is below platform
  // precision; larger is too coarse to be a useful frame target.
  explicit FramePacer(std::chrono::microseconds target_interval);
  ~FramePacer();

  // Owns OS-level handle on Windows; non-copyable, move-only.
  FramePacer(const FramePacer&) = delete;
  FramePacer& operator=(const FramePacer&) = delete;
  FramePacer(FramePacer&& other) noexcept;
  FramePacer& operator=(FramePacer&& other) noexcept;

  void set_target_interval(std::chrono::microseconds interval) noexcept;
  std::chrono::microseconds target_interval() const noexcept;

  // Mark the start of a new frame. Lightweight — single steady_clock::now()
  // store + counter increment. Safe to call from a different thread than
  // wait_until_target() as long as the FramePacer object outlives both.
  void begin_frame() noexcept;

  // Sleep until last begin_frame_time + target_interval(). If the target has
  // already passed, returns immediately and bumps deadline_misses_total.
  // After return, the pacer is back in the "Constructed" state — the next
  // begin_frame starts a fresh frame.
  void wait_until_target() noexcept;

  FramePacingStats stats() const noexcept;
  void reset_stats() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace vkpt::core::pacing
