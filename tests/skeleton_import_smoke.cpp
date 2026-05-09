// Phase 1 ANI01 smoke: load the hero glTF skeleton, validate it, then round-trip
// it through SceneDocument JSON. Prints `skeleton_import_smoke: ok` on success.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "animation/Skeleton.h"
#include "assets/SceneAssetLoaderInternal.h"
#include "scene/Scene.h"
#include "scene/SceneDocument.h"
#include "scene/SceneDocumentSchema.h"

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "skeleton_import_smoke: " << message << "\n";
  }
  return condition;
}

constexpr float kEps = 1.0e-6f;

bool MatricesEqual(const vkpt::scene::Mat4& a, const vkpt::scene::Mat4& b) {
  for (std::size_t i = 0; i < 16u; ++i) {
    const float diff = std::abs(a.values[i] - b.values[i]);
    if (!std::isfinite(diff) || diff > kEps) {
      return false;
    }
  }
  return true;
}

}  // namespace

// Resolve a relative glTF path by walking up from CWD looking for the file.
static std::filesystem::path resolve_asset(const std::filesystem::path& rel) {
  if (std::filesystem::exists(rel)) {
    return rel;
  }
  auto cwd = std::filesystem::current_path();
  for (int i = 0; i < 8; ++i) {
    const auto candidate = cwd / rel;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
    if (!cwd.has_parent_path() || cwd.parent_path() == cwd) {
      break;
    }
    cwd = cwd.parent_path();
  }
  return rel;
}

// Validate a glTF skeleton: 14 joints, single root, JOINTS_0/WEIGHTS_0
// vertex attributes present (the latter via skinned_attributes_present on
// at least one primitive). Returns true on success; logs to stderr otherwise.
static bool validate_humanoid_gltf(const std::filesystem::path& gltf_path,
                                   const char* label) {
  std::vector<std::string> diagnostics;
  auto loaded = vkpt::assets::scene_asset_detail::LoadGltf(gltf_path, &diagnostics);
  for (const auto& msg : diagnostics) {
    std::cerr << "skeleton_import_smoke[" << label << " diag]: " << msg << "\n";
  }
  if (!loaded.skeleton.has_value()) {
    std::cerr << "skeleton_import_smoke[" << label << "]: skeleton missing\n";
    return false;
  }
  const auto& skel = *loaded.skeleton;
  if (skel.joints.size() != 14u) {
    std::cerr << "skeleton_import_smoke[" << label << "]: expected 14 joints, got "
              << skel.joints.size() << "\n";
    return false;
  }
  std::int32_t roots = 0;
  for (const auto& j : skel.joints) {
    if (j.parent_index == -1) ++roots;
  }
  if (roots != 1) {
    std::cerr << "skeleton_import_smoke[" << label << "]: expected 1 root, got "
              << roots << "\n";
    return false;
  }
  // JOINTS_0 + WEIGHTS_0 confirmation: any geometry bucket with non-zero
  // joint_indices count proves the loader extracted skinning attributes.
  bool any_skinned_prim = false;
  for (const auto& bucket : loaded.geometry) {
    if (!bucket.joint_indices.empty() && !bucket.joint_weights.empty()) {
      any_skinned_prim = true;
      break;
    }
  }
  if (!any_skinned_prim) {
    std::cerr << "skeleton_import_smoke[" << label
              << "]: no primitive has JOINTS_0/WEIGHTS_0\n";
    return false;
  }
  std::cout << "skeleton_import_smoke[" << label << "]: 14 joints, 1 root, skinning attrs ok\n";
  return true;
}

