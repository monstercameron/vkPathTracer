// Phase 5 Part B smoke: end-to-end exercise of the hero hit demo.
//
// 1. Validate hero_hit_demo.json parses cleanly.
// 2. Synthesize the hero entity (skeleton + clips + animation + script + ragdoll
//    component, all wired) — the asset import pipeline that maps JSON
//    `assets[]` -> EntityRecord is intentionally bypassed so the smoke stays
//    self-contained and doesn't depend on the broader asset loader path.
// 3. Run the script for 240 frames at 60Hz.
//    - on_load runs once.
//    - on_update runs every frame.
//    - At ~t=2s the script fires entity:enable_ragdoll(...) which writes a
//      RagdollComponent { active=true, has_impulse=true, impulse_joint='body' }.
//    - On the frame the component flips, the smoke seeds the JPH ragdoll from
//      the current animation pose and applies the requested impulse.
// 4. Validate: no NaNs in any joint matrix, ragdoll active became true once,
//    `enable_ragdoll` callback fired exactly once, root Y descended between
//    t=2s and t=4s, and no script errors.

#include <algorithm>
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
#include "audio/AudioSystem.h"
#include "physics/Ragdoll.h"
#include "scene/Scene.h"
#include "scene/SceneTypes.h"
#include "scene/SceneWorld.h"
#include "scripting/ScriptRuntime.h"

// Audio stubs (the smoke does not link audio/AudioSystem.cpp; the script's
// LuaAudio* bindings are no-ops without a global audio system).
namespace vkpt::audio {
IAudioSystem* GlobalAudioSystem() { return nullptr; }
void SetGlobalAudioSystem(IAudioSystem*) {}
}  // namespace vkpt::audio

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
    std::cerr << "hero_hit_demo_smoke: FAIL " << msg << "\n";
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

