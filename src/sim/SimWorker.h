#pragma once

// SimWorker — owns the per-frame simulation step (input integration,
// scripts, physics, snapshot publish, UI mirror update) so the Qt main
// loop is reduced to: poll -> input drain -> submit -> acquire latest
// framebuffer -> repaint -> pace.
//
// Thread model:
//   - Construction and destruction happen on the owning (UI) thread.
//   - start() spawns one std::thread that owns the loop. Until start() is
//     called the worker is inert (submit_input still buffers; status()
//     reports Uninitialized -> Ready transitions on start()).
//   - submit_input() is wait-free for the UI thread (MpscRing under the
//     hood). Multiple producers are tolerated for symmetry but only the UI
//     thread is expected to push.
//   - latest_ui_mirror() is a snapshot copy; no shared state escapes.
//   - stop() joins the worker thread. Idempotent.
//
// Step 1 (current): the loop body lives in a private method but isn't run.
// start() is a no-op; submit_input still queues; latest_ui_mirror returns
// the default-constructed mirror. This lets the call sites be wired in
// without behavior changes; subsequent steps move physics, then scripts,
// then snapshot/mirror publication into the spawned worker thread.

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "core/contracts/SubsystemStatus.h"
#include "core/sync/LatestSlot.h"
#include "core/sync/MpscRing.h"
#include "sim/SimChannels.h"

// Forward declarations — keep the worker header light and avoid cycling
// scripting/physics/render headers into UI translation units.
namespace vkpt::scene {
class SceneWorld;
class SnapshotRing;
}  // namespace vkpt::scene
namespace vkpt::physics {
class IPhysicsWorld;
}
namespace vkpt::scripting {
class IScriptRuntime;
}
namespace vkpt::render {
class RenderCoordinator;
}

namespace vkpt::sim {

class SimWorker {
 public:
  struct Deps {
    vkpt::scene::SceneWorld* world = nullptr;
    vkpt::physics::IPhysicsWorld* physics = nullptr;
    vkpt::scripting::IScriptRuntime* scripts = nullptr;
    vkpt::render::RenderCoordinator* render = nullptr;
    vkpt::scene::SnapshotRing* snapshot_ring = nullptr;
    std::chrono::microseconds tick_target = std::chrono::microseconds(16667);
  };

  explicit SimWorker(Deps deps);
  ~SimWorker();

  SimWorker(const SimWorker&) = delete;
  SimWorker& operator=(const SimWorker&) = delete;

  // Spawn the worker thread. No-op if already started or stopped.
  void start();

  // Request stop; joins the worker thread. Idempotent.
  void stop();

  // Enqueue an input frame. Returns true on success; false if the ring
  // dropped the frame (full).
  bool submit_input(const SimInputFrame& frame);

  // Read the latest UI mirror (copy). Returns a default-constructed mirror
  // if nothing has been published yet.
  UiSimMirror latest_ui_mirror() const;

  // Subsystem status (lifecycle + tick counters). Safe to call on any
  // thread.
  vkpt::core::contracts::SubsystemStatus status() const;

  // Diagnostics: count of input frames the UI submitted to the ring.
  std::uint64_t inputs_submitted_total() const noexcept;
  // Diagnostics: count of input frames the worker has consumed.
  std::uint64_t inputs_consumed_total() const noexcept;
  // Diagnostics: count of input frames the ring rejected as full.
  std::uint64_t inputs_dropped_total() const noexcept;

 private:
  void run_loop();  // Body of the worker thread (Step 2+).

  Deps m_deps;

  // Channels.
  vkpt::core::sync::MpscRing<SimInputFrame> m_input_ring;
  mutable vkpt::core::sync::LatestSlot<UiSimMirror> m_ui_mirror;

  // Lifecycle.
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_stop_requested{false};

  // Counters.
  std::atomic<std::uint64_t> m_inputs_submitted{0u};
  std::atomic<std::uint64_t> m_inputs_consumed{0u};
  std::atomic<std::uint64_t> m_ticks_total{0u};
  std::atomic<std::uint64_t> m_errors_total{0u};
  std::atomic<std::uint64_t> m_started_at_ns{0u};
  std::atomic<std::uint64_t> m_last_tick_ns{0u};

  // Cached UI mirror so that latest_ui_mirror() can return a sensible
  // default before the worker has produced one. The LatestSlot::peek path
  // returns nullopt until first publish; this cache holds the last value
  // we observed there to give callers a stable value to read.
  mutable std::mutex m_mirror_cache_mutex;
  mutable UiSimMirror m_mirror_cache{};
};

}  // namespace vkpt::sim
