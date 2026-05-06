#include "editor/QtPanelModels.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "render/DebugViews.h"

namespace vkpt::editor {
namespace {

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

std::string IdText(vkpt::core::StableId id) {
  if (id == 0) {
    return "none";
  }
  return std::to_string(id);
}

std::string FloatText(double value, int precision = 3) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

std::string Vec3Text(const vkpt::scene::Vec3& value) {
  return FloatText(value.x) + ", " + FloatText(value.y) + ", " + FloatText(value.z);
}

std::string Vec3Text(const vkpt::pathtracer::Vec3& value) {
  return FloatText(value.x) + ", " + FloatText(value.y) + ", " + FloatText(value.z);
}

std::string QuatText(const vkpt::scene::Quat& value) {
  return FloatText(value.x) + ", " + FloatText(value.y) + ", " +
         FloatText(value.z) + ", " + FloatText(value.w);
}

std::string ResolutionText(std::uint32_t width, std::uint32_t height) {
  return std::to_string(width) + " x " + std::to_string(height);
}

std::string BoundsText(const Bounds& bounds) {
  if (!bounds.valid) {
    return "unavailable";
  }
  const auto min = FloatText(bounds.min.x) + ", " + FloatText(bounds.min.y) + ", " + FloatText(bounds.min.z);
  const auto max = FloatText(bounds.max.x) + ", " + FloatText(bounds.max.y) + ", " + FloatText(bounds.max.z);
  return "min(" + min + ") max(" + max + ")";
}

QtPanelProperty Property(std::string id,
                         std::string label,
                         std::string value,
                         std::string group = {},
                         QtPanelPropertyKind kind = QtPanelPropertyKind::Text,
                         bool editable = false) {
  QtPanelProperty property;
  property.id = std::move(id);
  property.label = std::move(label);
  property.value = std::move(value);
  property.group = std::move(group);
  property.kind = kind;
  property.editable = editable;
  return property;
}

QtPanelRow Row(std::string id,
               std::string label,
               std::string detail = {},
               std::string icon = {},
               std::uint32_t depth = 0) {
  QtPanelRow row;
  row.id = std::move(id);
  row.label = std::move(label);
  row.detail = std::move(detail);
  row.icon = std::move(icon);
  row.depth = depth;
  return row;
}

void AddProperty(QtPanelModel& model,
                 std::string id,
                 std::string label,
                 std::string value,
                 std::string group = {},
                 QtPanelPropertyKind kind = QtPanelPropertyKind::Text,
                 bool editable = false) {
  model.properties.push_back(Property(std::move(id), std::move(label), std::move(value), std::move(group), kind, editable));
}

bool IsSelected(const QtPanelBuildContext& context, vkpt::core::StableId id) {
  if (context.selection == nullptr) {
    return false;
  }
  const auto& selected = context.selection->selected_entity_ids;
  return std::find(selected.begin(), selected.end(), id) != selected.end();
}

std::string EntityName(const vkpt::scene::SceneEntityDefinition& entity) {
  if (!entity.name.empty()) {
    return entity.name;
  }
  return "Entity " + IdText(entity.id);
}

const vkpt::scene::SceneEntityDefinition* FindDocumentEntity(const QtPanelBuildContext& context,
                                                             vkpt::core::StableId id) {
  if (context.document == nullptr) {
    return nullptr;
  }
  for (const auto& entity : context.document->entities) {
    if (entity.id == id) {
      return &entity;
    }
  }
  return nullptr;
}

std::string EntityComponentSummary(const vkpt::scene::SceneEntityDefinition& entity) {
  std::vector<std::string> components;
  if (entity.has_transform) {
    components.push_back("Transform");
  }
  if (entity.has_mesh) {
    components.push_back("Mesh");
  }
  if (entity.has_light) {
    components.push_back("Light");
  }
  if (entity.has_camera) {
    components.push_back("Camera");
  }
  if (!entity.script.script.empty()) {
    components.push_back("Script");
  }
  if (!entity.animation.clip.empty()) {
    components.push_back("Animation");
  }
  if (entity.has_physics_body) {
    components.push_back(entity.physics_body.enabled ? "Physics" : "Physics Off");
  }
  if (components.empty()) {
    return "Entity";
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < components.size(); ++i) {
    if (i > 0) {
      out << ", ";
    }
    out << components[i];
  }
  return out.str();
}

void AddTransformProperties(std::vector<QtPanelProperty>& properties,
                            const vkpt::scene::TransformComponent& transform,
                            bool editable) {
  properties.push_back(Property("transform.translation", "Position", Vec3Text(transform.translation),
                                "Transform", QtPanelPropertyKind::Vector3, editable));
  properties.push_back(Property("transform.rotation", "Rotation", QuatText(transform.rotation),
                                "Transform", QtPanelPropertyKind::Vector3, editable));
  properties.push_back(Property("transform.scale", "Scale", Vec3Text(transform.scale),
                                "Transform", QtPanelPropertyKind::Vector3, editable));
  properties.push_back(Property("transform.dirty", "Dirty", BoolText(transform.dirty),
                                "Transform", QtPanelPropertyKind::Toggle, false));
}

void AddCameraComponentProperties(std::vector<QtPanelProperty>& properties,
                                  const vkpt::scene::CameraComponent& camera,
                                  bool editable) {
  properties.push_back(Property("camera.fov", "FOV", FloatText(camera.fov), "Camera",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.near", "Near", FloatText(camera.near_plane), "Camera",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.far", "Far", FloatText(camera.far_plane), "Camera",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.focal_length_mm", "Focal Length", FloatText(camera.focal_length_mm), "Lens",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.sensor_width_mm", "Sensor Width", FloatText(camera.sensor_width_mm), "Lens",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.sensor_height_mm", "Sensor Height", FloatText(camera.sensor_height_mm), "Lens",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.aperture_radius", "Aperture Radius", FloatText(camera.aperture_radius), "Lens",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.focus_distance", "Focus Distance", FloatText(camera.focus_distance), "Lens",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.f_stop", "F-stop", FloatText(camera.f_stop), "Lens",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.shutter_seconds", "Shutter", FloatText(camera.shutter_seconds), "Exposure",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.iso", "ISO", FloatText(camera.iso), "Exposure",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.exposure_compensation", "Exposure Compensation",
                                FloatText(camera.exposure_compensation), "Exposure",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.white_balance_kelvin", "White Balance",
                                FloatText(camera.white_balance_kelvin), "Color",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.iris_blade_count", "Iris Blades",
                                std::to_string(camera.iris_blade_count), "Iris",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.iris_rotation_degrees", "Iris Rotation",
                                FloatText(camera.iris_rotation_degrees), "Iris",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.iris_roundness", "Iris Roundness",
                                FloatText(camera.iris_roundness), "Iris",
                                QtPanelPropertyKind::Number, editable));
  properties.push_back(Property("camera.anamorphic_squeeze", "Anamorphic Squeeze",
                                FloatText(camera.anamorphic_squeeze), "Lens",
                                QtPanelPropertyKind::Number, editable));
}

void AddEntityProperties(QtPanelRow& row, const vkpt::scene::SceneEntityDefinition& entity) {
  row.properties.push_back(Property("entity.id", "Stable ID", IdText(entity.id), "Entity", QtPanelPropertyKind::Entity));
  row.properties.push_back(Property("entity.name", "Name", EntityName(entity), "Entity", QtPanelPropertyKind::Text, true));
  row.properties.push_back(Property("entity.components", "Components", EntityComponentSummary(entity), "Entity"));
  if (entity.hierarchy.parent != 0) {
    row.properties.push_back(Property("hierarchy.parent", "Parent", IdText(entity.hierarchy.parent), "Hierarchy",
                                      QtPanelPropertyKind::Entity, true));
  }
  if (entity.has_transform) {
    AddTransformProperties(row.properties, entity.transform, true);
  }
  if (entity.has_mesh) {
    row.properties.push_back(Property("mesh.geometry", "Geometry", IdText(entity.mesh.mesh_id), "Mesh",
                                      QtPanelPropertyKind::Asset, true));
    row.properties.push_back(Property("mesh.material", "Material", IdText(entity.mesh.material_id), "Mesh",
                                      QtPanelPropertyKind::Asset, true));
  }
  if (entity.material.material_id != 0) {
    row.properties.push_back(Property("material.override", "Material Override", IdText(entity.material.material_id),
                                      "Material", QtPanelPropertyKind::Asset, true));
  }
  if (entity.has_light) {
    row.properties.push_back(Property("light.type", "Type", entity.light.type, "Light", QtPanelPropertyKind::Text, true));
    row.properties.push_back(Property("light.color", "Color", Vec3Text(entity.light.color), "Light",
                                      QtPanelPropertyKind::Color, true));
    row.properties.push_back(Property("light.intensity", "Intensity", FloatText(entity.light.intensity), "Light",
                                      QtPanelPropertyKind::Number, true));
    row.properties.push_back(Property("light.radius", "Radius", FloatText(entity.light.radius), "Light",
                                      QtPanelPropertyKind::Number, true));
  }
  if (entity.has_camera) {
    AddCameraComponentProperties(row.properties, entity.camera, true);
  }
  if (!entity.script.script.empty()) {
    row.properties.push_back(Property("script.path", "Script", entity.script.script, "Script",
                                      QtPanelPropertyKind::Asset, true));
  }
  if (!entity.animation.clip.empty()) {
    row.properties.push_back(Property("animation.clip", "Clip", entity.animation.clip, "Animation",
                                      QtPanelPropertyKind::Asset, true));
    row.properties.push_back(Property("animation.looping", "Looping", BoolText(entity.animation.looping), "Animation",
                                      QtPanelPropertyKind::Toggle, true));
    row.properties.push_back(Property("animation.duration_seconds", "Duration", FloatText(entity.animation.duration_seconds),
                                      "Animation", QtPanelPropertyKind::Number, true));
    row.properties.push_back(Property("animation.playback_speed", "Speed", FloatText(entity.animation.playback_speed),
                                      "Animation", QtPanelPropertyKind::Number, true));
    row.properties.push_back(Property("animation.rotation_degrees", "Rotation", Vec3Text(entity.animation.rotation_degrees),
                                      "Animation", QtPanelPropertyKind::Vector3, true));
  }
  row.properties.push_back(Property("physics.enabled", "Physics Enabled",
                                    BoolText(entity.has_physics_body && entity.physics_body.enabled),
                                    "Physics", QtPanelPropertyKind::Toggle, true));
  if (entity.has_physics_body) {
    row.properties.push_back(Property("physics.body_type", "Body Type", entity.physics_body.dynamic ? "dynamic" : entity.physics_body.body_type,
                                      "Physics", QtPanelPropertyKind::Text, true));
    row.properties.push_back(Property("physics.shape", "Shape", entity.physics_body.shape,
                                      "Physics", QtPanelPropertyKind::Text, true));
    row.properties.push_back(Property("physics.mass", "Mass", FloatText(entity.physics_body.mass),
                                      "Physics", QtPanelPropertyKind::Number, true));
    row.properties.push_back(Property("physics.friction", "Friction", FloatText(entity.physics_body.friction),
                                      "Physics", QtPanelPropertyKind::Number, true));
    row.properties.push_back(Property("physics.restitution", "Restitution", FloatText(entity.physics_body.restitution),
                                      "Physics", QtPanelPropertyKind::Number, true));
    row.properties.push_back(Property("physics.trigger", "Trigger", BoolText(entity.physics_body.trigger),
                                      "Physics", QtPanelPropertyKind::Toggle, true));
  }
}

std::string MaterialLabel(vkpt::core::StableId id, std::string_view name) {
  if (!name.empty()) {
    return std::string(name);
  }
  return "Material " + IdText(id);
}

void MarkUnavailable(QtPanelModel& model, std::string message) {
  model.available = false;
  model.empty_message = std::move(message);
  model.summary = model.empty_message;
}

QtPanelModel MakeModel(std::string panel_id, std::string title) {
  QtPanelModel model;
  model.panel_id = std::move(panel_id);
  model.title = std::move(title);
  return model;
}

}  // namespace

std::string_view ToString(QtPanelPropertyKind kind) {
  switch (kind) {
    case QtPanelPropertyKind::Text:
      return "text";
    case QtPanelPropertyKind::Number:
      return "number";
    case QtPanelPropertyKind::Toggle:
      return "toggle";
    case QtPanelPropertyKind::Vector3:
      return "vector3";
    case QtPanelPropertyKind::Color:
      return "color";
    case QtPanelPropertyKind::Asset:
      return "asset";
    case QtPanelPropertyKind::Entity:
      return "entity";
    case QtPanelPropertyKind::Enum:
      return "enum";
    case QtPanelPropertyKind::Command:
      return "command";
  }
  return "text";
}

std::vector<std::string> BuildDefaultQtPanelIds() {
  return {
    "scene_tree",
    "inspector",
    "materials",
    "lights",
    "camera",
    "render_settings",
    "benchmark_panel",
    "diagnostics",
    "performance",
    "debug_views",
    "asset_browser",
    "timeline",
    "script_panel",
    "physics",
  };
}

QtPanelModel BuildSceneGraphPanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("scene_tree", "Scene Graph");

