// Phase 2 RAG01-04 smoke: build a Jolt-backed ragdoll from the hero skeleton,
// step a physics world for 2 simulated seconds, and verify the root joint
// fell at least 2m and every joint matrix stayed finite throughout.
//
// The smoke is self-contained: it does NOT exercise the engine's threaded
// PhysicsWorld pipeline. Instead it instantiates JPH::PhysicsSystem directly,
// adds a static floor + the ragdoll, and steps. This isolates the Phase 2
// bridge from the higher-level scene/sim integration that is incrementally
// rolling in.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "animation/Skeleton.h"
#include "assets/SceneAssetLoaderInternal.h"
#include "physics/Ragdoll.h"
#include "scene/SceneTypes.h"
#include "scene/SceneWorld.h"

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

namespace {

bool Check(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "ragdoll_smoke: FAIL " << msg << "\n";
  }
  return cond;
}

vkpt::scene::Mat4 IdentityMat4() {
  vkpt::scene::Mat4 m{};
  m.values[0] = 1.0f;
  m.values[5] = 1.0f;
  m.values[10] = 1.0f;
  m.values[15] = 1.0f;
  return m;
}

vkpt::scene::Mat4 TranslationMat4(float x, float y, float z) {
  auto m = IdentityMat4();
  m.values[12] = x;
  m.values[13] = y;
  m.values[14] = z;
  return m;
}

float ExtractY(const vkpt::scene::Mat4& m) { return m.values[13]; }

bool AnyNonFinite(const vkpt::scene::Mat4& m) {
  for (float v : m.values) {
    if (!std::isfinite(v)) return true;
  }
  return false;
}

#ifdef PT_ENABLE_JOLT

namespace SmokeLayers {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kDynamic = 1;
constexpr JPH::ObjectLayer kCount = 2;
}  // namespace SmokeLayers

namespace SmokeBPLayers {
constexpr JPH::BroadPhaseLayer kStatic(0);
constexpr JPH::BroadPhaseLayer kDynamic(1);
constexpr JPH::uint kCount = 2;
}  // namespace SmokeBPLayers

class SmokeBPLayerInterface final : public JPH::BroadPhaseLayerInterface {
 public:
  SmokeBPLayerInterface() {
    m_layers[SmokeLayers::kStatic] = SmokeBPLayers::kStatic;
    m_layers[SmokeLayers::kDynamic] = SmokeBPLayers::kDynamic;
  }
  JPH::uint GetNumBroadPhaseLayers() const override { return SmokeBPLayers::kCount; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    return m_layers[layer];
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override {
    return "smoke";
  }
#endif

 private:
  JPH::BroadPhaseLayer m_layers[SmokeLayers::kCount];
};

class SmokeOVBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer obj, JPH::BroadPhaseLayer bp) const override {
    if (obj == SmokeLayers::kStatic) return bp == SmokeBPLayers::kDynamic;
    if (obj == SmokeLayers::kDynamic) return true;
    return false;
  }
};

class SmokeObjPairFilter final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    if (a == SmokeLayers::kStatic && b == SmokeLayers::kStatic) return false;
    return a < SmokeLayers::kCount && b < SmokeLayers::kCount;
  }
};

#endif  // PT_ENABLE_JOLT

}  // namespace

