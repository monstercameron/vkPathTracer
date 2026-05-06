#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/Types.h"
#include "scene/SceneDocumentSchema.h"

namespace vkpt::scene {

struct SceneSnapshot {
  /// Hash over authored scene inputs used for cache/export invalidation.
  vkpt::core::Hash256 scene_hash{};
  /// Asset URIs referenced by the authored document.
  std::vector<std::string> asset_refs;
  /// Stable entity IDs in document/world traversal order.
  std::vector<vkpt::core::StableId> entity_ids;

  struct RenderableObject {
    vkpt::core::StableId entity_id = 0;
    vkpt::core::StableId mesh_id = 0;
    vkpt::core::StableId material_id = 0;
    TransformComponent transform;
  };

  struct LightObject {
    vkpt::core::StableId entity_id = 0;
    LightComponent light;
    TransformComponent transform;
  };

  struct MaterialObject {
    vkpt::core::StableId id = 0;
    SceneMaterialDefinition material;
  };

  std::vector<RenderableObject> renderables;
  std::vector<LightObject> lights;
  std::vector<MaterialObject> materials;
  std::optional<SceneCameraDefinition> camera;
  SceneBenchmarkMetadata benchmark{};
};

struct RenderSceneProxy {
  /// Scene content hash propagated to render backends for frame cache decisions.
  vkpt::core::Hash256 scene_hash{};
  vkpt::core::FrameIndex frame = 0;

  /// Renderer-facing mesh instance with the resolved world transform baked in.
  struct Renderable {
    vkpt::core::StableEntityId entity_id = 0;
    vkpt::core::AssetId geometry_id = 0;
    vkpt::core::MaterialId material_id = 0;
    Mat4 world_matrix{};
    Vec3 translation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
  };

  /// Renderer-facing light with transform-derived position and world matrix.
  struct Light {
    vkpt::core::StableEntityId entity_id = 0;
    std::string type;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 0.0f;
    Mat4 world_matrix{};
    Vec3 position{};
  };

  /// Renderer-facing material subset consumed by current path tracing backends.
  struct Material {
    vkpt::core::MaterialId id = 0;
    Vec3 albedo{1.0f, 1.0f, 1.0f};
    float roughness = 1.0f;
    Vec3 emission{0.0f, 0.0f, 0.0f};
    float emission_intensity = 0.0f;
  };

  /// Renderer-facing camera model including physical camera parameters.
  struct Camera {
    vkpt::core::StableEntityId entity_id = 0;
    float fov = 60.0f;
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
    float focal_length_mm = 35.0f;
    float sensor_width_mm = 36.0f;
    float sensor_height_mm = 24.0f;
    float aperture_radius = 0.0f;
    float focus_distance = 0.0f;
    float f_stop = 0.0f;
    float shutter_seconds = 0.0166666675f;
    float iso = 100.0f;
    float exposure_compensation = 0.0f;
    float white_balance_kelvin = 6500.0f;
    std::uint32_t iris_blade_count = 0u;
    float iris_rotation_degrees = 0.0f;
    float iris_roundness = 1.0f;
    float anamorphic_squeeze = 1.0f;
    Mat4 world_matrix{};
    Vec3 position{};
  };

  std::vector<Renderable> renderables;
  std::vector<Light> lights;
  std::vector<Material> materials;
  std::optional<Camera> camera;
  SceneBenchmarkMetadata benchmark{};

  bool empty() const {
    return renderables.empty() && lights.empty() && materials.empty() && !camera.has_value();
  }
};

}  // namespace vkpt::scene
