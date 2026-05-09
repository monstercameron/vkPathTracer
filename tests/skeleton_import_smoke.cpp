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

int main(int argc, char** argv) {
  std::filesystem::path gltf_path =
      "assets/models/low_poly_hero/character.gltf";
  if (argc > 1) {
    gltf_path = argv[1];
  }
  // Walk upwards looking for the assets root, since the binary is placed in a
  // build directory at runtime.
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

  if (!Check(std::filesystem::exists(gltf_path),
             "hero glTF asset not found")) {
    return 1;
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
