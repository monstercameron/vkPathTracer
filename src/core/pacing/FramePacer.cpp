#include "core/pacing/FramePacer.h"

#include <algorithm>
#include <atomic>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace vkpt::core::pacing {

namespace {

// Spin margin: how much time we leave between the timer wake-up and the true
// target. The instinct is "longer = more precise" — a 1.5 ms spin absolutely
// hits target down to the microsecond on a quiet harness. But on the Qt UI
// thread the spin steals CPU from Qt's event compositor / paint dispatch
// threads sharing the core, which actually inflates the *next* frame's work
// by 1-2 ms (measured: pace_wait drops 1.8 ms, frame_work rises 1.9 ms — a
// wash). We pick a small margin that handles the case where the timer wakes
// a hair early (rare on HIGH_RESOLUTION_TIMER, possible on the fallback)
// and accept the platform's natural wake jitter as the pacing tail. On
// Windows 11 with the high-res timer this gives ~100-500 us of overshoot
// per frame without contending with Qt internals.
constexpr std::chrono::microseconds kSpinMargin{100};

#ifdef _WIN32
// Per-thread Windows waitable timer. Owned via thread_local so each calling
// thread has its own handle and there's no contention. CreateWaitableTimerExW
// with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is available on Windows 10 1803+
// (Server 2019+); on older builds it falls through to the regular waitable
// timer (which is still better than sleep_for because it uses native NT
// scheduling primitives).
class WinTimer {
 public:
  WinTimer() {
    constexpr DWORD kHighResolutionFlag = 0x00000002;  // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
    m_handle = CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionFlag, TIMER_ALL_ACCESS);
    if (m_handle != nullptr) {
      m_high_resolution = true;
      return;
    }
    // Fallback: regular waitable timer (Windows 7+).
    m_handle = CreateWaitableTimerExW(nullptr, nullptr, 0u, TIMER_ALL_ACCESS);
  }

  ~WinTimer() {
    if (m_handle != nullptr) {
      CloseHandle(m_handle);
    }
  }

  WinTimer(const WinTimer&) = delete;
  WinTimer& operator=(const WinTimer&) = delete;

  HANDLE handle() const noexcept { return m_handle; }
  bool high_resolution() const noexcept { return m_high_resolution; }

 private:
  HANDLE m_handle = nullptr;
  bool m_high_resolution = false;
};

WinTimer& ThreadTimer() {
  thread_local WinTimer t;
  return t;
}
#endif

// Platform-agnostic sleep that lands as close to `target` as the OS allows.
void SleepCoarse(std::chrono::nanoseconds duration) noexcept {
  if (duration.count() <= 0) {
    return;
  }
#ifdef _WIN32
  const HANDLE timer = ThreadTimer().handle();
  if (timer != nullptr) {
    LARGE_INTEGER due;
    // Negative = relative time, in 100ns units.
    due.QuadPart = -static_cast<LONGLONG>(duration.count() / 100);
    if (due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != 0) {
      // INFINITE wait is fine because the timer fires at the requested time.
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
#endif
  std::this_thread::sleep_for(duration);
}

void SpinUntil(std::chrono::steady_clock::time_point target) noexcept {
  while (std::chrono::steady_clock::now() < target) {
#ifdef _WIN32
    YieldProcessor();
#endif
    std::this_thread::yield();
  }
}

}  // namespace

void SleepUntilHighResolution(std::chrono::steady_clock::time_point target) noexcept {
  const auto now = std::chrono::steady_clock::now();
  if (now >= target) {
    return;
  }
  const auto remaining = target - now;
  if (remaining > kSpinMargin) {
    // One coarse sleep, undershooting by kSpinMargin so the spin below
    // absorbs whatever wake jitter the platform timer leaves us with.
    SleepCoarse(remaining - kSpinMargin);
  }
  SpinUntil(target);
}

// ---------------------------------------------------------------------------
// FramePacer
// ---------------------------------------------------------------------------

struct FramePacer::Impl {
  using clock = std::chrono::steady_clock;

  // Target interval. Atomic so set_target_interval can update it from a
  // different thread than wait_until_target.
  std::atomic<std::int64_t> target_us{0};

  // Frame state. begin_frame stores `frame_start`; wait_until_target reads it
  // and the previous frame_start to compute the inter-frame delta. Stored as
  // count() of nanoseconds so we can use atomic loads/stores without a mutex.
  // 0 = no frame in progress.
  std::atomic<std::int64_t> frame_start_ns{0};
  std::atomic<std::int64_t> prev_frame_start_ns{0};

  // Stats counters. Accumulators in nanoseconds; the snapshot divides by
  // frames_total to get averages. Using int64 avoids overflow even at days
  // of accumulated frames.
  std::atomic<std::uint64_t> frames_total{0};
  std::atomic<std::uint64_t> deadline_misses_total{0};
  std::atomic<std::int64_t> sum_frame_ns{0};
  std::atomic<std::int64_t> max_frame_ns{0};
  std::atomic<std::int64_t> sum_pacing_ns{0};
  std::atomic<std::int64_t> max_overshoot_ns{0};

  bool high_resolution_timer = false;

  static std::int64_t to_count(clock::time_point tp) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
  }
  static clock::time_point from_count(std::int64_t count) noexcept {
    return clock::time_point{std::chrono::nanoseconds{count}};
  }

  static void update_max(std::atomic<std::int64_t>& slot, std::int64_t value) noexcept {
    std::int64_t cur = slot.load(std::memory_order_relaxed);
    while (value > cur && !slot.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
      // cur reloaded by compare_exchange_weak on failure; loop until either
      // value <= cur or we win the CAS.
    }
  }
};

