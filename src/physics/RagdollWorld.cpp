#include "physics/RagdollWorld.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/Logging.h"
#include "physics/Ragdoll.h"

#ifdef PT_ENABLE_JOLT
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#endif

namespace vkpt::physics {

#ifdef PT_ENABLE_JOLT
namespace {

// RagdollWorld owns its own Jolt object/broadphase layers — independent
// from the engine's main physics layers, since this Jolt PhysicsSystem
// instance is also independent.
namespace RWLayers {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kDynamic = 1;
constexpr JPH::ObjectLayer kCount = 2;
}  // namespace RWLayers

namespace RWBPLayers {
constexpr JPH::BroadPhaseLayer kStatic(0);
constexpr JPH::BroadPhaseLayer kDynamic(1);
constexpr JPH::uint kCount = 2;
}  // namespace RWBPLayers

class RWBPLayerInterface final : public JPH::BroadPhaseLayerInterface {
 public:
  RWBPLayerInterface() {
    m_layers[RWLayers::kStatic] = RWBPLayers::kStatic;
    m_layers[RWLayers::kDynamic] = RWBPLayers::kDynamic;
  }
  JPH::uint GetNumBroadPhaseLayers() const override {
    return RWBPLayers::kCount;
  }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    return m_layers[layer];
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override {
    return "ragdoll_world";
  }
#endif

 private:
  JPH::BroadPhaseLayer m_layers[RWLayers::kCount];
};

class RWOVBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer obj,
                     JPH::BroadPhaseLayer bp) const override {
    if (obj == RWLayers::kStatic) return bp == RWBPLayers::kDynamic;
    if (obj == RWLayers::kDynamic) return true;
    return false;
  }
};

class RWObjPairFilter final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    if (a == RWLayers::kStatic && b == RWLayers::kStatic) return false;
    return a < RWLayers::kCount && b < RWLayers::kCount;
  }
};

void EnsureJoltInitialized() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    JPH::RegisterDefaultAllocator();
    if (JPH::Factory::sInstance == nullptr) {
      JPH::Factory::sInstance = new JPH::Factory();
    }
    JPH::RegisterTypes();
  });
}

}  // namespace

struct RagdollWorld::Impl {
  Impl() = default;

  // Lazy world initialization: Jolt allocators / Factory / PhysicsSystem
  // boot the first time a ragdoll attaches. Scenes that never attach
  // ragdolls (the common case) avoid the global Jolt init entirely, which
  // is important because the engine's ThreadedPhysicsWorld also owns a
  // separate JoltRuntime — keeping our init lazy avoids any startup-order
  // surprise on Qt boot.
  void ensure_world_ready() {
    if (world_initialized) return;
    EnsureJoltInitialized();
    if (!temp_alloc) {
      temp_alloc = std::make_unique<JPH::TempAllocatorImpl>(8 * 1024 * 1024);
    }
    if (!job_system) {
      job_system = std::make_unique<JPH::JobSystemThreadPool>(
          JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 1);
    }
    jph_world.Init(1024, 0, 1024, 1024, bp_iface, ovbp_filter, obj_filter);
    jph_world.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    // Static floor at y=0.
    auto& bi = jph_world.GetBodyInterface();
    auto box = JPH::BoxShapeSettings(JPH::Vec3(50.0f, 0.5f, 50.0f)).Create();
    if (!box.HasError()) {
      JPH::BodyCreationSettings settings(box.Get(),
                                         JPH::RVec3(0.0f, -0.5f, 0.0f),
                                         JPH::Quat::sIdentity(),
                                         JPH::EMotionType::Static,
                                         RWLayers::kStatic);
      settings.mFriction = 0.7f;
      bi.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
    }
    jph_world.OptimizeBroadPhase();
    world_initialized = true;
    available = true;
  }

  ~Impl() {
    // Per-ragdoll cleanup happens via std::unique_ptr; the world destroys
    // remaining bodies on its own destruction. PhysicsSystem dtor handles
    // that path correctly.
    ragdolls.clear();
  }

