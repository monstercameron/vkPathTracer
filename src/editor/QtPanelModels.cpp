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
