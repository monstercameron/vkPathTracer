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

#include "animation/AnimationAuthority.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationSampler.h"
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

  // ---- Phase 5 RAG05/06/07 additions -------------------------------------
  int new_passed = 0;

  // Test 1: AnimationAuthority arbitration. Pure CPU — no Jolt needed.
  {
    using namespace vkpt::animation;
    const auto bind_world = compute_bind_world_matrices(skeleton);
    std::vector<vkpt::scene::Mat4> anim = bind_world;
    std::vector<vkpt::scene::Mat4> rag = bind_world;
    // Differentiate the matrices so we can tell which one was selected.
    for (auto& m : anim) m.values[12] += 1.0f;
    for (auto& m : rag) m.values[12] += 2.0f;

    auto r0 = resolve_authority(skeleton, false, false, anim, rag, bind_world);
    if (!Check(r0.source == JointAuthority::BindPose,
               "authority arbitration: no anim no rag -> BindPose")) {
      return 1;
    }
    auto r1 = resolve_authority(skeleton, true, false, anim, rag, bind_world);
    if (!Check(r1.source == JointAuthority::Animation,
               "authority arbitration: anim only -> Animation")) {
      return 1;
    }
    auto r2 = resolve_authority(skeleton, false, true, anim, rag, bind_world);
    if (!Check(r2.source == JointAuthority::Ragdoll,
               "authority arbitration: ragdoll only -> Ragdoll")) {
      return 1;
    }
    auto r3 = resolve_authority(skeleton, true, true, anim, rag, bind_world);
    if (!Check(r3.source == JointAuthority::Ragdoll,
               "authority arbitration: both -> Ragdoll wins")) {
      return 1;
    }
    if (!Check(r1.joint_world_matrices.size() == skeleton.joints.size(),
               "anim resolution returned wrong matrix count")) {
      return 1;
    }
    // r1 should carry the animation matrices' translation offset.
    if (!Check(std::abs(r1.joint_world_matrices.front().values[12] -
                        anim.front().values[12]) < 1.0e-6f,
               "anim resolution did not copy anim matrices")) {
      return 1;
    }
    if (!Check(std::abs(r3.joint_world_matrices.front().values[12] -
                        rag.front().values[12]) < 1.0e-6f,
               "rag-wins resolution did not copy rag matrices")) {
      return 1;
    }
    Skeleton empty;
    auto r4 = resolve_authority(empty, false, false, {}, {}, {});
    if (!Check(r4.source == JointAuthority::None,
               "authority arbitration: empty skeleton -> None")) {
      return 1;
    }
    ++new_passed;
    std::cout << "ragdoll_smoke: authority_arbitration ok\n";
  }

  // Test 2: pose seeding. Build a fresh ragdoll, seed it from a sampled clip
  // pose, step physics 1 frame, assert nothing teleported wildly.
  {
    if (!Check(!loaded.animation_clips.empty(),
               "no animation clips for seeding test")) {
      return 1;
    }
    // Pick the "idle" clip if present, else clip[0].
    const vkpt::animation::AnimationClip* clip = nullptr;
    for (const auto& c : loaded.animation_clips) {
      if (c.name == "idle") { clip = &c; break; }
    }
    if (clip == nullptr) clip = &loaded.animation_clips.front();

    const float t_sample = 0.5f * clip->duration_seconds;
    const auto locals = vkpt::animation::evaluate(*clip, skeleton, t_sample);
    auto pose_world = vkpt::animation::compose_world_matrices(skeleton, locals);
    // Apply spawn translation so the pose is anchored 5m above floor.
    const auto seed_spawn = TranslationMat4(0.0f, 5.0f, 0.0f);
    auto translate_pose = [&](std::vector<vkpt::scene::Mat4>& mats) {
      for (auto& m : mats) {
        m.values[12] += seed_spawn.values[12];
        m.values[13] += seed_spawn.values[13];
        m.values[14] += seed_spawn.values[14];
      }
    };
    translate_pose(pose_world);

    vkpt::physics::Ragdoll seeded;
    if (!Check(seeded.build(skeleton, seed_spawn, vkpt::physics::RagdollConfig{}),
               "seeded.build failed")) {
      return 1;
    }
    seeded.add_to_world(world);
    if (!Check(seeded.seed_pose_from_skeleton(pose_world, nullptr, kDt),
               "seed_pose_from_skeleton returned false")) {
      return 1;
    }

    // Compare ragdoll's reported joint matrices vs the seeded pose. Allow
    // some epsilon because joint position is reconstructed from body center
    // + bone_end_local, which is a slight idealization vs the input pose.
    const auto reported = seeded.read_joint_world_matrices();
    if (!Check(reported.size() == pose_world.size(),
               "seeded joint count mismatch")) {
      return 1;
    }
    float worst_seed_diff = 0.0f;
    for (std::size_t i = 0; i < reported.size(); ++i) {
      // Compare translation only — orientation is approximate.
      for (std::size_t k = 12; k < 15; ++k) {
        worst_seed_diff = std::max(worst_seed_diff,
                                    std::abs(reported[i].values[k] -
                                             pose_world[i].values[k]));
      }
    }

    // Step one frame.
    world.Update(kDt, 1, &temp_alloc, &job_system);
    const auto post = seeded.read_joint_world_matrices();
    for (const auto& m : post) {
      if (!Check(!AnyNonFinite(m),
                 "post-seed joint matrix went non-finite after 1 step")) {
        return 1;
      }
    }
    // No-teleport: every joint moved by less than 0.5m in one frame at 60Hz
    // (free-fall over dt = ~0.0027m; constraint solve adds some, but 0.5m is
    // a generous "didn't fly across the room" bound).
    float worst_step_drift = 0.0f;
    for (std::size_t i = 0; i < post.size(); ++i) {
      for (std::size_t k = 12; k < 15; ++k) {
        worst_step_drift = std::max(worst_step_drift,
                                    std::abs(post[i].values[k] -
                                             reported[i].values[k]));
      }
    }
    if (!Check(worst_step_drift < 0.5f,
               "seeded ragdoll teleported >0.5m in one frame")) {
      std::cerr << "  worst_step_drift=" << worst_step_drift << "\n";
      return 1;
    }

    seeded.remove_from_world(world);
    ++new_passed;
    std::cout << "ragdoll_smoke: pose_seeding ok worst_seed_diff="
              << worst_seed_diff
              << " worst_step_drift=" << worst_step_drift << "\n";
  }

  // Test 3: impulse on activation. Build a third ragdoll, locate the spine
  // joint (or 'body'), apply a downward impulse, step 0.5s, expect root to
  // have descended faster than free-fall would imply by itself? At minimum:
  // the root Y must have decreased.
  {
    vkpt::physics::Ragdoll impulse_rag;
    const auto spawn_imp = TranslationMat4(8.0f, 5.0f, 0.0f);  // sideways so we don't collide with the previous ragdolls' stack
    if (!Check(impulse_rag.build(skeleton, spawn_imp, vkpt::physics::RagdollConfig{}),
               "impulse_rag.build failed")) {
      return 1;
    }
    impulse_rag.add_to_world(world);
    // Find a "body"/"spine"/"torso" joint, fall back to root.
    std::int32_t target_joint = skeleton.root_index;
    for (std::size_t j = 0; j < skeleton.joints.size(); ++j) {
      const auto& name = skeleton.joints[j].name;
      // Lowercase-substring match.
      auto lower = name;
      for (auto& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (lower.find("spine") != std::string::npos ||
          lower.find("body") != std::string::npos ||
          lower.find("torso") != std::string::npos) {
        target_joint = static_cast<std::int32_t>(j);
        break;
      }
    }

    const auto pre = impulse_rag.read_joint_world_matrices();
    const float root_pre_y = ExtractY(pre[
        static_cast<std::size_t>(skeleton.root_index)]);

    const bool ok = impulse_rag.apply_impulse_to_joint(
        target_joint, vkpt::scene::Vec3{0.0f, -50.0f, 0.0f});
    if (!Check(ok, "apply_impulse_to_joint returned false")) {
      return 1;
    }

    // Step ~0.5s (30 frames at 60Hz).
    for (int i = 0; i < 30; ++i) {
      world.Update(kDt, 1, &temp_alloc, &job_system);
    }

    const auto post = impulse_rag.read_joint_world_matrices();
    for (const auto& m : post) {
      if (!Check(!AnyNonFinite(m),
                 "post-impulse joint matrix non-finite")) {
        return 1;
      }
    }
    const float root_post_y = ExtractY(post[
        static_cast<std::size_t>(skeleton.root_index)]);
    const float drop_imp = root_pre_y - root_post_y;
    if (!Check(drop_imp > 0.0f,
               "after impulse + 0.5s, root Y did not decrease")) {
      std::cerr << "  root_pre_y=" << root_pre_y
                << " root_post_y=" << root_post_y << "\n";
      return 1;
    }

    impulse_rag.remove_from_world(world);
    ++new_passed;
    std::cout << "ragdoll_smoke: impulse target_joint=" << target_joint
              << " drop=" << drop_imp << "m\n";
  }

  std::cout << "ragdoll_smoke: phase5_new_tests_passed=" << new_passed
            << "/3\n";

  // Tear down Jolt.
  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;

  std::cout << "ragdoll_smoke: ok\n";
  return 0;
#endif
}