int main(int argc, char** argv) {
#ifndef PT_ENABLE_JOLT
  (void)argc;
  (void)argv;
  std::cout << "ragdoll_smoke: SKIP — built without PT_ENABLE_JOLT; "
               "Jolt-backed ragdoll bridge requires the Jolt vendor target.\n";
  std::cout << "ragdoll_smoke: ok\n";
  return 0;
#else
  std::filesystem::path gltf_path =
      "assets/models/low_poly_hero/character.gltf";
  if (argc > 1) {
    gltf_path = argv[1];
  }
  if (!std::filesystem::exists(gltf_path)) {
    auto cwd = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
      const auto candidate = cwd / "assets/models/low_poly_hero/character.gltf";
      if (std::filesystem::exists(candidate)) {
        gltf_path = candidate;
        break;
      }
      if (!cwd.has_parent_path() || cwd.parent_path() == cwd) {
        break;
      }
      cwd = cwd.parent_path();
    }
  }
  if (!Check(std::filesystem::exists(gltf_path), "hero glTF missing")) {
    return 1;
  }

  std::vector<std::string> diagnostics;
  auto loaded = vkpt::assets::scene_asset_detail::LoadGltf(gltf_path, &diagnostics);
  for (const auto& d : diagnostics) {
    std::cerr << "ragdoll_smoke[diag]: " << d << "\n";
  }
  if (!Check(loaded.skeleton.has_value(), "loader produced no skeleton")) {
    return 1;
  }
  const auto& skeleton = *loaded.skeleton;
  if (!Check(skeleton.joints.size() == 14u, "expected 14 hero joints")) {
    std::cerr << "  got " << skeleton.joints.size() << "\n";
    return 1;
  }

  // Initialize Jolt runtime (factory + register types). The engine's
  // PhysicsWorld owns the canonical singleton, but we have no PhysicsWorld
  // here — duplicate the init.
  JPH::RegisterDefaultAllocator();
  if (JPH::Factory::sInstance == nullptr) {
    JPH::Factory::sInstance = new JPH::Factory();
  }
  JPH::RegisterTypes();

  JPH::TempAllocatorImpl temp_alloc(8 * 1024 * 1024);
  JPH::JobSystemThreadPool job_system(JPH::cMaxPhysicsJobs,
                                      JPH::cMaxPhysicsBarriers, 1);

  SmokeBPLayerInterface bp_iface;
  SmokeOVBPFilter ovbp_filter;
  SmokeObjPairFilter obj_filter;
  JPH::PhysicsSystem world;
  world.Init(1024, 0, 1024, 1024, bp_iface, ovbp_filter, obj_filter);
  world.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

  // Floor: large static box, top face at y=0.
  {
    auto& bi = world.GetBodyInterface();
    auto box = JPH::BoxShapeSettings(JPH::Vec3(20.0f, 0.5f, 20.0f)).Create();
    if (!Check(!box.HasError(), "floor box shape create failed")) {
      return 1;
    }
    JPH::BodyCreationSettings settings(box.Get(), JPH::RVec3(0.0f, -0.5f, 0.0f),
                                       JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Static,
                                       SmokeLayers::kStatic);
    settings.mFriction = 0.7f;
    bi.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
  }

  // Spawn the hero 5m above the floor. The hero's skeleton is authored at
  // bind pose centered at origin; multiply by an upward translation.
  vkpt::physics::Ragdoll ragdoll;
  vkpt::physics::RagdollConfig cfg;  // defaults: self_collision=false
  const auto spawn = TranslationMat4(0.0f, 5.0f, 0.0f);
  if (!Check(ragdoll.build(skeleton, spawn, cfg), "ragdoll build failed")) {
    return 1;
  }
  if (!Check(ragdoll.is_built(), "is_built() false after build")) {
    return 1;
  }
  // The hero has 14 joints with 1 root, so 13 non-root joints become bones.
  if (!Check(ragdoll.body_count() >= 12u && ragdoll.body_count() <= 14u,
             "body_count() outside expected range (~13)")) {
    std::cerr << "  body_count=" << ragdoll.body_count() << "\n";
    return 1;
  }

  ragdoll.add_to_world(world);
  if (!Check(ragdoll.is_added_to_world(), "is_added_to_world() false")) {
    return 1;
  }

  // Constraint count should be (bodies - bones-attached-to-skeleton-root).
  // Bones whose parent_bone_index == -1 (i.e. the root spine bone) have no
  // parent body, so no constraint. Others get one constraint each.
  if (!Check(ragdoll.constraint_count() >= 1u, "no constraints created")) {
    std::cerr << "  constraint_count=" << ragdoll.constraint_count() << "\n";
    return 1;
  }

  world.OptimizeBroadPhase();

  // Initial joint matrices.
  const auto initial = ragdoll.read_joint_world_matrices();
  if (!Check(initial.size() == skeleton.joints.size(),
             "initial read_joint_world_matrices size mismatch")) {
    return 1;
  }
  for (const auto& m : initial) {
    if (!Check(!AnyNonFinite(m), "initial joint matrix non-finite")) {
      return 1;
    }
  }
  const float root_y0 = ExtractY(initial[static_cast<std::size_t>(skeleton.root_index)]);

  // Step 2 simulated seconds at 60Hz = 120 substeps.
  constexpr float kDt = 1.0f / 60.0f;
  constexpr int kSteps = 120;
  for (int i = 0; i < kSteps; ++i) {
    world.Update(kDt, 1, &temp_alloc, &job_system);
    const auto m = ragdoll.read_joint_world_matrices();
    for (const auto& mat : m) {
      if (AnyNonFinite(mat)) {
        std::cerr << "ragdoll_smoke: joint matrix went non-finite at step " << i
                  << "\n";
        return 1;
      }
    }
  }

  const auto final_mats = ragdoll.read_joint_world_matrices();
  const float root_yN = ExtractY(final_mats[static_cast<std::size_t>(skeleton.root_index)]);
  const float drop = root_y0 - root_yN;

  if (!Check(drop > 2.0f, "root joint did not fall >2m under gravity")) {
    std::cerr << "  root_y0=" << root_y0 << " root_yN=" << root_yN
              << " drop=" << drop << "\n";
    return 1;
  }

  for (const auto& m : final_mats) {
    if (!Check(!AnyNonFinite(m), "final joint matrix non-finite")) {
      return 1;
    }
  }

  std::cout << "ragdoll_smoke: skeleton joints=" << skeleton.joints.size()
            << " ragdoll bodies=" << ragdoll.body_count()
            << " constraints=" << ragdoll.constraint_count() << "\n";
  std::cout << "ragdoll_smoke: root y0=" << root_y0 << " yN=" << root_yN
            << " drop=" << drop << " m\n";

  ragdoll.remove_from_world(world);

  // Tear down Jolt.
  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;

  std::cout << "ragdoll_smoke: ok\n";
  return 0;
#endif
}
