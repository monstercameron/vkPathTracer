#include "physics/Ragdoll.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#ifdef PT_ENABLE_JOLT
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#endif

namespace vkpt::physics {

namespace {

// PhysicsWorld owns the canonical layer constants; we mirror the dynamic
// layer here (1) without including its translation unit's internal types.
constexpr std::uint16_t kRagdollDynamicLayer = 1;

std::string LowerCopy(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) {
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

bool NameContains(std::string_view name, std::string_view needle) {
  const auto lo = LowerCopy(name);
  return lo.find(needle) != std::string::npos;
}

enum class ConstraintKind {
  Fixed,
  Hinge,
  SwingTwist,
};

ConstraintKind ChooseConstraintKind(std::string_view joint_name) {
  if (NameContains(joint_name, "shoulder") ||
      NameContains(joint_name, "thigh") ||
      NameContains(joint_name, "hip")) {
    return ConstraintKind::SwingTwist;
  }
  if (NameContains(joint_name, "arm") ||
      NameContains(joint_name, "leg") ||
      NameContains(joint_name, "knee") ||
      NameContains(joint_name, "elbow")) {
    return ConstraintKind::Hinge;
  }
  return ConstraintKind::Fixed;
}

vkpt::scene::Mat4 IdentityMat4Local() {
  vkpt::scene::Mat4 m{};
  m.values[0] = 1.0f;
  m.values[5] = 1.0f;
  m.values[10] = 1.0f;
  m.values[15] = 1.0f;
  return m;
}

vkpt::scene::Mat4 MulMat4Local(const vkpt::scene::Mat4& a,
                               const vkpt::scene::Mat4& b) {
  vkpt::scene::Mat4 out{};
  for (std::size_t col = 0; col < 4u; ++col) {
    for (std::size_t row = 0; row < 4u; ++row) {
      float sum = 0.0f;
      for (std::size_t k = 0; k < 4u; ++k) {
        sum += a.values[k * 4u + row] * b.values[col * 4u + k];
      }
      out.values[col * 4u + row] = sum;
    }
  }
  return out;
}

}  // namespace

#ifdef PT_ENABLE_JOLT

namespace {

class SelfCollisionGroupFilter final : public JPH::GroupFilter {
 public:
  bool CanCollide(const JPH::CollisionGroup& a,
                  const JPH::CollisionGroup& b) const override {
    if (a.GetGroupID() == JPH::CollisionGroup::cInvalidGroup ||
        b.GetGroupID() == JPH::CollisionGroup::cInvalidGroup) {
      return true;
    }
    return a.GetGroupID() != b.GetGroupID();
  }
};

}  // namespace

struct Ragdoll::Impl {
  // Per-bone (body) record. The body represents the capsule between
  // joint.parent and joint.
  struct BoneRecord {
    std::int32_t joint_index = -1;
    std::int32_t parent_bone_index = -1;
    JPH::BodyID body_id;                  // valid once added to world
    JPH::BodyCreationSettings settings;    // pre-add settings
    JPH::TwoBodyConstraint* constraint = nullptr;  // owned by Jolt world
    ConstraintKind constraint_kind = ConstraintKind::Fixed;
    // Bone-end (joint position) in body-local space, used for joint readback.
    JPH::Vec3 bone_end_local{0.0f, 0.0f, 0.0f};
    // Joint world position at build time (used to seed constraints).
    JPH::RVec3 joint_world_at_build{};
    // Bone direction at build time (parent->child, normalized).
    JPH::Vec3 bone_dir_at_build = JPH::Vec3::sAxisY();
  };