int main(int argc, char** argv) {
  std::filesystem::path gltf_path =
      "assets/models/low_poly_hero/character.gltf";
  if (argc > 1) {
    gltf_path = argv[1];
  }
  gltf_path = resolve_asset(gltf_path);

  if (!Check(std::filesystem::exists(gltf_path),
             "hero glTF asset not found")) {
    return 1;
  }

  // Multi-target: also validate the auto-rigged Meshy character if present.
  // Non-fatal if the file is missing (e.g. on CI without the rigged asset).
  const auto rigged_path = resolve_asset(
      "assets/models/black_ops_operator_rigged/character.gltf");
  if (std::filesystem::exists(rigged_path)) {
    if (!validate_humanoid_gltf(rigged_path, "black_ops_operator_rigged")) {
      return 1;
    }
  } else {
    std::cout << "skeleton_import_smoke: black_ops_operator_rigged asset not "
                 "present, skipping auto-rigged target\n";
  }

  std::vector<std::string> diagnostics;
  auto loaded =
      vkpt::assets::scene_asset_detail::LoadGltf(gltf_path, &diagnostics);
  for (const auto& msg : diagnostics) {
    std::cerr << "skeleton_import_smoke[diag]: " << msg << "\n";
  }

  if (!Check(loaded.skeleton.has_value(),
             "ObjLoadResult.skeleton missing")) {
    return 1;
  }
  const auto& skeleton = *loaded.skeleton;

  if (!Check(skeleton.joints.size() == 14u,
             "expected 14 joints in hero skeleton")) {
    std::cerr << "  got " << skeleton.joints.size() << "\n";
    return 1;
  }

  std::int32_t roots = 0;
  for (const auto& joint : skeleton.joints) {
    if (joint.parent_index == -1) {
      ++roots;
    }
  }
  if (!Check(roots == 1, "expected exactly one root joint")) {
    std::cerr << "  got " << roots << "\n";
    return 1;
  }

  std::vector<std::string> issues;
  if (!Check(vkpt::animation::validate(skeleton, &issues),
             "skeleton validation failed")) {
    for (const auto& issue : issues) {
      std::cerr << "  issue: " << issue << "\n";
    }
    return 1;
  }
  if (!Check(issues.empty(), "validate() returned issues despite ok")) {
    return 1;
  }

  // bind world matrices: confirm each joint composes to a finite matrix.
  const auto world_mats = vkpt::animation::compute_bind_world_matrices(skeleton);
  if (!Check(world_mats.size() == skeleton.joints.size(),
             "compute_bind_world_matrices produced wrong size")) {
    return 1;
  }
  for (const auto& m : world_mats) {
    for (float v : m.values) {
      if (!Check(std::isfinite(v),
                 "bind world matrix has non-finite value")) {
        return 1;
      }
    }
  }

  // Build a SceneDocument with the skeleton attached to a single entity, then
  // round-trip through to_json/load_from_text.
  vkpt::scene::SceneDocument document;
  document.metadata.scene_name = "skeleton_smoke";
  vkpt::scene::SceneEntityDefinition root_entity;
  root_entity.id = 1;
  root_entity.name = "hero_root";
  root_entity.has_transform = true;
  root_entity.transform = vkpt::scene::TransformComponent{};
  root_entity.has_skeleton = true;
  root_entity.skeleton = skeleton;
  document.entities.push_back(std::move(root_entity));

  const auto json = document.to_json(false);
  if (!Check(!json.empty(), "to_json produced empty string")) {
    return 1;
  }

  auto reparsed = vkpt::scene::SceneDocument::load_from_text(json);
  if (!Check(static_cast<bool>(reparsed),
             "load_from_text failed on round-tripped JSON")) {
    return 1;
  }
  const auto& reloaded_doc = reparsed.value();
  if (!Check(reloaded_doc.entities.size() == 1u,
             "round-tripped document missing entity")) {
    return 1;
  }
  const auto& reloaded_entity = reloaded_doc.entities.front();
  if (!Check(reloaded_entity.has_skeleton,
             "round-tripped entity missing skeleton")) {
    return 1;
  }
  const auto& reloaded_skel = reloaded_entity.skeleton;
  if (!Check(reloaded_skel.joints.size() == skeleton.joints.size(),
             "round-tripped joint count mismatch")) {
    return 1;
  }
  if (!Check(reloaded_skel.root_index == skeleton.root_index,
             "round-tripped root_index mismatch")) {
    return 1;
  }
  for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
    const auto& src = skeleton.joints[i];
    const auto& dst = reloaded_skel.joints[i];
    if (!Check(src.name == dst.name, "joint name mismatch after round-trip")) {
      std::cerr << "  index=" << i << " src=" << src.name
                << " dst=" << dst.name << "\n";
      return 1;
    }
    if (!Check(src.parent_index == dst.parent_index,
               "joint parent_index mismatch after round-trip")) {
      return 1;
    }
    if (!Check(MatricesEqual(src.inverse_bind, dst.inverse_bind),
               "joint inverse_bind mismatch after round-trip")) {
      return 1;
    }
  }

  std::cout << "skeleton_import_smoke: hero joint roster (" << skeleton.joints.size() << ")";
  for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
    std::cout << "\n  [" << i << "] " << skeleton.joints[i].name
              << " parent=" << skeleton.joints[i].parent_index;
  }
  std::cout << "\n";
  std::cout << "skeleton_import_smoke: ok\n";
  return 0;
}