  if (context.document != nullptr) {
    AddProperty(model, "scene.name", "Scene", context.document->metadata.scene_name.empty()
                    ? "untitled"
                    : context.document->metadata.scene_name);
    AddProperty(model, "scene.entities", "Entities", std::to_string(context.document->entities.size()), {},
                QtPanelPropertyKind::Number);
    AddProperty(model, "scene.geometry", "Geometry", std::to_string(context.document->geometry.size()), {},
                QtPanelPropertyKind::Number);
    AddProperty(model, "scene.materials", "Materials", std::to_string(context.document->materials.size()), {},
                QtPanelPropertyKind::Number);

    std::unordered_map<vkpt::core::StableId, std::vector<const vkpt::scene::SceneEntityDefinition*>> children;
    std::unordered_set<vkpt::core::StableId> ids;
    for (const auto& entity : context.document->entities) {
      ids.insert(entity.id);
      children[entity.hierarchy.parent].push_back(&entity);
    }

    std::unordered_set<vkpt::core::StableId> visited;
    auto append_entity = [&](auto&& self,
                             const vkpt::scene::SceneEntityDefinition& entity,
                             std::uint32_t depth) -> void {
      if (!visited.insert(entity.id).second) {
        return;
      }
      auto row = Row("entity." + IdText(entity.id), EntityName(entity), EntityComponentSummary(entity),
                     entity.has_camera ? "camera" : (entity.has_light ? "light" : (entity.has_mesh ? "mesh" : "entity")),
                     depth);
      row.selected = IsSelected(context, entity.id);
      row.expanded = true;
      AddEntityProperties(row, entity);
      model.rows.push_back(std::move(row));
      const auto child_it = children.find(entity.id);
      if (child_it != children.end()) {
        for (const auto* child : child_it->second) {
          self(self, *child, depth + 1);
        }
      }
    };

    for (const auto* root : children[0]) {
      append_entity(append_entity, *root, 0);
    }
    for (const auto& entity : context.document->entities) {
      if (!visited.contains(entity.id)) {
        append_entity(append_entity, entity, ids.contains(entity.hierarchy.parent) ? 1u : 0u);
      }
    }

    model.summary = std::to_string(model.rows.size()) + " entities";
    return model;
  }