  RagdollConfig config{};
  vkpt::animation::Skeleton skeleton;
  std::vector<vkpt::scene::Mat4> bind_world;
  std::vector<BoneRecord> bones;
  std::vector<std::int32_t> joint_to_bone;  // joint_index -> bones[] index, -1 if none
  vkpt::scene::Mat4 spawn_world = IdentityMat4Local();
  bool built = false;
  bool added_to_world = false;
  JPH::PhysicsSystem* world = nullptr;
  JPH::Ref<JPH::GroupFilter> group_filter;
};

#else  // !PT_ENABLE_JOLT

struct Ragdoll::Impl {
  struct BoneRecord {
    std::int32_t joint_index = -1;
    std::int32_t parent_bone_index = -1;
  };
  RagdollConfig config{};
  vkpt::animation::Skeleton skeleton;
  std::vector<vkpt::scene::Mat4> bind_world;
  std::vector<BoneRecord> bones;
  std::vector<std::int32_t> joint_to_bone;
  vkpt::scene::Mat4 spawn_world = IdentityMat4Local();
  bool built = false;
  bool added_to_world = false;
};

#endif

Ragdoll::Ragdoll() : m_impl(std::make_unique<Impl>()) {}
Ragdoll::~Ragdoll() = default;
Ragdoll::Ragdoll(Ragdoll&&) noexcept = default;
Ragdoll& Ragdoll::operator=(Ragdoll&&) noexcept = default;

bool Ragdoll::build(const vkpt::animation::Skeleton& skeleton,
                    const vkpt::scene::Mat4& spawn_world_transform,
                    const RagdollConfig& config) {
  if (skeleton.joints.empty() || skeleton.root_index < 0) {
    return false;
  }
  std::vector<std::string> issues;
  if (!vkpt::animation::validate(skeleton, &issues)) {
    return false;
  }

  m_impl->config = config;
  m_impl->skeleton = skeleton;
  m_impl->spawn_world = spawn_world_transform;
  m_impl->bind_world = vkpt::animation::compute_bind_world_matrices(skeleton);
  m_impl->bones.clear();
  m_impl->joint_to_bone.assign(skeleton.joints.size(),
                               static_cast<std::int32_t>(-1));

#ifdef PT_ENABLE_JOLT
  // Compute spawn-anchored joint world positions.
  std::vector<JPH::RVec3> joint_world_pos(skeleton.joints.size());
  for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
    const auto m = MulMat4Local(spawn_world_transform, m_impl->bind_world[i]);
    joint_world_pos[i] = JPH::RVec3(m.values[12], m.values[13], m.values[14]);
  }

  m_impl->bones.reserve(skeleton.joints.size());

  for (std::int32_t ji = 0;
       ji < static_cast<std::int32_t>(skeleton.joints.size()); ++ji) {
    const auto& joint = skeleton.joints[static_cast<std::size_t>(ji)];
    if (joint.parent_index < 0) {
      continue;  // root has no body
    }

    const auto parent_idx = static_cast<std::size_t>(joint.parent_index);
    const auto child_idx = static_cast<std::size_t>(ji);
    const JPH::RVec3 parent_pos = joint_world_pos[parent_idx];
    const JPH::RVec3 child_pos = joint_world_pos[child_idx];
    const JPH::Vec3 axis = JPH::Vec3(child_pos - parent_pos);
    const float bone_len = axis.Length();
    if (!std::isfinite(bone_len) || bone_len < 1.0e-4f) {
      continue;  // degenerate bone
    }
    const JPH::Vec3 axis_norm = axis / bone_len;

    float radius = bone_len * config.capsule_radius_scale;
    if (NameContains(joint.name, "head")) {
      radius = std::max(radius, config.head_capsule_radius);
    } else if (NameContains(joint.name, "body") ||
               NameContains(joint.name, "spine") ||
               NameContains(joint.name, "torso")) {
      radius = std::max(radius, config.spine_capsule_radius);
    }
    radius = std::max(radius, 0.02f);

    const float half_height = std::max(0.5f * bone_len - radius, 0.005f);

    // Capsule local axis is +Y. Rotate so local +Y maps to bone direction.
    JPH::Quat orient = JPH::Quat::sIdentity();
    const JPH::Vec3 local_y = JPH::Vec3::sAxisY();
    const float dot = local_y.Dot(axis_norm);
    if (dot > 0.9999f) {
      // Already aligned.
    } else if (dot < -0.9999f) {
      orient = JPH::Quat::sRotation(JPH::Vec3::sAxisX(),
                                    static_cast<float>(JPH::JPH_PI));
    } else {
      const JPH::Vec3 cross = local_y.Cross(axis_norm).Normalized();
      const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
      orient = JPH::Quat::sRotation(cross, angle);
    }

    const JPH::RVec3 center = parent_pos + JPH::RVec3(0.5f * axis);

    auto shape_result = JPH::CapsuleShapeSettings(half_height, radius).Create();
    if (shape_result.HasError()) {
      continue;
    }
    JPH::ShapeRefC shape = shape_result.Get();

    Impl::BoneRecord rec;
    rec.joint_index = ji;
    rec.parent_bone_index = m_impl->joint_to_bone[parent_idx];
    rec.constraint_kind = ChooseConstraintKind(joint.name);
    rec.bone_end_local = JPH::Vec3(0.0f, 0.5f * bone_len, 0.0f);
    rec.joint_world_at_build = child_pos;
    rec.bone_dir_at_build = axis_norm;
    rec.settings = JPH::BodyCreationSettings(
        shape,
        center,
        orient,
        JPH::EMotionType::Dynamic,
        static_cast<JPH::ObjectLayer>(kRagdollDynamicLayer));
    rec.settings.mAllowSleeping = true;
    rec.settings.mFriction = 0.5f;
    rec.settings.mRestitution = 0.0f;
    rec.settings.mGravityFactor = 1.0f;
    rec.settings.mOverrideMassProperties =
        JPH::EOverrideMassProperties::CalculateInertia;
    const float volume =
        static_cast<float>(JPH::JPH_PI) * radius * radius *
        (2.0f * half_height + (4.0f / 3.0f) * radius);
    rec.settings.mMassPropertiesOverride.mMass =
        std::max(0.05f, volume * config.density);

    m_impl->joint_to_bone[child_idx] =
        static_cast<std::int32_t>(m_impl->bones.size());
    m_impl->bones.push_back(std::move(rec));
  }