FramePacer::FramePacer() : FramePacer(std::chrono::microseconds(16667)) {}

FramePacer::FramePacer(std::chrono::microseconds target_interval)
    : m_impl(std::make_unique<Impl>()) {
  set_target_interval(target_interval);
#ifdef _WIN32
  m_impl->high_resolution_timer = ThreadTimer().high_resolution();
#endif
}

FramePacer::~FramePacer() = default;

FramePacer::FramePacer(FramePacer&& other) noexcept = default;

FramePacer& FramePacer::operator=(FramePacer&& other) noexcept = default;

void FramePacer::set_target_interval(std::chrono::microseconds interval) noexcept {
  if (m_impl == nullptr) {
    return;
  }
  // Clamp to [1us, 1s]. Below 1us isn't reachable on any common timer; above
  // 1s isn't a frame target.
  constexpr auto kMin = std::chrono::microseconds{1};
  constexpr auto kMax = std::chrono::seconds{1};
  const auto clamped = std::clamp(
      interval,
      std::chrono::duration_cast<std::chrono::microseconds>(kMin),
      std::chrono::duration_cast<std::chrono::microseconds>(kMax));
  m_impl->target_us.store(clamped.count(), std::memory_order_release);
}

std::chrono::microseconds FramePacer::target_interval() const noexcept {
  if (m_impl == nullptr) {
    return std::chrono::microseconds{0};
  }
  return std::chrono::microseconds{m_impl->target_us.load(std::memory_order_acquire)};
}

void FramePacer::begin_frame() noexcept {
  if (m_impl == nullptr) {
    return;
  }
  const auto now = Impl::clock::now();
  const auto now_ns = Impl::to_count(now);
  // Roll the previous frame_start forward; if there was one, record the
  // inter-frame duration as a deadline miss because wait_until_target wasn't
  // called.
  const auto prev_start = m_impl->frame_start_ns.exchange(now_ns, std::memory_order_acq_rel);
  if (prev_start != 0) {
    m_impl->deadline_misses_total.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t delta = now_ns - prev_start;
    m_impl->sum_frame_ns.fetch_add(delta, std::memory_order_relaxed);
    Impl::update_max(m_impl->max_frame_ns, delta);
  }
  m_impl->prev_frame_start_ns.store(prev_start, std::memory_order_relaxed);
}

