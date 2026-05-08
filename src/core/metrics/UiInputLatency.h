#pragma once

#include <cstdint>

// Input-to-pixel latency correlation. Phase 7.3 of SYSTEM.md calls
// `vkp.ui.input_to_pixel_us` the headline UX metric: end-to-end latency from a
// user input event to the first frame that reflects it. Wiring it cleanly
// requires three call sites coordinating across the Qt input thread, the
// snapshot publish lambda, and the present path:
//
//   1. RecordInputEvent(now_ns) — every Qt input handler stamps the most
//      recent input timestamp.
//   2. ConsumePendingInputForPublish() — the publish lambda atomically takes
//      the timestamp (clearing it) so only the *next* publish after the input
//      is attributed.
//   3. RegisterPublishInput(generation, input_ns) — stash (gen, t_input) in a
//      bounded ring keyed by snapshot generation.
//   4. ResolveInputNsForGeneration(generation) — record_frame_presented looks
//      up the input timestamp for the presented snapshot's generation. Returns
//      0 if the generation isn't tracked (e.g. publish without prior input).
//
// All operations are wait-free. The ring is small and bounded; old entries
// are overwritten without notice.

namespace vkpt::core::metrics {

// Seconds since steady_clock epoch in nanoseconds. Use the same clock for
// input stamping and present recording so the delta is meaningful.
std::uint64_t UiInputLatencyNowNs() noexcept;

// Stamp this input event. Multiple inputs between two publishes coalesce —
// only the most recent timestamp survives.
void RecordInputEvent(std::uint64_t input_ns) noexcept;

// Atomically take and clear the pending input timestamp. Returns 0 if no
// input has occurred since the last consume. Called once per snapshot
// publish, regardless of whether the publish was input-driven.
std::uint64_t ConsumePendingInputForPublish() noexcept;

// Associate a snapshot generation with the input that triggered it. No-op
// when input_ns is 0. The ring holds the last 256 entries.
void RegisterPublishInput(std::uint64_t generation, std::uint64_t input_ns) noexcept;

// Look up the input timestamp registered for `generation`. Returns 0 if the
// generation was never registered (or has been evicted from the ring).
std::uint64_t ResolveInputNsForGeneration(std::uint64_t generation) noexcept;

}  // namespace vkpt::core::metrics