  struct Entry {
    vkpt::core::StableEntityId entity = 0u;
    std::unique_ptr<Ragdoll> ragdoll;
    std::size_t joint_count = 0u;
    // Cached snapshot of the latest joint world matrices, updated each
    // step() call. Reads are guarded by `mutex` so the qt loop's
    // read_ragdoll_joint_matrices() call is race-free w.r.t. step().
    std::vector<vkpt::scene::Mat4> snapshot;
  };

  // Storage. Mutex guards cmd path (attach/detach/seed/impulse) AND the
  // step()+snapshot publish path. Reads of the snapshot copy under-lock,
  // so the qt loop's per-frame query is fast and safe.
  mutable std::mutex mutex;
  std::unordered_map<std::uint64_t, Entry> ragdolls;
  std::atomic<std::uint64_t> next_id{1u};
  std::atomic<std::uint64_t> step_counter{0u};

  bool available = false;
  bool world_initialized = false;

  // Owned Jolt resources (lazy).
  std::unique_ptr<JPH::TempAllocatorImpl> temp_alloc;
  std::unique_ptr<JPH::JobSystemThreadPool> job_system;
  RWBPLayerInterface bp_iface;
  RWOVBPFilter ovbp_filter;
  RWObjPairFilter obj_filter;
  JPH::PhysicsSystem jph_world;
};

#else  // PT_ENABLE_JOLT

struct RagdollWorld::Impl {
  bool available = false;
  mutable std::mutex mutex;
  std::unordered_map<std::uint64_t, std::vector<vkpt::scene::Mat4>> snapshots;
  std::atomic<std::uint64_t> next_id{1u};
  std::atomic<std::uint64_t> step_counter{0u};
};

#endif

RagdollWorld::RagdollWorld() : m_impl(std::make_unique<Impl>()) {
#ifdef PT_ENABLE_JOLT
  // Mark available — actual Jolt world creation is lazy (see
  // Impl::ensure_world_ready()).
  m_impl->available = true;
#endif
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info, "ragdoll_world", "constructed",
        {{"available", m_impl->available ? "true" : "false"},
         {"lazy_init", "true"}});
  } catch (...) {
  }
}

RagdollWorld::~RagdollWorld() = default;
RagdollWorld::RagdollWorld(RagdollWorld&&) noexcept = default;
RagdollWorld& RagdollWorld::operator=(RagdollWorld&&) noexcept = default;

bool RagdollWorld::is_available() const noexcept {
  return m_impl != nullptr && m_impl->available;
}

RagdollHandle RagdollWorld::attach_ragdoll(
    vkpt::core::StableEntityId entity,
    const vkpt::animation::Skeleton& skeleton,
    const vkpt::scene::Mat4& spawn_world,
    const RagdollConfig& config) {
#ifdef PT_ENABLE_JOLT
  if (m_impl == nullptr || !m_impl->available) {
    return RagdollHandle{};
  }
  std::scoped_lock lock(m_impl->mutex);
  m_impl->ensure_world_ready();
  if (!m_impl->world_initialized) {
    return RagdollHandle{};
  }
  auto rd = std::make_unique<Ragdoll>();
  if (!rd->build(skeleton, spawn_world, config)) {
    return RagdollHandle{};
  }
  rd->add_to_world(m_impl->jph_world);
  Impl::Entry entry;
  entry.entity = entity;
  entry.joint_count = skeleton.joints.size();
  // Initial snapshot = bind pose so reads before the first step() return
  // a valid frame instead of an empty vector.
  entry.snapshot = rd->read_joint_world_matrices();
  entry.ragdoll = std::move(rd);
  const auto id = m_impl->next_id.fetch_add(1u, std::memory_order_relaxed);
  m_impl->ragdolls.emplace(id, std::move(entry));
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info, "ragdoll_world", "attached",
        {{"handle", std::to_string(id)},
         {"entity", std::to_string(entity)},
         {"joints", std::to_string(skeleton.joints.size())}});
  } catch (...) {
  }
  return RagdollHandle{id};
#else
  (void)entity;
  (void)skeleton;
  (void)spawn_world;
  (void)config;
  return RagdollHandle{};
#endif
}