void FramePacer::wait_until_target() noexcept {
  if (m_impl == nullptr) {
    return;
  }
  const auto frame_start_ns = m_impl->frame_start_ns.exchange(0, std::memory_order_acq_rel);
  if (frame_start_ns == 0) {
    // wait without begin_frame: nothing to pace against.
    return;
  }
  const auto target_us = m_impl->target_us.load(std::memory_order_acquire);
  const auto frame_start = Impl::from_count(frame_start_ns);
  const auto target = frame_start + std::chrono::microseconds{target_us};

  const auto now_before_pacing = Impl::clock::now();
  if (now_before_pacing >= target) {
    m_impl->deadline_misses_total.fetch_add(1, std::memory_order_relaxed);
    // Still record the frame interval against the previous frame if any.
    const auto prev_start = m_impl->prev_frame_start_ns.load(std::memory_order_relaxed);
    if (prev_start != 0) {
      const std::int64_t delta = frame_start_ns - prev_start;
      m_impl->sum_frame_ns.fetch_add(delta, std::memory_order_relaxed);
      Impl::update_max(m_impl->max_frame_ns, delta);
    }
    m_impl->frames_total.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  SleepUntilHighResolution(target);
  const auto now_after = Impl::clock::now();
  const std::int64_t pacing_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now_after - now_before_pacing).count();
  const std::int64_t overshoot_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now_after - target).count();
  m_impl->sum_pacing_ns.fetch_add(pacing_ns, std::memory_order_relaxed);
  if (overshoot_ns > 0) {
    Impl::update_max(m_impl->max_overshoot_ns, overshoot_ns);
  }

  // Record the actual frame interval against the previous frame if any.
  const auto prev_start = m_impl->prev_frame_start_ns.load(std::memory_order_relaxed);
  if (prev_start != 0) {
    const std::int64_t delta = frame_start_ns - prev_start;
    m_impl->sum_frame_ns.fetch_add(delta, std::memory_order_relaxed);
    Impl::update_max(m_impl->max_frame_ns, delta);
  }
  m_impl->frames_total.fetch_add(1, std::memory_order_relaxed);
}

FramePacingStats FramePacer::stats() const noexcept {
  FramePacingStats out;
  if (m_impl == nullptr) {
    return out;
  }
  const auto frames = m_impl->frames_total.load(std::memory_order_relaxed);
  out.frames_total = frames;
  out.deadline_misses_total = m_impl->deadline_misses_total.load(std::memory_order_relaxed);
  out.target_us = static_cast<std::uint64_t>(m_impl->target_us.load(std::memory_order_acquire));
  const auto sum_frame_ns = m_impl->sum_frame_ns.load(std::memory_order_relaxed);
  const auto sum_pacing_ns = m_impl->sum_pacing_ns.load(std::memory_order_relaxed);
  const auto max_frame_ns = m_impl->max_frame_ns.load(std::memory_order_relaxed);
  const auto max_overshoot_ns = m_impl->max_overshoot_ns.load(std::memory_order_relaxed);
  if (frames > 0) {
    out.avg_frame_us = static_cast<std::uint64_t>(sum_frame_ns / static_cast<std::int64_t>(frames) / 1000);
    out.avg_pacing_us = static_cast<std::uint64_t>(sum_pacing_ns / static_cast<std::int64_t>(frames) / 1000);
  }
  out.max_frame_us = static_cast<std::uint64_t>(std::max<std::int64_t>(0, max_frame_ns) / 1000);
  out.max_pacing_overshoot_us = static_cast<std::uint64_t>(std::max<std::int64_t>(0, max_overshoot_ns) / 1000);
  out.high_resolution_timer = m_impl->high_resolution_timer;
  return out;
}

void FramePacer::reset_stats() noexcept {
  if (m_impl == nullptr) {
    return;
  }
  m_impl->frames_total.store(0, std::memory_order_relaxed);
  m_impl->deadline_misses_total.store(0, std::memory_order_relaxed);
  m_impl->sum_frame_ns.store(0, std::memory_order_relaxed);
  m_impl->max_frame_ns.store(0, std::memory_order_relaxed);
  m_impl->sum_pacing_ns.store(0, std::memory_order_relaxed);
  m_impl->max_overshoot_ns.store(0, std::memory_order_relaxed);
}

}  // namespace vkpt::core::pacing