  if (context.render_proxy != nullptr) {
    AddProperty(model, "render.renderables", "Renderables", std::to_string(context.render_proxy->renderables.size()), {},
                QtPanelPropertyKind::Number);
    AddProperty(model, "render.lights", "Lights", std::to_string(context.render_proxy->lights.size()), {},
                QtPanelPropertyKind::Number);
    for (const auto& renderable : context.render_proxy->renderables) {
      auto row = Row("renderable." + IdText(renderable.entity_id), "Renderable " + IdText(renderable.entity_id),
                     "geometry " + IdText(renderable.geometry_id) + ", material " + IdText(renderable.material_id), "mesh");
      row.selected = IsSelected(context, renderable.entity_id);
      row.properties.push_back(Property("geometry.id", "Geometry", IdText(renderable.geometry_id), "Mesh",
                                        QtPanelPropertyKind::Asset));
      row.properties.push_back(Property("material.id", "Material", IdText(renderable.material_id), "Mesh",
                                        QtPanelPropertyKind::Asset));
      row.properties.push_back(Property("translation", "Translation", Vec3Text(renderable.translation), "Transform",
                                        QtPanelPropertyKind::Vector3));
      row.properties.push_back(Property("scale", "Scale", Vec3Text(renderable.scale), "Transform",
                                        QtPanelPropertyKind::Vector3));
      model.rows.push_back(std::move(row));
    }
    for (const auto& light : context.render_proxy->lights) {
      auto row = Row("light." + IdText(light.entity_id), "Light " + IdText(light.entity_id), light.type, "light");
      row.selected = IsSelected(context, light.entity_id);
      row.properties.push_back(Property("position", "Position", Vec3Text(light.position), "Transform",
                                        QtPanelPropertyKind::Vector3));
      row.properties.push_back(Property("intensity", "Intensity", FloatText(light.intensity), "Light",
                                        QtPanelPropertyKind::Number));
      model.rows.push_back(std::move(row));
    }
    if (context.render_proxy->camera.has_value()) {
      const auto& camera = *context.render_proxy->camera;
      auto row = Row("camera." + IdText(camera.entity_id), "Camera " + IdText(camera.entity_id),
                     "fov " + FloatText(camera.fov), "camera");
      row.selected = IsSelected(context, camera.entity_id);
      model.rows.push_back(std::move(row));
    }
    model.summary = std::to_string(model.rows.size()) + " render objects";
    return model;
  }

  MarkUnavailable(model, "No scene document or render proxy is available.");
  return model;
}

QtPanelModel BuildInspectorPanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("inspector", "Inspector");
  const auto selected_count = context.selection == nullptr ? 0u : context.selection->selected_entity_ids.size();
  AddProperty(model, "selection.count", "Selected", std::to_string(selected_count), "Selection", QtPanelPropertyKind::Number);
  if (context.selection != nullptr) {
    AddProperty(model, "selection.primary", "Primary", IdText(context.selection->active_primary_entity),
                "Selection", QtPanelPropertyKind::Entity);
    AddProperty(model, "selection.bounds", "Bounds", BoundsText(context.selection->aggregate_bounds), "Selection");
  }
  if (selected_count == 0) {
    model.summary = "No selection";
    model.empty_message = "Select an entity to inspect transform, material, light, camera, script, and physics properties.";
    return model;
  }

  for (const auto entity_id : context.selection->selected_entity_ids) {
    if (const auto* entity = FindDocumentEntity(context, entity_id)) {
      auto row = Row("inspector.entity." + IdText(entity_id), EntityName(*entity), EntityComponentSummary(*entity), "entity");
      row.selected = true;
      AddEntityProperties(row, *entity);
      model.rows.push_back(std::move(row));
      continue;
    }

    auto row = Row("inspector.entity." + IdText(entity_id), "Entity " + IdText(entity_id), "runtime selection", "entity");
    row.selected = true;
    row.properties.push_back(Property("entity.id", "Stable ID", IdText(entity_id), "Entity", QtPanelPropertyKind::Entity));
    if (context.render_proxy != nullptr) {
      for (const auto& renderable : context.render_proxy->renderables) {
        if (renderable.entity_id == entity_id) {
          row.detail = "renderable";
          row.properties.push_back(Property("geometry.id", "Geometry", IdText(renderable.geometry_id), "Mesh",
                                            QtPanelPropertyKind::Asset));
          row.properties.push_back(Property("material.id", "Material", IdText(renderable.material_id), "Mesh",
                                            QtPanelPropertyKind::Asset));
          row.properties.push_back(Property("translation", "Translation", Vec3Text(renderable.translation), "Transform",
                                            QtPanelPropertyKind::Vector3));
          row.properties.push_back(Property("scale", "Scale", Vec3Text(renderable.scale), "Transform",
                                            QtPanelPropertyKind::Vector3));
          break;
        }
      }
    }
    model.rows.push_back(std::move(row));
  }
  model.summary = std::to_string(model.rows.size()) + " inspected";
  return model;
}