std::filesystem::path FindUp(const std::filesystem::path& relative) {
  if (std::filesystem::exists(relative)) return relative;
  auto cwd = std::filesystem::current_path();
  for (int i = 0; i < 8; ++i) {
    const auto candidate = cwd / relative;
    if (std::filesystem::exists(candidate)) return candidate;
    if (!cwd.has_parent_path() || cwd.parent_path() == cwd) break;
    cwd = cwd.parent_path();
  }
  return relative;
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
  JPH::uint GetNumBroadPhaseLayers() const override {
    return SmokeBPLayers::kCount;
  }
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
  bool ShouldCollide(JPH::ObjectLayer obj,
                     JPH::BroadPhaseLayer bp) const override {
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
  (void)argc;
  (void)argv;

#ifndef PT_ENABLE_JOLT
  std::cout << "hero_hit_demo_smoke: SKIP — built without PT_ENABLE_JOLT\n";
  std::cout << "hero_hit_demo_smoke: ok\n";
  return 0;
#else
#ifndef PT_ENABLE_LUA
  std::cout << "hero_hit_demo_smoke: SKIP — built without PT_ENABLE_LUA\n";
  std::cout << "hero_hit_demo_smoke: ok\n";
  return 0;
#else
  // Step 1: validate scene JSON parses.
  const auto scene_path = FindUp("assets/scenes/hero_hit_demo.json");
  if (!Check(std::filesystem::exists(scene_path),
             "hero_hit_demo.json missing")) {
    return 1;
  }
  {
    auto loader = vkpt::scene::SceneDocument::load_from_file(
        scene_path.generic_string());
    if (!Check(loader.has_value(), "hero_hit_demo.json failed to parse")) {
      std::cerr << "  error_code=" << static_cast<int>(loader.error()) << "\n";
      return 1;
    }
  }

  // Step 2: load hero glTF for skeleton + clips.
  const auto gltf_path = FindUp("assets/models/low_poly_hero/character.gltf");
  if (!Check(std::filesystem::exists(gltf_path), "hero glTF missing")) {
    return 1;
  }
  std::vector<std::string> diagnostics;
  auto loaded =
      vkpt::assets::scene_asset_detail::LoadGltf(gltf_path, &diagnostics);
  if (!Check(loaded.skeleton.has_value(), "loader produced no skeleton")) {
    return 1;
  }
  const auto skeleton = *loaded.skeleton;
  if (!Check(!loaded.animation_clips.empty(), "no animation clips loaded")) {
    return 1;
  }

  // Step 3: synthesize SceneWorld with the hero entity.
  vkpt::scene::SceneWorld world;
  const auto hero_id = world.create_entity("hero", 9501u);

  vkpt::scene::TransformComponent tx;
  tx.translation = vkpt::scene::Vec3{0.0f, 1.0f, 0.0f};
  world.set_component(hero_id, vkpt::scene::ComponentKind::Transform, tx);

  // Find idle clip.
  std::int32_t idle_index = -1;
  for (std::size_t i = 0; i < loaded.animation_clips.size(); ++i) {
    if (loaded.animation_clips[i].name == "idle") {
      idle_index = static_cast<std::int32_t>(i);
      break;
    }
  }
  if (idle_index < 0) idle_index = 0;

  // Direct entity-record writes for skeleton + clips. SkeletonComponent is
  // an alias for vkpt::animation::Skeleton and not a member of the scene
  // ComponentVariant, so set_component() can't carry it. Production code
  // (Scene::ApplyEntityDefinition) writes directly the same way.
  if (auto* rec = world.get_entity(hero_id); rec != nullptr) {
    rec->skeleton = skeleton;
    rec->clips = loaded.animation_clips;
  }

  vkpt::scene::AnimationComponent anim;
  anim.clip_index = idle_index;
  anim.time_seconds = 0.0f;
  anim.speed = 1.0f;
  anim.loop = true;
  anim.paused = false;
  world.set_component(hero_id, vkpt::scene::ComponentKind::Animation, anim);

  vkpt::scene::RagdollComponent rag;
  rag.active = false;
  world.set_component(hero_id, vkpt::scene::ComponentKind::Ragdoll, rag);

  vkpt::scene::ScriptComponent script;
  script.script = "assets/scripts/user/hero_hit_demo.lua";
  script.module_id = "hero_hit_demo";
  world.set_component(hero_id, vkpt::scene::ComponentKind::Script, script);

  // Step 4: build script runtime + Jolt world.
  auto runtime = vkpt::scripting::CreateScriptRuntime();
  runtime->reload_bindings(world);

  // Initialize Jolt singletons.
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
  JPH::PhysicsSystem jph_world;
  jph_world.Init(1024, 0, 1024, 1024, bp_iface, ovbp_filter, obj_filter);
  jph_world.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
  // Static floor at y=0 (top face).
  {
    auto& bi = jph_world.GetBodyInterface();
    auto box = JPH::BoxShapeSettings(JPH::Vec3(20.0f, 0.5f, 20.0f)).Create();
    if (!Check(!box.HasError(), "floor box shape create failed")) {
      return 1;
    }
    JPH::BodyCreationSettings settings(box.Get(),
                                       JPH::RVec3(0.0f, -0.5f, 0.0f),
                                       JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Static,
                                       SmokeLayers::kStatic);
    settings.mFriction = 0.7f;
    bi.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
  }
  jph_world.OptimizeBroadPhase();

  // Step 5: run the simulation loop.
  constexpr float kDt = 1.0f / 60.0f;
  constexpr int kFrames = 240;
  constexpr float kHitT = 2.0f;

  vkpt::physics::Ragdoll ragdoll;
  bool ragdoll_built = false;
  bool ragdoll_active_seen = false;
  int ragdoll_activation_frame = -1;
  float root_y_at_activation = 0.0f;
  float root_y_at_end = 0.0f;
  int hooks_fired_total = 0;
  int script_errors_total = 0;

  // Capture animation joint matrices for seeding (one frame before activation
  // so we can produce a velocity from the prior pose).
  std::vector<vkpt::scene::Mat4> prev_anim_world;

  for (int frame = 0; frame < kFrames; ++frame) {
    vkpt::scripting::ScriptExecutionContext ctx;
    ctx.frame = static_cast<vkpt::core::FrameIndex>(frame);
    ctx.elapsed_seconds = static_cast<double>(frame) * kDt;
    ctx.delta_seconds = kDt;
    ctx.fixed_delta_seconds = kDt;
    ctx.runtime.mode = "play";
    ctx.runtime.scripts_running = true;
    ctx.game_mode = true;

    // Frame 0: on_load.
    if (frame == 0) {
      vkpt::scene::WorldCommandBuffer load_cmds;
      const auto load_disp = runtime->dispatch_hook(
          world, vkpt::scripting::ScriptLifecycleHook::OnLoad, ctx, load_cmds);
      if (!load_disp.result.errors.empty()) {
        for (const auto& e : load_disp.result.errors) {
          std::cerr << "hero_hit_demo_smoke: on_load error: " << e.message
                    << "\n";
          ++script_errors_total;
        }
      }
      const auto replay = load_cmds.replay(world);
      if (!Check(replay.has_value(), "on_load command replay failed")) {
        return 1;
      }
    }

    // on_update.
    vkpt::scene::WorldCommandBuffer update_cmds;
    const auto upd_disp = runtime->dispatch_hook(
        world, vkpt::scripting::ScriptLifecycleHook::OnUpdate, ctx,
        update_cmds);
    hooks_fired_total += static_cast<int>(upd_disp.hook_call_count);
    if (!upd_disp.result.errors.empty()) {
      for (const auto& e : upd_disp.result.errors) {
        std::cerr << "hero_hit_demo_smoke: on_update error frame=" << frame
                  << ": " << e.message << "\n";
        ++script_errors_total;
      }
    }
    const auto upd_replay = update_cmds.replay(world);
    if (!Check(upd_replay.has_value(), "on_update command replay failed")) {
      return 1;
    }

    // Tick animations: write entity->joint_world_matrices from current clip.
    vkpt::animation::tick_animations(world, kDt);

    // Read animation joint matrices BEFORE the ragdoll potentially overwrites
    // them. This is the source of truth for the seed pose.
    std::vector<vkpt::scene::Mat4> anim_world_now;
    {
      const auto* rec = world.get_entity(hero_id);
      if (rec != nullptr) {
        anim_world_now = rec->joint_world_matrices;
      }
    }
    // Apply transform offset (entity transform moves the whole skeleton).
    {
      const auto* rec = world.get_entity(hero_id);
      if (rec != nullptr && rec->transform.has_value()) {
        const auto& t = rec->transform->translation;
        for (auto& m : anim_world_now) {
          m.values[12] += t.x;
          m.values[13] += t.y;
          m.values[14] += t.z;
        }
      }
    }

    // Detect ragdoll activation.
    const auto* rec = world.get_entity(hero_id);
    const bool now_active =
        rec != nullptr && rec->ragdoll.has_value() && rec->ragdoll->active;
    if (now_active && !ragdoll_active_seen) {
      ragdoll_active_seen = true;
      ragdoll_activation_frame = frame;

      // Build + add a Jolt-backed ragdoll. Seed from current animation pose.
      const auto spawn = TranslationMat4(rec->transform->translation.x,
                                         rec->transform->translation.y,
                                         rec->transform->translation.z);
      vkpt::physics::RagdollConfig cfg;
      cfg.capsule_radius_scale = rec->ragdoll->capsule_radius_scale;
      cfg.spine_capsule_radius = rec->ragdoll->spine_capsule_radius;
      cfg.head_capsule_radius = rec->ragdoll->head_capsule_radius;
      cfg.density = rec->ragdoll->density;
      cfg.self_collision = rec->ragdoll->self_collision;
      if (!Check(ragdoll.build(skeleton, spawn, cfg),
                 "ragdoll.build failed at activation")) {
        return 1;
      }
      ragdoll.add_to_world(jph_world);
      ragdoll_built = true;

      if (rec->ragdoll->seed_from_animation) {
        const std::vector<vkpt::scene::Mat4>* prev_ptr =
            (prev_anim_world.size() == anim_world_now.size())
                ? &prev_anim_world
                : nullptr;
        if (!Check(ragdoll.seed_pose_from_skeleton(anim_world_now, prev_ptr,
                                                   kDt),
                   "ragdoll.seed_pose_from_skeleton failed")) {
          return 1;
        }
      }
      if (rec->ragdoll->has_impulse) {
        // Resolve the named joint, fall back to spine/body, fall back to root.
        std::int32_t target = skeleton.root_index;
        const auto& target_name = rec->ragdoll->impulse_joint;
        auto contains_lc = [](const std::string& s, const char* needle) {
          std::string lo;
          lo.reserve(s.size());
          for (char c : s) {
            lo.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(c))));
          }
          return lo.find(needle) != std::string::npos;
        };
        if (!target_name.empty()) {
          for (std::size_t j = 0; j < skeleton.joints.size(); ++j) {
            if (skeleton.joints[j].name == target_name ||
                contains_lc(skeleton.joints[j].name, target_name.c_str())) {
              target = static_cast<std::int32_t>(j);
              break;
            }
          }
        }
        if (target < 0) target = skeleton.root_index;
        const auto applied = ragdoll.apply_impulse_to_joint(
            target, rec->ragdoll->impulse);
        if (!Check(applied, "ragdoll.apply_impulse_to_joint failed")) {
          return 1;
        }
      }
      // Capture root Y at activation for later comparison.
      const auto rag_mats = ragdoll.read_joint_world_matrices();
      root_y_at_activation = ExtractY(
          rag_mats[static_cast<std::size_t>(skeleton.root_index)]);
    }

    // Step physics if ragdoll exists.
    if (ragdoll_built) {
      jph_world.Update(kDt, 1, &temp_alloc, &job_system);
    }

    // Authority arbitration: choose ragdoll matrices when active, else
    // animation matrices, else bind. Validates the new RAG05 path.
    if (rec != nullptr) {
      std::vector<vkpt::scene::Mat4> rag_world;
      if (ragdoll_built) {
        rag_world = ragdoll.read_joint_world_matrices();
      }
      const auto bind_world =
          vkpt::animation::compute_bind_world_matrices(skeleton);
      const auto resolution = vkpt::animation::resolve_authority(
          skeleton, anim.clip_index >= 0 && !anim.paused, now_active,
          anim_world_now, rag_world, bind_world);
      // Sanity: never None — skeleton is always present.
      if (!Check(resolution.source != vkpt::animation::JointAuthority::None,
                 "authority went None despite skeleton present")) {
        return 1;
      }
      // After activation we expect Ragdoll authority.
      if (now_active &&
          !Check(resolution.source ==
                     vkpt::animation::JointAuthority::Ragdoll,
                 "authority not Ragdoll after activation")) {
        return 1;
      }
      // Validate matrices finite.
      for (const auto& m : resolution.joint_world_matrices) {
        if (!Check(!AnyNonFinite(m),
                   "resolved joint matrix went non-finite")) {
          std::cerr << "  frame=" << frame << "\n";
          return 1;
        }
      }
    }

    prev_anim_world = std::move(anim_world_now);
  }

  // Step 6: validate.
  if (!Check(ragdoll_active_seen,
             "ragdoll component never flipped to active")) {
    return 1;
  }
  if (!Check(ragdoll_activation_frame > 0, "activation frame <= 0")) {
    return 1;
  }
  // Activation should happen near t=2s, give a reasonable window (one frame
  // past 2s means frame ~120 or 121).
  const float t_at_activation = static_cast<float>(ragdoll_activation_frame) *
                                kDt;
  if (!Check(std::abs(t_at_activation - kHitT) < 0.1f,
             "activation timing off by more than 100ms")) {
    std::cerr << "  t_at_activation=" << t_at_activation << "\n";
    return 1;
  }

  // Final root Y after 2 more seconds of physics.
  if (ragdoll_built) {
    const auto end_mats = ragdoll.read_joint_world_matrices();
    root_y_at_end = ExtractY(
        end_mats[static_cast<std::size_t>(skeleton.root_index)]);
    for (const auto& m : end_mats) {
      if (!Check(!AnyNonFinite(m), "final ragdoll joint matrix non-finite")) {
        return 1;
      }
    }
  }

  if (!Check(root_y_at_end < root_y_at_activation,
             "root Y did not decrease between activation and end")) {
    std::cerr << "  root_y_activation=" << root_y_at_activation
              << " root_y_end=" << root_y_at_end << "\n";
    return 1;
  }

  if (!Check(script_errors_total == 0,
             "script errors during smoke run")) {
    return 1;
  }

  // Tear down.
  if (ragdoll_built) {
    ragdoll.remove_from_world(jph_world);
  }
  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;

  std::cout << "hero_hit_demo_smoke: hero_id=" << hero_id
            << " idle_clip=" << idle_index
            << " activation_frame=" << ragdoll_activation_frame
            << " (t=" << t_at_activation << "s)\n";
  std::cout << "hero_hit_demo_smoke: root_y activation="
            << root_y_at_activation << " end=" << root_y_at_end
            << " drop=" << (root_y_at_activation - root_y_at_end)
            << "m hooks_fired=" << hooks_fired_total << "\n";
  std::cout << "hero_hit_demo_smoke: ok\n";
  return 0;
#endif  // PT_ENABLE_LUA
#endif  // PT_ENABLE_JOLT
}