void RagdollWorld::detach_ragdoll(RagdollHandle handle) {
  if (m_impl == nullptr || !handle.valid()) return;
  std::scoped_lock lock(m_impl->mutex);
#ifdef PT_ENABLE_JOLT
  auto it = m_impl->ragdolls.find(handle.id);
  if (it == m_impl->ragdolls.end()) return;
  if (it->second.ragdoll && it->second.ragdoll->is_added_to_world()) {
    it->second.ragdoll->remove_from_world(m_impl->jph_world);
  }
  m_impl->ragdolls.erase(it);
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info, "ragdoll_world", "detached",
        {{"handle", std::to_string(handle.id)}});
  } catch (...) {
  }
#else
  m_impl->snapshots.erase(handle.id);
#endif
}

std::vector<vkpt::scene::Mat4> RagdollWorld::read_ragdoll_joint_matrices(
    RagdollHandle handle) const {
  if (m_impl == nullptr || !handle.valid()) return {};
  std::scoped_lock lock(m_impl->mutex);
#ifdef PT_ENABLE_JOLT
  auto it = m_impl->ragdolls.find(handle.id);
  if (it == m_impl->ragdolls.end()) return {};
  return it->second.snapshot;  // copy
#else
  auto it = m_impl->snapshots.find(handle.id);
  if (it == m_impl->snapshots.end()) return {};
  return it->second;
#endif
}

bool RagdollWorld::seed_ragdoll_pose(
    RagdollHandle handle,
    const std::vector<vkpt::scene::Mat4>& joint_world_matrices,
    const std::vector<vkpt::scene::Mat4>* prev_joint_world_matrices,
    float dt) {
  if (m_impl == nullptr || !handle.valid()) return false;
#ifdef PT_ENABLE_JOLT
  std::scoped_lock lock(m_impl->mutex);
  auto it = m_impl->ragdolls.find(handle.id);
  if (it == m_impl->ragdolls.end() || !it->second.ragdoll) return false;
  return it->second.ragdoll->seed_pose_from_skeleton(
      joint_world_matrices, prev_joint_world_matrices, dt);
#else
  (void)joint_world_matrices;
  (void)prev_joint_world_matrices;
  (void)dt;
  return false;
#endif
}

bool RagdollWorld::apply_ragdoll_impulse(RagdollHandle handle,
                                         std::int32_t joint_index,
                                         vkpt::scene::Vec3 impulse) {
  if (m_impl == nullptr || !handle.valid()) return false;
#ifdef PT_ENABLE_JOLT
  std::scoped_lock lock(m_impl->mutex);
  auto it = m_impl->ragdolls.find(handle.id);
  if (it == m_impl->ragdolls.end() || !it->second.ragdoll) return false;
  return it->second.ragdoll->apply_impulse_to_joint(joint_index, impulse);
#else
  (void)joint_index;
  (void)impulse;
  return false;
#endif
}

void RagdollWorld::step(float dt) {
  if (m_impl == nullptr) return;
#ifdef PT_ENABLE_JOLT
  std::scoped_lock lock(m_impl->mutex);
  if (!m_impl->available || !m_impl->world_initialized ||
      m_impl->ragdolls.empty()) return;
  if (!std::isfinite(dt) || dt <= 0.0f) return;
  // Cap dt to avoid Jolt assertions on big stalls.
  dt = std::min(dt, 1.0f / 30.0f);
  m_impl->jph_world.Update(dt, 1, m_impl->temp_alloc.get(),
                           m_impl->job_system.get());
  for (auto& [id, entry] : m_impl->ragdolls) {
    if (entry.ragdoll) {
      entry.snapshot = entry.ragdoll->read_joint_world_matrices();
    }
  }
  m_impl->step_counter.fetch_add(1u, std::memory_order_relaxed);
#else
  (void)dt;
#endif
}

std::size_t RagdollWorld::active_ragdoll_count() const noexcept {
  if (m_impl == nullptr) return 0u;
  std::scoped_lock lock(m_impl->mutex);
#ifdef PT_ENABLE_JOLT
  return m_impl->ragdolls.size();
#else
  return m_impl->snapshots.size();
#endif
}

std::size_t RagdollWorld::total_steps() const noexcept {
  if (m_impl == nullptr) return 0u;
  return m_impl->step_counter.load(std::memory_order_relaxed);
}

}  // namespace vkpt::physics