  if (m_impl->bones.empty()) {
    return false;
  }

  if (!config.self_collision) {
    m_impl->group_filter =
        JPH::Ref<JPH::GroupFilter>(new SelfCollisionGroupFilter());
  }

  // Stamp collision groups. The SelfCollisionGroupFilter treats "same
  // group_id" as non-colliding, which gives us no-self-collision when
  // self_collision == false. The filter is null when self_collision is
  // requested, so all bones collide normally in that mode.
  for (std::size_t i = 0; i < m_impl->bones.size(); ++i) {
    auto& rec = m_impl->bones[i];
    rec.settings.mCollisionGroup.SetGroupFilter(m_impl->group_filter.GetPtr());
    rec.settings.mCollisionGroup.SetGroupID(0x4011u);
    rec.settings.mCollisionGroup.SetSubGroupID(static_cast<JPH::uint32>(i + 1u));
  }

#else
  (void)spawn_world_transform;
  (void)config;
#endif

  m_impl->built = true;
  return true;
}

#ifdef PT_ENABLE_JOLT

void Ragdoll::add_to_world(JPH::PhysicsSystem& world) {
  if (!m_impl->built || m_impl->added_to_world) {
    return;
  }
  auto& bi = world.GetBodyInterface();

  // Phase 1: create + add bodies.
  for (auto& rec : m_impl->bones) {
    const JPH::BodyID id =
        bi.CreateAndAddBody(rec.settings, JPH::EActivation::Activate);
    rec.body_id = id;
  }

  // Phase 2: build constraints between bodies.
  for (auto& rec : m_impl->bones) {
    if (rec.parent_bone_index < 0) {
      continue;  // root bone (parent is the skeleton root joint, no body)
    }
    const auto parent_rec_idx = static_cast<std::size_t>(rec.parent_bone_index);
    auto& parent_rec = m_impl->bones[parent_rec_idx];
    if (rec.body_id.IsInvalid() || parent_rec.body_id.IsInvalid()) {
      continue;
    }

    JPH::TwoBodyConstraint* constraint = nullptr;
    const JPH::RVec3 joint_world = rec.joint_world_at_build;
    const JPH::Vec3 bone_dir = rec.bone_dir_at_build;

    // Pick a reference perpendicular axis. Use world up unless the bone is
    // near vertical, in which case use world Z.
    JPH::Vec3 perp_ref = JPH::Vec3::sAxisY();
    if (std::abs(bone_dir.Dot(perp_ref)) > 0.9f) {
      perp_ref = JPH::Vec3::sAxisZ();
    }
    const JPH::Vec3 hinge_axis = bone_dir.Cross(perp_ref).Normalized();
    const JPH::Vec3 normal_axis = bone_dir;

    {
      // Construct the constraint while both bodies are write-locked. Jolt
      // constraint Create() reads body refs once (transforms, COM offsets)
      // and stores constraint-local data; releasing the lock after Create
      // returns is safe.
      const JPH::BodyLockInterfaceLocking& lock_iface =
          world.GetBodyLockInterface();
      const JPH::BodyID ids[2] = {parent_rec.body_id, rec.body_id};
      JPH::BodyLockMultiWrite lock(lock_iface, ids, 2);
      JPH::Body* body_parent = lock.GetBody(0);
      JPH::Body* body_child = lock.GetBody(1);
      if (body_parent == nullptr || body_child == nullptr) {
        continue;
      }

      switch (rec.constraint_kind) {
        case ConstraintKind::Hinge: {
          JPH::HingeConstraintSettings s;
          s.mSpace = JPH::EConstraintSpace::WorldSpace;
          s.mPoint1 = joint_world;
          s.mPoint2 = joint_world;
          s.mHingeAxis1 = hinge_axis;
          s.mHingeAxis2 = hinge_axis;
          s.mNormalAxis1 = normal_axis;
          s.mNormalAxis2 = normal_axis;
          s.mLimitsMin = -10.0f * static_cast<float>(JPH::JPH_PI) / 180.0f;
          s.mLimitsMax = 150.0f * static_cast<float>(JPH::JPH_PI) / 180.0f;
          constraint = s.Create(*body_parent, *body_child);
          break;
        }
        case ConstraintKind::SwingTwist: {
          JPH::SwingTwistConstraintSettings s;
          s.mSpace = JPH::EConstraintSpace::WorldSpace;
          s.mPosition1 = joint_world;
          s.mPosition2 = joint_world;
          s.mTwistAxis1 = bone_dir;
          s.mTwistAxis2 = bone_dir;
          s.mPlaneAxis1 = hinge_axis;
          s.mPlaneAxis2 = hinge_axis;
          s.mNormalHalfConeAngle = 60.0f * static_cast<float>(JPH::JPH_PI) / 180.0f;
          s.mPlaneHalfConeAngle = 60.0f * static_cast<float>(JPH::JPH_PI) / 180.0f;
          s.mTwistMinAngle = -90.0f * static_cast<float>(JPH::JPH_PI) / 180.0f;
          s.mTwistMaxAngle = 90.0f * static_cast<float>(JPH::JPH_PI) / 180.0f;
          constraint = s.Create(*body_parent, *body_child);
          break;
        }
        case ConstraintKind::Fixed:
        default: {
          JPH::FixedConstraintSettings s;
          s.mSpace = JPH::EConstraintSpace::WorldSpace;
          s.mPoint1 = joint_world;
          s.mPoint2 = joint_world;
          s.mAxisX1 = hinge_axis;
          s.mAxisX2 = hinge_axis;
          s.mAxisY1 = bone_dir;
          s.mAxisY2 = bone_dir;
          constraint = s.Create(*body_parent, *body_child);
          break;
        }
      }
    }

    if (constraint != nullptr) {
      world.AddConstraint(constraint);
      rec.constraint = constraint;
    }
  }

  m_impl->world = &world;
  m_impl->added_to_world = true;
}

void Ragdoll::remove_from_world(JPH::PhysicsSystem& world) {
  if (!m_impl->added_to_world) {
    return;
  }
  for (auto& rec : m_impl->bones) {
    if (rec.constraint != nullptr) {
      world.RemoveConstraint(rec.constraint);
      rec.constraint = nullptr;
    }
  }
  auto& bi = world.GetBodyInterface();
  for (auto& rec : m_impl->bones) {
    if (!rec.body_id.IsInvalid()) {
      bi.RemoveBody(rec.body_id);
      bi.DestroyBody(rec.body_id);
      rec.body_id = JPH::BodyID();
    }
  }
  m_impl->added_to_world = false;
  m_impl->world = nullptr;
}

#endif  // PT_ENABLE_JOLT

std::vector<vkpt::scene::Mat4> Ragdoll::read_joint_world_matrices() const {
  std::vector<vkpt::scene::Mat4> out;
  if (!m_impl->built) {
    return out;
  }
  out.assign(m_impl->skeleton.joints.size(), IdentityMat4Local());

#ifdef PT_ENABLE_JOLT
  // Helper: write a Jolt body's world transform into out[joint], then move
  // by bone_end_local so the joint sits at the end of the capsule.
  auto write_body_to_joint = [&](std::size_t joint_index,
                                 const Impl::BoneRecord& rec) {
    if (m_impl->world == nullptr || rec.body_id.IsInvalid()) {
      // Not yet added: fall back to bind pose.
      out[joint_index] = MulMat4Local(m_impl->spawn_world,
                                      m_impl->bind_world[joint_index]);
      return;
    }
    JPH::RVec3 pos;
    JPH::Quat rot;
    m_impl->world->GetBodyInterface().GetPositionAndRotation(rec.body_id, pos,
                                                              rot);
    // Joint world = body center + body rotation * bone_end_local
    const JPH::Vec3 offset = rot * rec.bone_end_local;
    const JPH::RVec3 joint_world = pos + JPH::RVec3(offset);
    JPH::Mat44 m44 = JPH::Mat44::sRotationTranslation(rot, joint_world);

    vkpt::scene::Mat4 result{};
    for (std::size_t col = 0; col < 4u; ++col) {
      JPH::Vec4 c = m44.GetColumn4(static_cast<JPH::uint>(col));
      result.values[col * 4u + 0u] = c.GetX();
      result.values[col * 4u + 1u] = c.GetY();
      result.values[col * 4u + 2u] = c.GetZ();
      result.values[col * 4u + 3u] = c.GetW();
    }
    out[joint_index] = result;
  };

  for (std::size_t i = 0; i < m_impl->skeleton.joints.size(); ++i) {
    const std::int32_t bone_idx = m_impl->joint_to_bone[i];
    if (bone_idx >= 0) {
      write_body_to_joint(i,
                          m_impl->bones[static_cast<std::size_t>(bone_idx)]);
    } else {
      // Root joint (or degenerate joint with no body): inherit from the
      // child bone's body, or fall back to spawn-anchored bind pose if no
      // child is owned. For the smoke we just use the bind world translated
      // to spawn — the root joint sits at the head of the spine bone.
      out[i] = MulMat4Local(m_impl->spawn_world, m_impl->bind_world[i]);
      // Try to refine using a child bone: search for a bone whose joint's
      // parent is i, and use that bone's body position (the joint at this
      // joint sits at the start of that bone, i.e. body_center -
      // bone_end_local).
      for (const auto& rec : m_impl->bones) {
        if (rec.joint_index < 0) continue;
        const auto& jj =
            m_impl->skeleton
                .joints[static_cast<std::size_t>(rec.joint_index)];
        if (static_cast<std::size_t>(jj.parent_index) == i &&
            !rec.body_id.IsInvalid() && m_impl->world != nullptr) {
          JPH::RVec3 pos;
          JPH::Quat rot;
          m_impl->world->GetBodyInterface().GetPositionAndRotation(
              rec.body_id, pos, rot);
          const JPH::Vec3 offset = rot * (-rec.bone_end_local);
          const JPH::RVec3 joint_world = pos + JPH::RVec3(offset);
          JPH::Mat44 m44 =
              JPH::Mat44::sRotationTranslation(rot, joint_world);
          vkpt::scene::Mat4 result{};
          for (std::size_t col = 0; col < 4u; ++col) {
            JPH::Vec4 c = m44.GetColumn4(static_cast<JPH::uint>(col));
            result.values[col * 4u + 0u] = c.GetX();
            result.values[col * 4u + 1u] = c.GetY();
            result.values[col * 4u + 2u] = c.GetZ();
            result.values[col * 4u + 3u] = c.GetW();
          }
          out[i] = result;
          break;
        }
      }
    }
  }
#else
  // Fallback (Jolt disabled): return spawn-anchored bind pose.
  for (std::size_t i = 0; i < m_impl->skeleton.joints.size(); ++i) {
    out[i] = MulMat4Local(m_impl->spawn_world, m_impl->bind_world[i]);
  }
#endif

  return out;
}

std::size_t Ragdoll::body_count() const noexcept {
  return m_impl == nullptr ? 0u : m_impl->bones.size();
}

std::size_t Ragdoll::constraint_count() const noexcept {
#ifdef PT_ENABLE_JOLT
  if (m_impl == nullptr) {
    return 0u;
  }
  std::size_t n = 0u;
  for (const auto& rec : m_impl->bones) {
    if (rec.constraint != nullptr) {
      ++n;
    }
  }
  return n;
#else
  return 0u;
#endif
}

std::size_t Ragdoll::joint_count() const noexcept {
  return m_impl == nullptr ? 0u : m_impl->skeleton.joints.size();
}

bool Ragdoll::is_built() const noexcept {
  return m_impl != nullptr && m_impl->built;
}

bool Ragdoll::is_added_to_world() const noexcept {
  return m_impl != nullptr && m_impl->added_to_world;
}

#ifdef PT_ENABLE_JOLT

namespace {

// Extract the world translation (column 3) from a column-major 4x4.
JPH::RVec3 ExtractTranslation(const vkpt::scene::Mat4& m) {
  return JPH::RVec3(m.values[12], m.values[13], m.values[14]);
}

// Build a Jolt orientation that maps capsule local +Y to bone direction
// (parent->child). Mirrors the alignment math used in Ragdoll::build().
JPH::Quat OrientForBoneDirection(const JPH::Vec3& dir_norm) {
  const JPH::Vec3 local_y = JPH::Vec3::sAxisY();
  const float dot = local_y.Dot(dir_norm);
  if (dot > 0.9999f) {
    return JPH::Quat::sIdentity();
  }
  if (dot < -0.9999f) {
    return JPH::Quat::sRotation(JPH::Vec3::sAxisX(),
                                static_cast<float>(JPH::JPH_PI));
  }
  const JPH::Vec3 cross = local_y.Cross(dir_norm).Normalized();
  const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
  return JPH::Quat::sRotation(cross, angle);
}

}  // namespace

#endif  // PT_ENABLE_JOLT

bool Ragdoll::seed_pose_from_skeleton(
    const std::vector<vkpt::scene::Mat4>& joint_world_matrices,
    const std::vector<vkpt::scene::Mat4>* prev_joint_world_matrices,
    float dt) {
  if (m_impl == nullptr || !m_impl->built) {
    return false;
  }
  const std::size_t joint_count = m_impl->skeleton.joints.size();
  if (joint_world_matrices.size() != joint_count) {
    return false;
  }
  if (prev_joint_world_matrices != nullptr &&
      prev_joint_world_matrices->size() != joint_count) {
    return false;
  }

#ifdef PT_ENABLE_JOLT
  if (m_impl->world == nullptr) {
    return false;  // not added to a world yet
  }
  auto& bi = m_impl->world->GetBodyInterface();

  const bool has_velocity = (prev_joint_world_matrices != nullptr) &&
                            std::isfinite(dt) && dt > 0.0f;

  for (auto& rec : m_impl->bones) {
    if (rec.body_id.IsInvalid() || rec.joint_index < 0) {
      continue;
    }
    const auto child_idx = static_cast<std::size_t>(rec.joint_index);
    const auto& joint = m_impl->skeleton.joints[child_idx];
    if (joint.parent_index < 0) {
      continue;
    }
    const auto parent_idx = static_cast<std::size_t>(joint.parent_index);

    const JPH::RVec3 parent_pos =
        ExtractTranslation(joint_world_matrices[parent_idx]);
    const JPH::RVec3 child_pos =
        ExtractTranslation(joint_world_matrices[child_idx]);
    const JPH::Vec3 axis = JPH::Vec3(child_pos - parent_pos);
    const float bone_len = axis.Length();
    if (!std::isfinite(bone_len) || bone_len < 1.0e-4f) {
      // Degenerate; leave body where it is.
      continue;
    }
    const JPH::Vec3 dir_norm = axis / bone_len;
    const JPH::Quat orient = OrientForBoneDirection(dir_norm);
    const JPH::RVec3 center = parent_pos + JPH::RVec3(0.5f * axis);

    bi.SetPositionAndRotation(rec.body_id, center, orient,
                              JPH::EActivation::Activate);

    JPH::Vec3 lin_vel(0.0f, 0.0f, 0.0f);
    JPH::Vec3 ang_vel(0.0f, 0.0f, 0.0f);
    if (has_velocity) {
      const auto& prev = *prev_joint_world_matrices;
      const JPH::RVec3 prev_parent = ExtractTranslation(prev[parent_idx]);
      const JPH::RVec3 prev_child = ExtractTranslation(prev[child_idx]);
      const JPH::RVec3 prev_center = prev_parent + JPH::RVec3(0.5f *
                                       JPH::Vec3(prev_child - prev_parent));
      const JPH::Vec3 lin = JPH::Vec3(center - prev_center) / dt;
      // Angular velocity from the change in bone direction. Use the small-angle
      // approximation: omega ~= (prev_dir x cur_dir) / dt — this is finite for
      // any non-degenerate previous pose and produces zero when both poses
      // coincide.
      const JPH::Vec3 prev_axis = JPH::Vec3(prev_child - prev_parent);
      const float prev_len = prev_axis.Length();
      if (std::isfinite(prev_len) && prev_len > 1.0e-4f) {
        const JPH::Vec3 prev_dir = prev_axis / prev_len;
        ang_vel = prev_dir.Cross(dir_norm) / dt;
      }
      if (std::isfinite(lin.GetX()) && std::isfinite(lin.GetY()) &&
          std::isfinite(lin.GetZ())) {
        lin_vel = lin;
      }
    }
    bi.SetLinearVelocity(rec.body_id, lin_vel);
    bi.SetAngularVelocity(rec.body_id, ang_vel);
  }

  return true;
#else
  (void)joint_world_matrices;
  (void)prev_joint_world_matrices;
  (void)dt;
  return false;
#endif
}

bool Ragdoll::apply_impulse_to_joint(std::int32_t joint_index,
                                     vkpt::scene::Vec3 impulse) {
  if (m_impl == nullptr || !m_impl->built) {
    return false;
  }
  if (joint_index < 0 ||
      static_cast<std::size_t>(joint_index) >= m_impl->joint_to_bone.size()) {
    return false;
  }
  const std::int32_t bone_index = m_impl->joint_to_bone[
      static_cast<std::size_t>(joint_index)];
  if (bone_index < 0) {
    // Root joint (no body owns it). Try to forward the impulse to the first
    // child bone whose parent is this joint — that's the "spine root" body.
    for (std::size_t b = 0; b < m_impl->bones.size(); ++b) {
      const auto& rec = m_impl->bones[b];
      if (rec.joint_index < 0) continue;
      const auto& jj = m_impl->skeleton.joints[
          static_cast<std::size_t>(rec.joint_index)];
      if (jj.parent_index == joint_index) {
#ifdef PT_ENABLE_JOLT
        if (m_impl->world == nullptr || rec.body_id.IsInvalid()) {
          return false;
        }
        m_impl->world->GetBodyInterface().AddImpulse(
            rec.body_id,
            JPH::Vec3(impulse.x, impulse.y, impulse.z));
        return true;
#else
        (void)impulse;
        return false;
#endif
      }
    }
    return false;
  }

#ifdef PT_ENABLE_JOLT
  if (m_impl->world == nullptr) {
    return false;
  }
  const auto& rec = m_impl->bones[static_cast<std::size_t>(bone_index)];
  if (rec.body_id.IsInvalid()) {
    return false;
  }
  m_impl->world->GetBodyInterface().AddImpulse(
      rec.body_id,
      JPH::Vec3(impulse.x, impulse.y, impulse.z));
  return true;
#else
  (void)impulse;
  return false;
#endif
}

}  // namespace vkpt::physics