QtPanelModel BuildMaterialsPanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("materials", "Materials");
  if (context.document != nullptr) {
    AddProperty(model, "materials.count", "Materials", std::to_string(context.document->materials.size()), {},
                QtPanelPropertyKind::Number);
    for (const auto& material : context.document->materials) {
      auto row = Row("material." + IdText(material.id), MaterialLabel(material.id, material.name),
                     "roughness " + FloatText(material.roughness), "material");
      row.properties.push_back(Property("material.id", "Material ID", IdText(material.id), "Material",
                                        QtPanelPropertyKind::Asset));
      row.properties.push_back(Property("material.albedo", "Albedo", Vec3Text(material.albedo), "Material",
                                        QtPanelPropertyKind::Color, true));
      row.properties.push_back(Property("material.roughness", "Roughness", FloatText(material.roughness), "Material",
                                        QtPanelPropertyKind::Number, true));
      row.properties.push_back(Property("material.emission", "Emission", Vec3Text(material.emission), "Material",
                                        QtPanelPropertyKind::Color, true));
      row.properties.push_back(Property("material.emission_intensity", "Emission Intensity",
                                        FloatText(material.emission_intensity), "Material",
                                        QtPanelPropertyKind::Number, true));
      model.rows.push_back(std::move(row));
    }
  } else if (context.render_proxy != nullptr) {
    AddProperty(model, "materials.count", "Materials", std::to_string(context.render_proxy->materials.size()), {},
                QtPanelPropertyKind::Number);
    for (const auto& material : context.render_proxy->materials) {
      auto row = Row("material." + IdText(material.id), "Material " + IdText(material.id),
                     "roughness " + FloatText(material.roughness), "material");
      row.properties.push_back(Property("material.albedo", "Albedo", Vec3Text(material.albedo), "Material",
                                        QtPanelPropertyKind::Color));
      row.properties.push_back(Property("material.emission", "Emission", Vec3Text(material.emission), "Material",
                                        QtPanelPropertyKind::Color));
      row.properties.push_back(Property("material.emission_intensity", "Emission Intensity",
                                        FloatText(material.emission_intensity), "Material",
                                        QtPanelPropertyKind::Number));
      model.rows.push_back(std::move(row));
    }
  } else if (context.rt_scene != nullptr) {
    AddProperty(model, "materials.count", "Materials", std::to_string(context.rt_scene->materials.size()), {},
                QtPanelPropertyKind::Number);
    for (std::size_t i = 0; i < context.rt_scene->materials.size(); ++i) {
      const auto& material = context.rt_scene->materials[i];
      auto row = Row("rt_material." + std::to_string(i), "RT Material " + std::to_string(i),
                     material.is_emissive() ? "emissive" : "surface", "material");
      row.properties.push_back(Property("material.albedo", "Albedo", Vec3Text(material.albedo), "Material",
                                        QtPanelPropertyKind::Color));
      row.properties.push_back(Property("material.roughness", "Roughness", FloatText(material.roughness), "Material",
                                        QtPanelPropertyKind::Number));
      row.properties.push_back(Property("material.emissive", "Emissive", Vec3Text(material.emissive), "Material",
                                        QtPanelPropertyKind::Color));
      model.rows.push_back(std::move(row));
    }
  }
  model.summary = std::to_string(model.rows.size()) + " materials";
  if (model.rows.empty()) {
    model.empty_message = "No material data is available.";
  }
  return model;
}

QtPanelModel BuildLightsPanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("lights", "Lights");
  if (context.document != nullptr) {
    for (const auto& light : context.document->lights) {
      auto row = Row("light." + IdText(light.id), "Light " + IdText(light.id), light.light.type, "light");
      row.selected = IsSelected(context, light.id);
      row.properties.push_back(Property("light.type", "Type", light.light.type, "Light", QtPanelPropertyKind::Text, true));
      row.properties.push_back(Property("light.color", "Color", Vec3Text(light.light.color), "Light",
                                        QtPanelPropertyKind::Color, true));
      row.properties.push_back(Property("light.intensity", "Intensity", FloatText(light.light.intensity), "Light",
                                        QtPanelPropertyKind::Number, true));
      row.properties.push_back(Property("light.radius", "Radius", FloatText(light.light.radius), "Light",
                                        QtPanelPropertyKind::Number, true));
      model.rows.push_back(std::move(row));
    }
    for (const auto& entity : context.document->entities) {
      if (!entity.has_light) {
        continue;
      }
      auto row = Row("entity_light." + IdText(entity.id), EntityName(entity), entity.light.type, "light");
      row.selected = IsSelected(context, entity.id);
      row.properties.push_back(Property("entity.id", "Entity", IdText(entity.id), "Entity", QtPanelPropertyKind::Entity));
      row.properties.push_back(Property("light.color", "Color", Vec3Text(entity.light.color), "Light",
                                        QtPanelPropertyKind::Color, true));
      row.properties.push_back(Property("light.intensity", "Intensity", FloatText(entity.light.intensity), "Light",
                                        QtPanelPropertyKind::Number, true));
      model.rows.push_back(std::move(row));
    }
  } else if (context.render_proxy != nullptr) {
    for (const auto& light : context.render_proxy->lights) {
      auto row = Row("light." + IdText(light.entity_id), "Light " + IdText(light.entity_id), light.type, "light");
      row.selected = IsSelected(context, light.entity_id);
      row.properties.push_back(Property("light.position", "Position", Vec3Text(light.position), "Transform",
                                        QtPanelPropertyKind::Vector3));
      row.properties.push_back(Property("light.color", "Color", Vec3Text(light.color), "Light",
                                        QtPanelPropertyKind::Color));
      row.properties.push_back(Property("light.intensity", "Intensity", FloatText(light.intensity), "Light",
                                        QtPanelPropertyKind::Number));
      model.rows.push_back(std::move(row));
    }
  } else if (context.rt_scene != nullptr) {
    for (std::size_t i = 0; i < context.rt_scene->lights.size(); ++i) {
      const auto& light = context.rt_scene->lights[i];
      auto row = Row("rt_light." + std::to_string(i), "RT Light " + std::to_string(i),
                     "intensity " + FloatText(light.intensity), "light");
      row.properties.push_back(Property("light.position", "Position", Vec3Text(light.position), "Transform",
                                        QtPanelPropertyKind::Vector3));
      row.properties.push_back(Property("light.color", "Color", Vec3Text(light.color), "Light",
                                        QtPanelPropertyKind::Color));
      row.properties.push_back(Property("light.radius", "Radius", FloatText(light.radius), "Light",
                                        QtPanelPropertyKind::Number));
      model.rows.push_back(std::move(row));
    }
  }
  model.summary = std::to_string(model.rows.size()) + " lights";
  if (model.rows.empty()) {
    model.empty_message = "No lights are available.";
  }
  return model;
}

QtPanelModel BuildCameraPanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("camera", "Camera");
  if (context.runtime != nullptr) {
    AddProperty(model, "runtime.active_camera", "Active Camera", context.runtime->active_camera.empty()
                    ? "default"
                    : context.runtime->active_camera);
  }
  if (context.document != nullptr) {
    for (const auto& camera : context.document->cameras) {
      auto row = Row("camera." + IdText(camera.id), "Camera " + IdText(camera.id),
                     "fov " + FloatText(camera.camera.fov), "camera");
      row.selected = IsSelected(context, camera.id);
      AddCameraComponentProperties(row.properties, camera.camera, true);
      model.rows.push_back(std::move(row));
    }
    for (const auto& entity : context.document->entities) {
      if (!entity.has_camera) {
        continue;
      }
      auto row = Row("entity_camera." + IdText(entity.id), EntityName(entity),
                     "fov " + FloatText(entity.camera.fov), "camera");
      row.selected = IsSelected(context, entity.id);
      row.properties.push_back(Property("entity.id", "Entity", IdText(entity.id), "Entity", QtPanelPropertyKind::Entity));
      AddCameraComponentProperties(row.properties, entity.camera, true);
      model.rows.push_back(std::move(row));
    }
  }
  if (context.render_proxy != nullptr && context.render_proxy->camera.has_value()) {
    const auto& camera = *context.render_proxy->camera;
    auto row = Row("render_camera." + IdText(camera.entity_id), "Render Camera " + IdText(camera.entity_id),
                   "fov " + FloatText(camera.fov), "camera");
    row.selected = IsSelected(context, camera.entity_id);
    row.properties.push_back(Property("camera.position", "Position", Vec3Text(camera.position), "Transform",
                                      QtPanelPropertyKind::Vector3));
    row.properties.push_back(Property("camera.fov", "FOV", FloatText(camera.fov), "Camera",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("camera.near", "Near", FloatText(camera.near_plane), "Camera",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("camera.far", "Far", FloatText(camera.far_plane), "Camera",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("camera.focal_length_mm", "Focal Length", FloatText(camera.focal_length_mm), "Lens",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("camera.aperture_radius", "Aperture Radius", FloatText(camera.aperture_radius), "Lens",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("camera.focus_distance", "Focus Distance", FloatText(camera.focus_distance), "Lens",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("camera.exposure_compensation", "Exposure Compensation",
                                      FloatText(camera.exposure_compensation), "Exposure",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("camera.white_balance_kelvin", "White Balance",
                                      FloatText(camera.white_balance_kelvin), "Color",
                                      QtPanelPropertyKind::Number));
    model.rows.push_back(std::move(row));
  }
  if (context.rt_scene != nullptr) {
    auto row = Row("rt_camera", "RT Camera", "fov " + FloatText(context.rt_scene->camera_fov_deg), "camera");
    row.properties.push_back(Property("camera.position", "Position", Vec3Text(context.rt_scene->camera_position),
                                      "Transform", QtPanelPropertyKind::Vector3, true));
    row.properties.push_back(Property("camera.target", "Target", Vec3Text(context.rt_scene->camera_target),
                                      "Transform", QtPanelPropertyKind::Vector3, true));
    row.properties.push_back(Property("camera.up", "Up", Vec3Text(context.rt_scene->camera_up),
                                      "Transform", QtPanelPropertyKind::Vector3, true));
    row.properties.push_back(Property("camera.fov", "FOV", FloatText(context.rt_scene->camera_fov_deg), "Camera",
                                      QtPanelPropertyKind::Number, true));
    row.properties.push_back(Property("camera.focal_length_mm", "Focal Length",
                                      FloatText(context.rt_scene->camera_focal_length_mm), "Lens",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("camera.aperture_radius", "Aperture Radius",
                                      FloatText(context.rt_scene->camera_aperture_radius), "Lens",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("camera.focus_distance", "Focus Distance",
                                      FloatText(context.rt_scene->camera_focus_distance), "Lens",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("camera.exposure_compensation", "Exposure Compensation",
                                      FloatText(context.rt_scene->camera_exposure_compensation), "Exposure",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("camera.white_balance_kelvin", "White Balance",
                                      FloatText(context.rt_scene->camera_white_balance_kelvin), "Color",
                                      QtPanelPropertyKind::Number));
    model.rows.push_back(std::move(row));
  }
  model.summary = model.rows.empty() ? "No camera data" : std::to_string(model.rows.size()) + " camera rows";
  if (model.rows.empty()) {
    model.empty_message = "No camera data is available.";
  }
  return model;
}

QtPanelModel BuildRenderSettingsPanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("render_settings", "Render Settings");
  if (context.runtime != nullptr) {
    AddProperty(model, "runtime.backend", "Backend", context.runtime->active_renderer_backend, "Runtime");
    AddProperty(model, "runtime.path", "Renderer Path", context.runtime->active_renderer_path, "Runtime");
    AddProperty(model, "runtime.spp_accumulated", "Accumulated SPP", std::to_string(context.runtime->spp_accumulated),
                "Runtime", QtPanelPropertyKind::Number);
  }
  if (context.render_settings == nullptr) {
    model.empty_message = "No render settings object is available.";
    model.summary = model.empty_message;
    return model;
  }
  const auto& settings = *context.render_settings;
  AddProperty(model, "render.resolution", "Resolution", ResolutionText(settings.width, settings.height), "Film");
  AddProperty(model, "render.spp", "Samples Per Pixel", std::to_string(settings.spp), "Integrator",
              QtPanelPropertyKind::Number, true);
  AddProperty(model, "render.max_depth", "Max Depth", std::to_string(settings.max_depth), "Integrator",
              QtPanelPropertyKind::Number, true);
  AddProperty(model, "render.seed", "Seed", std::to_string(settings.seed), "Sampling",
              QtPanelPropertyKind::Number, true);
  AddProperty(model, "render.deterministic", "Deterministic", BoolText(settings.deterministic), "Sampling",
              QtPanelPropertyKind::Toggle, true);
  AddProperty(model, "render.nee", "Next Event Estimation", BoolText(settings.enable_nee), "Integrator",
              QtPanelPropertyKind::Toggle, true);
  AddProperty(model, "render.mis", "MIS", BoolText(settings.enable_mis), "Integrator",
              QtPanelPropertyKind::Toggle, true);
  AddProperty(model, "render.rr_start", "RR Start Depth", std::to_string(settings.russian_roulette_start_depth),
              "Integrator", QtPanelPropertyKind::Number, true);
  AddProperty(model, "render.rr_min", "RR Min Survival", FloatText(settings.russian_roulette_min_survival),
              "Integrator", QtPanelPropertyKind::Number, true);
  AddProperty(model, "render.rr_max", "RR Max Survival", FloatText(settings.russian_roulette_max_survival),
              "Integrator", QtPanelPropertyKind::Number, true);
  AddProperty(model, "camera.aperture", "Aperture Radius", FloatText(settings.camera_aperture_radius), "Camera",
              QtPanelPropertyKind::Number, true);
  AddProperty(model, "camera.focus", "Focus Distance", FloatText(settings.camera_focus_distance), "Camera",
              QtPanelPropertyKind::Number, true);
  AddProperty(model, "film.exposure", "Exposure", FloatText(settings.film_resolve.exposure), "Resolve",
              QtPanelPropertyKind::Number, true);
  std::string toneMap = "linear";
  switch (settings.film_resolve.tone_map) {
    case vkpt::pathtracer::ToneMapMode::Reinhard: toneMap = "reinhard"; break;
    case vkpt::pathtracer::ToneMapMode::FilmicApprox: toneMap = "filmic_approx"; break;
    case vkpt::pathtracer::ToneMapMode::AcesApprox: toneMap = "aces_approx"; break;
    case vkpt::pathtracer::ToneMapMode::Linear:
    default: break;
  }
  AddProperty(model, "film.tone_map", "Tone Mapper", toneMap, "Resolve",
              QtPanelPropertyKind::Enum, true);
  AddProperty(model, "film.output_transform", "Output Transform",
              settings.film_resolve.output_transform == vkpt::pathtracer::OutputTransformMode::Linear
                  ? "linear"
                  : "gamma",
              "Resolve", QtPanelPropertyKind::Enum, true);
  AddProperty(model, "film.gamma", "Gamma", FloatText(settings.film_resolve.gamma), "Resolve",
              QtPanelPropertyKind::Number, true);
  AddProperty(model, "film.clamp", "Clamp Output", BoolText(settings.film_resolve.clamp_output), "Resolve",
              QtPanelPropertyKind::Toggle, true);
  model.summary = ResolutionText(settings.width, settings.height) + ", " + std::to_string(settings.spp) + " spp";
  return model;
}

QtPanelModel BuildBenchmarkPanelTextModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("benchmark_panel", "Benchmark");
  if (context.benchmark == nullptr) {
    model.summary = "No benchmark model";
    model.empty_message = "No benchmark descriptor has been supplied.";
    return model;
  }
  const auto& benchmark = *context.benchmark;
  AddProperty(model, "benchmark.can_run", "Can Run", BoolText(benchmark.can_run), "State", QtPanelPropertyKind::Toggle);
  AddProperty(model, "benchmark.can_cancel", "Can Cancel", BoolText(benchmark.can_cancel), "State", QtPanelPropertyKind::Toggle);
  AddProperty(model, "benchmark.scene", "Scene", benchmark.run_desc.scene_path, "Descriptor", QtPanelPropertyKind::Asset);
  AddProperty(model, "benchmark.backend", "Backend", benchmark.run_desc.backend, "Descriptor");
  AddProperty(model, "benchmark.renderer", "Renderer", benchmark.run_desc.renderer_path, "Descriptor");
  AddProperty(model, "benchmark.resolution", "Resolution",
              ResolutionText(benchmark.run_desc.resolution.width, benchmark.run_desc.resolution.height), "Descriptor");
  AddProperty(model, "benchmark.spp", "SPP", std::to_string(benchmark.run_desc.samples_per_pixel), "Descriptor",
              QtPanelPropertyKind::Number);
  AddProperty(model, "benchmark.max_depth", "Max Depth", std::to_string(benchmark.run_desc.max_depth), "Descriptor",
              QtPanelPropertyKind::Number);
  AddProperty(model, "benchmark.duration", "Duration", FloatText(benchmark.run_desc.duration), "Descriptor",
              QtPanelPropertyKind::Number);
  AddProperty(model, "benchmark.summary", "Result", benchmark.result_summary, "Result");
  AddProperty(model, "benchmark.score", "Score", FloatText(benchmark.score.normalized_score), "Result",
              QtPanelPropertyKind::Number);
  AddProperty(model, "benchmark.samples_per_second", "Samples/s", FloatText(benchmark.raw_metrics.samples_per_second),
              "Raw Metrics", QtPanelPropertyKind::Number);
  AddProperty(model, "benchmark.paths_per_second", "Paths/s", FloatText(benchmark.raw_metrics.paths_per_second),
              "Raw Metrics", QtPanelPropertyKind::Number);
  AddProperty(model, "benchmark.frame_ms", "Frame ms", FloatText(benchmark.raw_metrics.frame_ms),
              "Raw Metrics", QtPanelPropertyKind::Number);

  for (const auto& action : benchmark.calibration_actions) {
    auto row = Row("benchmark.action." + action.id, action.label, action.supported ? "supported" : action.unavailable_reason,
                   "command");
    row.warning = !action.supported;
    row.properties.push_back(Property("command.id", "Command", action.id, "Action", QtPanelPropertyKind::Command));
    row.properties.push_back(Property("command.backend", "Backend", action.backend, "Action"));
    row.properties.push_back(Property("command.renderer", "Renderer", action.renderer_path, "Action"));
    model.rows.push_back(std::move(row));
  }
  for (const auto& history : benchmark.history) {
    auto row = Row("benchmark.history." + history.timestamp_utc, history.scene,
                   history.backend + " " + history.renderer_path, "history");
    row.properties.push_back(Property("score", "Score", FloatText(history.score.normalized_score), "History",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("artifact", "Artifact", history.artifact_path, "History", QtPanelPropertyKind::Asset));
    row.properties.push_back(Property("timestamp", "Timestamp", history.timestamp_utc, "History"));
    row.warning = !history.regression_marker.empty();
    model.rows.push_back(std::move(row));
  }
  for (const auto& line : context.benchmark_history) {
    model.rows.push_back(Row("benchmark.external_history." + std::to_string(model.rows.size()), line, {}, "history"));
  }
  model.summary = benchmark.result_summary.empty() ? "Benchmark descriptor ready" : benchmark.result_summary;
  return model;
}

QtPanelModel BuildDiagnosticsPanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("diagnostics", "Diagnostics");
  if (context.runtime != nullptr) {
    AddProperty(model, "status.message", "Status", context.runtime->status_message, "Runtime");
    AddProperty(model, "status.warning", "Last Warning/Error", context.runtime->last_warning_or_error, "Runtime");
    AddProperty(model, "status.jobs", "Background Jobs", std::to_string(context.runtime->background_job_count),
                "Runtime", QtPanelPropertyKind::Number);
  }
  for (const auto& gate : context.release_gates) {
    auto row = Row("gate." + gate.id, gate.label, gate.passed ? "passed" : (gate.deferred ? "deferred" : "pending"),
                   gate.required ? "required" : "optional");
    row.warning = gate.required && !gate.passed && !gate.deferred;
    row.properties.push_back(Property("gate.id", "Gate", gate.id, "Release Gate"));
    row.properties.push_back(Property("gate.required", "Required", BoolText(gate.required), "Release Gate",
                                      QtPanelPropertyKind::Toggle));
    row.properties.push_back(Property("gate.passed", "Passed", BoolText(gate.passed), "Release Gate",
                                      QtPanelPropertyKind::Toggle));
    row.properties.push_back(Property("gate.evidence", "Evidence", gate.evidence, "Release Gate"));
    if (!gate.deferred_reason.empty()) {
      row.properties.push_back(Property("gate.deferred_reason", "Deferred Reason", gate.deferred_reason, "Release Gate"));
    }
    model.rows.push_back(std::move(row));
  }
  for (const auto& line : context.diagnostics_log) {
    auto row = Row("diagnostic." + std::to_string(model.rows.size()), line, {}, "log");
    row.warning = line.find("error") != std::string::npos || line.find("warning") != std::string::npos;
    model.rows.push_back(std::move(row));
  }
  model.summary = std::to_string(model.rows.size()) + " diagnostic rows";
  if (model.rows.empty() && model.properties.empty()) {
    model.empty_message = "No diagnostic data is available.";
  }
  return model;
}

QtPanelModel BuildPerformancePanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("performance", "Performance");
  if (context.runtime != nullptr) {
    AddProperty(model, "runtime.fps", "FPS", FloatText(context.runtime->fps), "Frame", QtPanelPropertyKind::Number);
    AddProperty(model, "runtime.frame_ms", "Frame ms", FloatText(context.runtime->frame_ms), "Frame",
                QtPanelPropertyKind::Number);
    AddProperty(model, "runtime.spp", "Accumulated SPP", std::to_string(context.runtime->spp_accumulated), "Frame",
                QtPanelPropertyKind::Number);
    AddProperty(model, "runtime.jobs", "Background Jobs", std::to_string(context.runtime->background_job_count), "Jobs",
                QtPanelPropertyKind::Number);
  }
  if (context.status_bar != nullptr) {
    AddProperty(model, "status.score", "Normalized Score", FloatText(context.status_bar->normalized_score), "Score",
                QtPanelPropertyKind::Number);
  }
  if (context.benchmark != nullptr) {
    const auto& raw = context.benchmark->raw_metrics;
    AddProperty(model, "benchmark.cpu_ms", "CPU ms", FloatText(raw.cpu_ms), "Benchmark", QtPanelPropertyKind::Number);
    AddProperty(model, "benchmark.gpu_ms", "GPU ms", FloatText(raw.gpu_ms), "Benchmark", QtPanelPropertyKind::Number);
    AddProperty(model, "benchmark.samples_per_second", "Samples/s", FloatText(raw.samples_per_second), "Benchmark",
                QtPanelPropertyKind::Number);
    AddProperty(model, "benchmark.paths_per_second", "Paths/s", FloatText(raw.paths_per_second), "Benchmark",
                QtPanelPropertyKind::Number);
    AddProperty(model, "benchmark.memory", "Memory Estimate", std::to_string(raw.memory_estimate_bytes), "Benchmark",
                QtPanelPropertyKind::Number);
    AddProperty(model, "workload.cost", "Workload Cost", FloatText(context.benchmark->workload.normalized_cost_units),
                "Workload", QtPanelPropertyKind::Number);
    for (const auto& driver : context.benchmark->workload.cost_drivers) {
      model.rows.push_back(Row("workload.driver." + std::to_string(model.rows.size()), driver, {}, "metric"));
    }
  }
  model.summary = context.runtime != nullptr ? FloatText(context.runtime->frame_ms) + " ms" : "Performance data";
  return model;
}

QtPanelModel BuildDebugViewsPanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("debug_views", "Debug Views");
  if (context.runtime != nullptr) {
    AddProperty(model, "debug.selected", "Selected", context.runtime->selected_debug_view, "Debug View");
    AddProperty(model, "debug.channel", "Active Channel", context.runtime->active_debug_channel, "Debug View");
  }
  for (const auto& descriptor : vkpt::render::GetDebugViewRegistry()) {
    auto row = Row("debug_view." + descriptor.view_id, descriptor.display_name,
                   descriptor.available ? "available" : "unavailable", "debug");
    row.selected = context.runtime != nullptr && context.runtime->selected_debug_view == descriptor.view_id;
    row.warning = !descriptor.available;
    row.properties.push_back(Property("debug.id", "ID", descriptor.view_id, "Debug View"));
    row.properties.push_back(Property("debug.channel", "Channel", vkpt::render::ToString(descriptor.channel), "Debug View"));
    row.properties.push_back(Property("debug.requirement", "Backend Requirement",
                                      vkpt::render::ToString(descriptor.backend_requirement), "Debug View"));
    row.properties.push_back(Property("debug.command", "Command", descriptor.command_id, "Debug View",
                                      QtPanelPropertyKind::Command));
    row.properties.push_back(Property("debug.reset", "Resets Accumulation",
                                      BoolText(descriptor.accumulation_reset_required), "Debug View",
                                      QtPanelPropertyKind::Toggle));
    row.properties.push_back(Property("debug.notes", "Notes", descriptor.notes, "Debug View"));
    model.rows.push_back(std::move(row));
  }
  model.summary = std::to_string(model.rows.size()) + " debug views";
  return model;
}

QtPanelModel BuildAssetBrowserPanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("asset_browser", "Asset Browser");
  if (context.document != nullptr) {
    for (const auto& asset : context.document->assets) {
      auto row = Row("asset." + IdText(asset.id), asset.uri.empty() ? "Asset " + IdText(asset.id) : asset.uri,
                     asset.type, "asset");
      row.properties.push_back(Property("asset.id", "Asset ID", IdText(asset.id), "Asset", QtPanelPropertyKind::Asset));
      row.properties.push_back(Property("asset.type", "Type", asset.type, "Asset"));
      row.properties.push_back(Property("asset.uri", "URI", asset.uri, "Asset", QtPanelPropertyKind::Asset));
      model.rows.push_back(std::move(row));
    }
    for (const auto& geometry : context.document->geometry) {
      auto row = Row("geometry." + IdText(geometry.id), "Geometry " + IdText(geometry.id), geometry.primitive, "mesh");
      row.properties.push_back(Property("geometry.vertices", "Vertices", std::to_string(geometry.vertices.size()),
                                        "Geometry", QtPanelPropertyKind::Number));
      row.properties.push_back(Property("geometry.indices", "Indices", std::to_string(geometry.indices.size()),
                                        "Geometry", QtPanelPropertyKind::Number));
      row.properties.push_back(Property("geometry.material", "Material", IdText(geometry.material_id), "Geometry",
                                        QtPanelPropertyKind::Asset));
      model.rows.push_back(std::move(row));
    }
  }
  for (const auto& card : context.asset_cards) {
    auto row = Row("asset_card." + card.asset_id, card.display_name, card.category + " " + card.status, "asset");
    row.selected = card.selected;
    row.warning = card.missing;
    row.properties.push_back(Property("asset.path", "Path", card.path, "Asset", QtPanelPropertyKind::Asset));
    row.properties.push_back(Property("asset.status", "Status", card.status, "Asset"));
    row.properties.push_back(Property("asset.thumbnail", "Thumbnail", card.thumbnail_hint, "Asset"));
    model.rows.push_back(std::move(row));
  }
  if (context.snapshot != nullptr) {
    for (const auto& ref : context.snapshot->asset_refs) {
      model.rows.push_back(Row("asset_ref." + ref, ref, "snapshot reference", "asset"));
    }
  }
  model.summary = std::to_string(model.rows.size()) + " assets";
  if (model.rows.empty()) {
    model.empty_message = "No assets are available.";
  }
  return model;
}

QtPanelModel BuildTimelinePanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("timeline", "Timeline");
  AddProperty(model, "timeline.frame", "Frame", std::to_string(context.current_frame), "Playback",
              QtPanelPropertyKind::Number);
  AddProperty(model, "timeline.delta_seconds", "Delta Seconds", FloatText(context.delta_seconds), "Playback",
              QtPanelPropertyKind::Number);
  AddProperty(model, "timeline.playing", "Playing", BoolText(context.timeline_playing), "Playback",
              QtPanelPropertyKind::Toggle, true);
  if (context.document != nullptr) {
    for (const auto& entity : context.document->entities) {
      if (entity.animation.clip.empty()) {
        continue;
      }
      auto row = Row("animation." + IdText(entity.id), EntityName(entity), entity.animation.clip, "animation");
      row.selected = IsSelected(context, entity.id);
      row.properties.push_back(Property("animation.clip", "Clip", entity.animation.clip, "Animation",
                                        QtPanelPropertyKind::Asset, true));
      row.properties.push_back(Property("animation.looping", "Looping", BoolText(entity.animation.looping), "Animation",
                                        QtPanelPropertyKind::Toggle, true));
      row.properties.push_back(Property("animation.duration_seconds", "Duration", FloatText(entity.animation.duration_seconds),
                                        "Animation", QtPanelPropertyKind::Number, true));
      row.properties.push_back(Property("animation.playback_speed", "Speed", FloatText(entity.animation.playback_speed),
                                        "Animation", QtPanelPropertyKind::Number, true));
      row.properties.push_back(Property("animation.rotation_degrees", "Rotation", Vec3Text(entity.animation.rotation_degrees),
                                        "Animation", QtPanelPropertyKind::Vector3, true));
      model.rows.push_back(std::move(row));
    }
  }
  if (context.benchmark != nullptr) {
    const auto& desc = context.benchmark->run_desc;
    auto row = Row("timeline.benchmark", "Benchmark Run", desc.scene_path, "benchmark");
    row.properties.push_back(Property("benchmark.duration", "Duration", FloatText(desc.duration), "Benchmark",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("benchmark.warmup", "Warmup Frames", std::to_string(desc.warmup_frames),
                                      "Benchmark", QtPanelPropertyKind::Number));
    model.rows.push_back(std::move(row));
  }
  model.summary = model.rows.empty() ? "No animation tracks" : std::to_string(model.rows.size()) + " timeline tracks";
  return model;
}

QtPanelModel BuildScriptingPanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("script_panel", "Scripting");
  if (context.document != nullptr) {
    for (const auto& entity : context.document->entities) {
      if (entity.script.script.empty()) {
        continue;
      }
      auto row = Row("script." + IdText(entity.id), EntityName(entity), entity.script.script, "script");
      row.selected = IsSelected(context, entity.id);
      row.properties.push_back(Property("script.path", "Script", entity.script.script, "Script",
                                        QtPanelPropertyKind::Asset, true));
      row.properties.push_back(Property("entity.id", "Entity", IdText(entity.id), "Entity", QtPanelPropertyKind::Entity));
      model.rows.push_back(std::move(row));
    }
  }
  for (const auto& hook : context.script_hooks) {
    auto row = Row("script_hook." + hook.hook_name, hook.hook_name,
                   hook.implemented ? "implemented" : "not implemented", "hook");
    row.warning = !hook.last_error.empty();
    row.properties.push_back(Property("hook.implemented", "Implemented", BoolText(hook.implemented), "Hook",
                                      QtPanelPropertyKind::Toggle));
    row.properties.push_back(Property("hook.last_frame", "Last Fired Frame", std::to_string(hook.last_fired_frame), "Hook",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("hook.last_error", "Last Error", hook.last_error, "Hook"));
    model.rows.push_back(std::move(row));
  }
  model.summary = std::to_string(model.rows.size()) + " script rows";
  if (model.rows.empty()) {
    model.empty_message = "No scripts or lifecycle hooks are available.";
  }
  return model;
}

QtPanelModel BuildPhysicsPanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("physics", "Physics");
  std::size_t enabled = 0;
  std::size_t authored = 0;
  if (context.document != nullptr) {
    for (const auto& entity : context.document->entities) {
      const bool has_physics = entity.has_physics_body;
      const bool physics_enabled = has_physics && entity.physics_body.enabled;
      if (has_physics) {
        ++authored;
      }
      if (physics_enabled) {
        ++enabled;
      }
      auto row = Row("physics." + IdText(entity.id), EntityName(entity),
                     physics_enabled ? (entity.physics_body.dynamic ? "dynamic" : "static") : "off",
                     "physics");
      row.selected = IsSelected(context, entity.id);
      row.properties.push_back(Property("physics.enabled", "Physics Enabled", BoolText(physics_enabled), "Physics",
                                        QtPanelPropertyKind::Toggle, true));
      if (has_physics) {
        row.properties.push_back(Property("physics.mass", "Mass", FloatText(entity.physics_body.mass), "Physics",
                                          QtPanelPropertyKind::Number, true));
        row.properties.push_back(Property("physics.dynamic", "Dynamic", BoolText(entity.physics_body.dynamic), "Physics",
                                          QtPanelPropertyKind::Toggle, true));
        row.properties.push_back(Property("physics.shape", "Shape", entity.physics_body.shape, "Physics",
                                          QtPanelPropertyKind::Text, true));
        row.properties.push_back(Property("physics.trigger", "Trigger", BoolText(entity.physics_body.trigger), "Physics",
                                          QtPanelPropertyKind::Toggle, true));
      }
      model.rows.push_back(std::move(row));
    }
  } else if (context.world != nullptr) {
    for (const auto entity_id : context.world->all_entities()) {
      const auto* entity = context.world->get_entity(entity_id);
      if (entity == nullptr) {
        continue;
      }
      const auto* physics = entity->physics_body ? &*entity->physics_body : nullptr;
      const bool physics_enabled = physics != nullptr && physics->enabled;
      if (physics != nullptr) {
        ++authored;
      }
      if (physics_enabled) {
        ++enabled;
      }
      auto row = Row("physics." + IdText(entity_id), entity->identity.name.empty()
                         ? "Entity " + IdText(entity_id)
                         : entity->identity.name,
                     physics_enabled ? (physics->dynamic ? "dynamic" : "static") : "off", "physics");
      row.selected = IsSelected(context, entity_id);
      row.properties.push_back(Property("physics.enabled", "Physics Enabled", BoolText(physics_enabled), "Physics",
                                        QtPanelPropertyKind::Toggle, true));
      if (physics != nullptr) {
        row.properties.push_back(Property("physics.mass", "Mass", FloatText(physics->mass), "Physics",
                                          QtPanelPropertyKind::Number, true));
        row.properties.push_back(Property("physics.dynamic", "Dynamic", BoolText(physics->dynamic), "Physics",
                                          QtPanelPropertyKind::Toggle, true));
        row.properties.push_back(Property("physics.shape", "Shape", physics->shape, "Physics",
                                          QtPanelPropertyKind::Text, true));
      }
      model.rows.push_back(std::move(row));
    }
  }
  AddProperty(model, "physics.rows", "Entities", std::to_string(model.rows.size()), "Physics",
              QtPanelPropertyKind::Number);
  AddProperty(model, "physics.authored", "Authored Bodies", std::to_string(authored), "Physics",
              QtPanelPropertyKind::Number);
  AddProperty(model, "physics.enabled_count", "Enabled Bodies", std::to_string(enabled), "Physics",
              QtPanelPropertyKind::Number);
  model.summary = std::to_string(enabled) + " enabled physics bodies / " + std::to_string(model.rows.size()) + " entities";
  if (model.rows.empty()) {
    model.empty_message = "No entities are available.";
  }
  return model;
}

QtPanelModel BuildQtPanelModel(std::string_view panel_id, const QtPanelBuildContext& context) {
  if (panel_id == "scene_tree" || panel_id == "scene_graph") {
    return BuildSceneGraphPanelModel(context);
  }
  if (panel_id == "inspector") {
    return BuildInspectorPanelModel(context);
  }
  if (panel_id == "materials" || panel_id == "material_editor") {
    return BuildMaterialsPanelModel(context);
  }
  if (panel_id == "lights") {
    return BuildLightsPanelModel(context);
  }
  if (panel_id == "camera") {
    return BuildCameraPanelModel(context);
  }
  if (panel_id == "render_settings") {
    return BuildRenderSettingsPanelModel(context);
  }
  if (panel_id == "benchmark_panel" || panel_id == "benchmark") {
    return BuildBenchmarkPanelTextModel(context);
  }
  if (panel_id == "diagnostics" || panel_id == "console") {
    return BuildDiagnosticsPanelModel(context);
  }
  if (panel_id == "performance") {
    return BuildPerformancePanelModel(context);
  }
  if (panel_id == "debug_views") {
    return BuildDebugViewsPanelModel(context);
  }
  if (panel_id == "asset_browser") {
    return BuildAssetBrowserPanelModel(context);
  }
  if (panel_id == "timeline") {
    return BuildTimelinePanelModel(context);
  }
  if (panel_id == "script_panel" || panel_id == "scripting") {
    return BuildScriptingPanelModel(context);
  }
  if (panel_id == "physics") {
    return BuildPhysicsPanelModel(context);
  }

  auto model = MakeModel(std::string(panel_id), "Unknown Panel");
  MarkUnavailable(model, "No builder is registered for panel '" + std::string(panel_id) + "'.");
  return model;
}

std::vector<QtPanelModel> BuildAllQtPanelModels(const QtPanelBuildContext& context) {
  std::vector<QtPanelModel> models;
  const auto ids = BuildDefaultQtPanelIds();
  models.reserve(ids.size());
  for (const auto& id : ids) {
    models.push_back(BuildQtPanelModel(id, context));
  }
  return models;
}

}  // namespace vkpt::editor
