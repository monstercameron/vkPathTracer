#ifdef PT_ENABLE_QT

#include "app/QtDockPanelsInternal.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace vkpt::app {

QtDockPanelContent BuildQtSceneTreeDock(const vkpt::scene::SceneDocument& document,
                                        const vkpt::editor::SelectionState& selection,
                                        const vkpt::editor::UiRuntimeState& runtime,
                                        const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "scene_graph", "Scene Graph", true, 280.0f, 600.0f);
  panel.tree_single_column = true;
  panel.tree_stretch = 1;
  panel.property_stretch = 0;
  panel.property_preferred_height = 220;
  QtDockAddTextGroupedProperty(panel,
                               "scene_tree.filter.name",
                               "Filter",
                               "Name contains",
                               runtime.scene_tree_name_filter);
  const std::uint32_t typeMask = runtime.scene_tree_type_filter_mask;
  QtDockAddToggleGroupedProperty(panel,
                                 "scene_tree.filter.camera",
                                 "Type Filter",
                                 "Cameras",
                                 (typeMask & kQtSceneTreeFilterCamera) != 0u);
  QtDockAddToggleGroupedProperty(panel,
                                 "scene_tree.filter.light",
                                 "Type Filter",
                                 "Lights",
                                 (typeMask & kQtSceneTreeFilterLight) != 0u);
  QtDockAddToggleGroupedProperty(panel,
                                 "scene_tree.filter.model",
                                 "Type Filter",
                                 "Models",
                                 (typeMask & kQtSceneTreeFilterModel) != 0u);
  QtDockAddToggleGroupedProperty(panel,
                                 "scene_tree.filter.sdf",
                                 "Type Filter",
                                 "SDF",
                                 (typeMask & kQtSceneTreeFilterSdf) != 0u);
  QtDockAddToggleGroupedProperty(panel,
                                 "scene_tree.filter.physics",
                                 "Type Filter",
                                 "Physics",
                                 (typeMask & kQtSceneTreeFilterPhysics) != 0u);
  QtDockAddToggleGroupedProperty(panel,
                                 "scene_tree.filter.script",
                                 "Type Filter",
                                 "Scripts",
                                 (typeMask & kQtSceneTreeFilterScript) != 0u);
  QtDockAddToggleGroupedProperty(panel,
                                 "scene_tree.filter.animation",
                                 "Type Filter",
                                 "Animation",
                                 (typeMask & kQtSceneTreeFilterAnimation) != 0u);
  QtDockAddToggleGroupedProperty(panel,
                                 "scene_tree.filter.entity",
                                 "Type Filter",
                                 "Groups",
                                 (typeMask & kQtSceneTreeFilterEntity) != 0u);
  QtDockAddToggleGroupedProperty(panel,
                                 "scene_tree.filter.particle",
                                 "Type Filter",
                                 "Particles",
                                 (typeMask & kQtSceneTreeFilterParticle) != 0u);
  QtDockAddButtonGroupedProperty(panel,
                                 "scene_tree.filter.clear",
                                 "Filter",
                                 "Clear filters",
                                 "Clear");
  QtDockAddProperty(panel, "entities", std::to_string(document.entities.size()));
  QtDockAddProperty(panel, "geometry", std::to_string(document.geometry.size()));
  QtDockAddProperty(panel, "sdf primitives", std::to_string(document.sdf_primitives.size()));
  QtDockAddProperty(panel, "particle emitters", std::to_string(document.particle_emitters.size()));
  const std::string nameFilter = QtDockToLower(QtTrim(runtime.scene_tree_name_filter));
  const bool typeFilterActive = typeMask != 0u;
  const bool filtersActive = typeFilterActive || !nameFilter.empty();

  std::unordered_set<vkpt::core::StableId> selected_ids;
  selected_ids.reserve(selection.selected_entity_ids.size());
  for (const auto id : selection.selected_entity_ids) {
    selected_ids.insert(id);
  }
  const auto is_selected = [&](vkpt::core::StableId id) {
    return selected_ids.contains(id);
  };

  const auto entity_icon = [](const vkpt::scene::SceneEntityDefinition& entity) {
    if (entity.has_camera) {
      return std::string("camera");
    }
    if (entity.has_light) {
      return std::string("light");
    }
    if (entity.has_mesh) {
      return std::string("model");
    }
    if (entity.has_physics_body) {
      return std::string("physics");
    }
    if (!entity.script.script.empty()) {
      return std::string("script");
    }
    if (!entity.animation.clip.empty()) {
      return std::string("animation");
    }
    return std::string("entity");
  };

  std::unordered_map<vkpt::core::StableId, std::vector<const vkpt::scene::SceneEntityDefinition*>> children;
  std::unordered_map<vkpt::core::StableId, const vkpt::scene::SceneEntityDefinition*> entityById;
  std::unordered_set<vkpt::core::StableId> entityIds;
  children.reserve(document.entities.size());
  entityById.reserve(document.entities.size());
  entityIds.reserve(document.entities.size());
  for (const auto& entity : document.entities) {
    entityIds.insert(entity.id);
    entityById.emplace(entity.id, &entity);
    children[entity.hierarchy.parent].push_back(&entity);
  }

  auto has_authored_payload = [](const vkpt::scene::SceneEntityDefinition& entity) {
    return entity.has_camera ||
           entity.has_light ||
           entity.has_mesh ||
           entity.has_physics_body ||
           entity.has_benchmark_tag ||
           entity.has_sdf_primitive ||
           !entity.script.script.empty() ||
           !entity.animation.clip.empty();
  };
  auto is_transparent_group = [&](const vkpt::scene::SceneEntityDefinition& entity) {
    if (has_authored_payload(entity)) {
      return false;
    }
    const auto lowerName = QtDockToLower(entity.name);
    if (lowerName == "cameras" ||
        lowerName == "lights" ||
        lowerName == "geometry" ||
        lowerName == "physics") {
      return true;
    }
    if (lowerName.starts_with("imported ")) {
      const auto childIt = children.find(entity.id);
      return childIt != children.end() && childIt->second.size() == 1u;
    }
    return false;
  };
  auto is_visibility_target = [](const vkpt::scene::SceneEntityDefinition& entity) {
    return entity.has_mesh || entity.has_sdf_primitive || entity.has_light;
  };
  auto entity_filter_bits = [](const vkpt::scene::SceneEntityDefinition& entity) {
    std::uint32_t bits = 0u;
    if (entity.has_camera) {
      bits |= kQtSceneTreeFilterCamera;
    }
    if (entity.has_light) {
      bits |= kQtSceneTreeFilterLight;
    }
    if (entity.has_mesh) {
      bits |= kQtSceneTreeFilterModel;
    }
    if (entity.has_sdf_primitive) {
      bits |= kQtSceneTreeFilterSdf;
    }
    if (entity.has_physics_body) {
      bits |= kQtSceneTreeFilterPhysics;
    }
    if (!entity.script.script.empty()) {
      bits |= kQtSceneTreeFilterScript;
    }
    if (!entity.animation.clip.empty()) {
      bits |= kQtSceneTreeFilterAnimation;
    }
    if (bits == 0u) {
      bits |= kQtSceneTreeFilterEntity;
    }
    return bits;
  };
  auto entity_matches_filters = [&](const vkpt::scene::SceneEntityDefinition& entity) {
    const bool typeOk =
        !typeFilterActive || (entity_filter_bits(entity) & typeMask) != 0u;
    const bool nameOk =
        nameFilter.empty() ||
        QtDockToLower(QtEntityDisplayName(entity)).find(nameFilter) != std::string::npos;
    return typeOk && nameOk;
  };

  std::unordered_map<vkpt::core::StableId, bool> visiblePathCache;
  visiblePathCache.reserve(document.entities.size());
  auto entity_visible_path =
      [&](const vkpt::scene::SceneEntityDefinition& entity) {
    if (const auto cached = visiblePathCache.find(entity.id);
        cached != visiblePathCache.end()) {
      return cached->second;
    }
    bool visible = true;
    const auto* current = &entity;
    std::unordered_set<vkpt::core::StableId> visitedPath;
    visitedPath.reserve(8u);
    for (std::size_t depth = 0u;
         current != nullptr && depth <= document.entities.size() &&
         visitedPath.insert(current->id).second;
         ++depth) {
      if (!current->visible) {
        visible = false;
        break;
      }
      const vkpt::core::StableId parent = current->hierarchy.parent;
      if (parent == 0u) {
        break;
      }
      const auto parentIt = entityById.find(parent);
      current = parentIt == entityById.end() ? nullptr : parentIt->second;
    }
    visiblePathCache.emplace(entity.id, visible);
    return visible;
  };

  std::unordered_map<vkpt::core::StableId, bool> visibilityTargetCache;
  visibilityTargetCache.reserve(document.entities.size());
  auto subtree_has_visibility_target =
      [&](auto&& self, vkpt::core::StableId entityId) -> bool {
    if (const auto cached = visibilityTargetCache.find(entityId);
        cached != visibilityTargetCache.end()) {
      return cached->second;
    }
    bool hasTarget = false;
    if (const auto entityIt = entityById.find(entityId);
        entityIt != entityById.end() && entityIt->second != nullptr) {
      hasTarget = is_visibility_target(*entityIt->second);
    }
    if (const auto childIt = children.find(entityId); childIt != children.end()) {
      for (const auto* child : childIt->second) {
        if (child != nullptr && self(self, child->id)) {
          hasTarget = true;
          break;
        }
      }
    }
    visibilityTargetCache.emplace(entityId, hasTarget);
    return hasTarget;
  };

  std::unordered_set<vkpt::core::StableId> visited;
  auto build_entity_row =
      [&](auto&& self,
          const vkpt::scene::SceneEntityDefinition& entity) -> std::optional<QtDockTreeRow> {
    visited.insert(entity.id);

    QtDockTreeRow row;
    row.id = "entity." + std::to_string(entity.id);
    row.label = QtEntityDisplayName(entity);
    row.value = "#" + std::to_string(entity.id) + "  " + QtEntityComponentSummary(entity);
    if (entity.hierarchy.parent != 0u && !entityIds.contains(entity.hierarchy.parent)) {
      row.value += "  missing parent #" + std::to_string(entity.hierarchy.parent);
    }
    row.visible = entity_visible_path(entity);
    row.visibility_toggle_enabled = subtree_has_visibility_target(
        subtree_has_visibility_target, entity.id);
    if (!row.visible) {
      row.value += "  hidden";
    }
    row.icon = entity_icon(entity);
    row.entity_id = entity.id;
    row.selected = is_selected(entity.id);
    const auto lowerName = QtDockToLower(entity.name);
    const bool sceneRoot = entity.hierarchy.parent == 0u &&
                           (lowerName == "scene root" || lowerName == "scene_root" || lowerName == "root");
    row.draggable = !sceneRoot;

    if (const auto childIt = children.find(entity.id); childIt != children.end()) {
      for (const auto* child : childIt->second) {
        if (child == nullptr || visited.contains(child->id)) {
          continue;
        }
        if (is_transparent_group(*child)) {
          visited.insert(child->id);
          if (const auto grandChildIt = children.find(child->id); grandChildIt != children.end()) {
            for (const auto* grandChild : grandChildIt->second) {
              if (grandChild == nullptr || visited.contains(grandChild->id)) {
                continue;
              }
              if (auto childRow = self(self, *grandChild)) {
                row.children.push_back(std::move(childRow.value()));
              }
            }
          }
          continue;
        }
        if (auto childRow = self(self, *child)) {
          row.children.push_back(std::move(childRow.value()));
        }
      }
    }
    if (filtersActive && !entity_matches_filters(entity) && row.children.empty()) {
      return std::nullopt;
    }
    return row;
  };

  for (const auto& entity : document.entities) {
    if (visited.contains(entity.id)) {
      continue;
    }
    if (entity.hierarchy.parent == 0u || !entityIds.contains(entity.hierarchy.parent)) {
      if (is_transparent_group(entity)) {
        visited.insert(entity.id);
        if (const auto childIt = children.find(entity.id); childIt != children.end()) {
          for (const auto* child : childIt->second) {
            if (child == nullptr || visited.contains(child->id)) {
              continue;
            }
            if (auto row = build_entity_row(build_entity_row, *child)) {
              QtDockAddTreeRow(panel, std::move(row.value()));
            }
          }
        }
      } else {
        if (auto row = build_entity_row(build_entity_row, entity)) {
          QtDockAddTreeRow(panel, std::move(row.value()));
        }
      }
    }
  }
  for (const auto& entity : document.entities) {
    if (!visited.contains(entity.id)) {
      if (auto row = build_entity_row(build_entity_row, entity)) {
        QtDockAddTreeRow(panel, std::move(row.value()));
      }
    }
  }

  if (!document.sdf_primitives.empty()) {
    QtDockTreeRow sdfGroup;
    sdfGroup.id = "sdf_primitives";
    sdfGroup.label = "SDF Primitives";
    sdfGroup.value = std::to_string(document.sdf_primitives.size()) + " authored";
    sdfGroup.icon = "group";
    for (const auto& primitive : document.sdf_primitives) {
      const std::string shape = primitive.shape.empty()
          ? (primitive.primitive.shape.empty() ? std::string("sphere") : primitive.primitive.shape)
          : primitive.shape;
      const std::string label = "SDF " + shape;
      const bool typeOk =
          !typeFilterActive || (typeMask & kQtSceneTreeFilterSdf) != 0u;
      const bool nameOk =
          nameFilter.empty() ||
          QtDockToLower(label).find(nameFilter) != std::string::npos;
      if (!typeOk || !nameOk) {
        continue;
      }
      QtDockTreeRow row;
      row.id = "sdf." + std::to_string(primitive.id);
      row.label = label;
      row.value = "#" + std::to_string(primitive.id);
      row.icon = "sdf";
      row.entity_id = primitive.id;
      row.selected = is_selected(primitive.id);
      sdfGroup.children.push_back(std::move(row));
    }
    if (!sdfGroup.children.empty()) {
      QtDockAddTreeRow(panel, std::move(sdfGroup));
    }
  }

  if (!document.particle_emitters.empty()) {
    QtDockTreeRow particleGroup;
    particleGroup.id = "particle_emitters";
    particleGroup.label = "Particle Emitters";
    particleGroup.value = std::to_string(document.particle_emitters.size()) + " authored";
    particleGroup.icon = "group";
    for (const auto& emitter : document.particle_emitters) {
      const std::string type = emitter.type.empty() ? std::string("particles") : emitter.type;
      const std::string label = emitter.name.empty() ? ("Emitter " + std::to_string(emitter.id)) : emitter.name;
      const bool typeOk =
          !typeFilterActive || (typeMask & kQtSceneTreeFilterParticle) != 0u;
      const bool nameOk =
          nameFilter.empty() ||
          QtDockToLower(label).find(nameFilter) != std::string::npos ||
          QtDockToLower(type).find(nameFilter) != std::string::npos;
      if (!typeOk || !nameOk) {
        continue;
      }
      QtDockTreeRow row;
      row.id = "particle." + std::to_string(emitter.id);
      row.label = label;
      row.value = "#" + std::to_string(emitter.id) + "  " + type +
                  " count=" + std::to_string(emitter.count);
      if (!emitter.enabled) {
        row.value += "  disabled";
        row.visible = false;
      }
      row.icon = "particle";
      row.entity_id = emitter.id;
      row.selected = is_selected(emitter.id);
      row.draggable = false;
      row.visibility_toggle_enabled = false;
      particleGroup.children.push_back(std::move(row));
    }
    if (!particleGroup.children.empty()) {
      QtDockAddTreeRow(panel, std::move(particleGroup));
    }
  }

  if (panel.tree_rows.empty()) {
    QtDockAddRow(panel,
                 filtersActive ? "No scene graph items match the active filters"
                               : "No authored entities in document");
  }
  return panel;
}

QtDockPanelContent BuildQtInspectorDock(const vkpt::scene::SceneDocument& document,
                                        const vkpt::editor::SelectionState& selection,
                                        const vkpt::editor::UiRuntimeState& runtime,
                                        const vkpt::editor::UiLayoutDocument& layout) {
  (void)runtime;
  auto panel = MakeQtDockPanel(layout, "inspector", "Inspector", true, 420.0f, 600.0f);
  const auto primaryId = QtPrimarySelectionId(selection);

  if (primaryId == 0u) {
    QtDockAddProperty(panel, "Selection", "No object selected");
    return panel;
  }

  const auto* entity = FindQtSceneEntity(document, primaryId);
  if (entity == nullptr) {
    const auto* primitive = FindQtSceneSdfPrimitive(document, primaryId);
    if (primitive == nullptr) {
      const auto emitterIt = std::find_if(
          document.particle_emitters.begin(),
          document.particle_emitters.end(),
          [&](const vkpt::scene::SceneParticleEmitterDefinition& emitter) {
            return emitter.id == primaryId;
          });
      if (emitterIt == document.particle_emitters.end()) {
        QtDockAddProperty(panel, "Selection", "Selected object is not in the loaded document");
        return panel;
      }
      const auto& emitter = *emitterIt;
      QtDockAddProperty(panel,
                        "Name",
                        emitter.name.empty()
                            ? ("particle emitter " + std::to_string(emitter.id))
                            : emitter.name);
      QtDockAddProperty(panel, "Type", "Particle Emitter");
      QtDockAddProperty(panel, "Emitter", emitter.type.empty() ? "particles" : emitter.type);
      QtDockAddProperty(panel, "Enabled", QtDockBool(emitter.enabled));
      QtDockAddProperty(panel, "Count", std::to_string(emitter.count));
      QtDockAddProperty(panel, "Material", QtMaterialDisplayLabel(document, emitter.material_id));
      QtDockAddProperty(panel, "Bounds", QtDockVec3(emitter.bounds));
      QtDockAddProperty(panel, "Velocity", QtDockVec3(emitter.velocity));
      QtDockAddProperty(panel, "Wind", QtDockVec3(emitter.wind));
      QtDockAddProperty(panel, "Lifetime", QtDockNumber(emitter.lifetime, 2) + " s");
      QtDockAddInspectorTransformControls(panel,
                                          "particle." + std::to_string(emitter.id) + ".transform.",
                                          emitter.transform);
      return panel;
    }

    QtDockAddProperty(panel, "Name", "sdf " + std::to_string(primitive->id));
    QtDockAddProperty(panel, "Type", "SDF Primitive");
    if (const auto* bounds = FindQtSelectionBounds(selection, primaryId)) {
      QtDockAddProperty(panel, "Bounds", QtDockBounds(*bounds));
    }

    const std::string sdfPrefix = "sdf." + std::to_string(primitive->id) + ".";
    QtDockAddInspectorTransformControls(panel,
                                        sdfPrefix + "transform.",
                                        primitive->transform);

    const std::string shape = primitive->shape.empty()
        ? (primitive->primitive.shape.empty() ? std::string("sphere") : primitive->primitive.shape)
        : primitive->shape;
    QtDockAddDropdownGroupedProperty(panel,
                                     sdfPrefix + "shape",
                                     "",
                                     "SDF shape",
                                     shape,
                                     QtSdfShapeOptions());
    QtDockAddSliderGroupedProperty(panel,
                                   sdfPrefix + "primitive.radius",
                                   "",
                                   "SDF radius",
                                   primitive->primitive.radius,
                                   0.01,
                                   10.0,
                                   0.01,
                                   1.0);
    QtDockAddSliderGroupedProperty(panel,
                                   sdfPrefix + "primitive.param_a",
                                   "",
                                   "SDF param A",
                                   primitive->primitive.param_a,
                                   -10.0,
                                   10.0,
                                   0.01,
                                   0.0);
    QtDockAddSliderGroupedProperty(panel,
                                   sdfPrefix + "primitive.param_b",
                                   "",
                                   "SDF param B",
                                   primitive->primitive.param_b,
                                   -10.0,
                                   10.0,
                                   0.01,
                                   0.0);
    return panel;
  }

  QtDockAddEditableGroupedProperty(panel,
                                   "entity." + std::to_string(entity->id) + ".name",
                                   "",
                                   "Name",
                                   QtEntityDisplayName(*entity));
  QtDockAddProperty(panel, "Type", QtEntityComponentSummary(*entity));
  if (const auto* bounds = FindQtSelectionBounds(selection, primaryId)) {
    QtDockAddProperty(panel, "Bounds", QtDockBounds(*bounds));
  }

  const std::string entityPrefix = "entity." + std::to_string(entity->id) + ".";

  if (entity->has_transform) {
    QtDockAddInspectorTransformControls(panel,
                                        entityPrefix + "transform.",
                                        entity->transform);
  }
  if (entity->has_mesh) {
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "mesh.mesh_id",
                                     "",
                                     "Mesh",
                                     QtGeometryDisplayLabel(document, entity->mesh.mesh_id),
                                     QtGeometryIdOptions(document, entity->mesh.mesh_id));
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "mesh.material_id",
                                     "",
                                     "Material",
                                     QtMaterialDisplayLabel(document, entity->mesh.material_id),
                                     QtMaterialIdOptions(document, entity->mesh.material_id));
    if (const auto* material = FindQtSceneMaterial(document, entity->mesh.material_id)) {
      const std::string materialPrefix = "material." + std::to_string(material->id) + ".";
      QtDockAddDropdownGroupedProperty(panel,
                                       materialPrefix + "family",
                                       "",
                                       "Material model",
                                       material->family.empty() ? std::string("diffuse") : material->family,
                                       QtMaterialFamilyOptions());
      QtDockAddVec3Sliders(panel,
                           materialPrefix + "albedo",
                           "",
                           "Base color",
                           material->albedo,
                           vkpt::scene::Vec3{0.8f, 0.8f, 0.8f},
                           0.0,
                           1.0,
                           0.01);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "roughness",
                                     "",
                                     "Roughness",
                                     material->roughness,
                                     0.0,
                                     1.0,
                                     0.01,
                                     0.6);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "metallic",
                                     "",
                                     "Metallic",
                                     material->metallic,
                                     0.0,
                                     1.0,
                                     0.01,
                                     0.0);
      if (material->emission_intensity > 0.0f ||
          material->emission.x > 0.0f ||
          material->emission.y > 0.0f ||
          material->emission.z > 0.0f) {
        QtDockAddSliderGroupedProperty(panel,
                                       materialPrefix + "emission_intensity",
                                       "",
                                       "Emission",
                                       material->emission_intensity,
                                       0.0,
                                       50.0,
                                       0.1,
                                       0.0);
      }
    }
  }
  if (entity->has_sdf_primitive) {
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "sdf_primitive.shape",
                                     "",
                                     "SDF shape",
                                     entity->sdf_primitive.shape.empty()
                                         ? std::string("sphere")
                                         : entity->sdf_primitive.shape,
                                     QtSdfShapeOptions());
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "sdf_primitive.radius",
                                   "",
                                   "SDF radius",
                                   entity->sdf_primitive.radius,
                                   0.01,
                                   10.0,
                                   0.01,
                                   1.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "sdf_primitive.param_a",
                                   "",
                                   "SDF param A",
                                   entity->sdf_primitive.param_a,
                                   -10.0,
                                   10.0,
                                   0.01,
                                   0.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "sdf_primitive.param_b",
                                   "",
                                   "SDF param B",
                                   entity->sdf_primitive.param_b,
                                   -10.0,
                                   10.0,
                                   0.01,
                                   0.0);
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "material.material_id",
                                     "",
                                     "Material",
                                     QtMaterialDisplayLabel(document, entity->material.material_id),
                                     QtMaterialIdOptions(document, entity->material.material_id));
  }
  if (entity->has_light) {
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "light.type",
                                     "",
                                     "Light type",
                                     entity->light.type.empty() ? std::string("point") : entity->light.type,
                                     QtLightTypeOptions());
    QtDockAddVec3Sliders(panel,
                         entityPrefix + "light.color",
                         "",
                         "Light color",
                         entity->light.color,
                         vkpt::scene::Vec3{1.0f, 1.0f, 1.0f},
                         0.0,
                         1.0,
                         0.01);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "light.intensity",
                                   "",
                                   "Light intensity",
                                   entity->light.intensity,
                                   0.0,
                                   100.0,
                                   0.1,
                                   1.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "light.radius",
                                   "",
                                   "Light radius",
                                   entity->light.radius,
                                   0.0,
                                   10.0,
                                   0.01,
                                   0.0);
    if (QtTrim(entity->light.type) == "spot") {
      QtDockAddVec3Sliders(panel,
                           entityPrefix + "light.direction",
                           "",
                           "Spot direction",
                           entity->light.direction,
                           vkpt::scene::Vec3{0.0f, -1.0f, 0.0f},
                           -1.0,
                           1.0,
                           0.01);
      QtDockAddSliderGroupedProperty(panel,
                                     entityPrefix + "light.beam_angle",
                                     "",
                                     "Spot beam",
                                     entity->light.beam_angle_degrees,
                                     1.0,
                                     120.0,
                                     0.5,
                                     35.0);
      QtDockAddSliderGroupedProperty(panel,
                                     entityPrefix + "light.blend",
                                     "",
                                     "Spot edge",
                                     entity->light.blend,
                                     0.0,
                                     1.0,
                                     0.01,
                                     0.35);
    }
  }
  if (entity->has_camera) {
    QtDockAddPrimaryCameraControls(panel, entityPrefix + "camera.", entity->camera);
  }
  QtDockAddDropdownGroupedProperty(panel,
                                   entityPrefix + "physics.enabled",
                                   "",
                                   "Physics",
                                   QtDockBool(entity->has_physics_body && entity->physics_body.enabled),
                                   QtBoolOptions());
  if (entity->has_physics_body) {
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "physics.body_type",
                                     "",
                                     "Body type",
                                     entity->physics_body.dynamic
                                         ? std::string("dynamic")
                                         : entity->physics_body.body_type,
                                     QtPhysicsBodyTypeOptions());
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "physics.shape",
                                     "",
                                     "Collision shape",
                                     entity->physics_body.shape.empty()
                                         ? std::string("box")
                                         : entity->physics_body.shape,
                                     QtPhysicsShapeOptions());
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "physics.mass",
                                   "",
                                   "Mass",
                                   entity->physics_body.mass,
                                   0.01,
                                   1000.0,
                                   0.01,
                                   1.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "physics.friction",
                                   "",
                                   "Friction",
                                   entity->physics_body.friction,
                                   0.0,
                                   2.0,
                                   0.01,
                                   0.5);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "physics.restitution",
                                   "",
                                   "Bounce",
                                   entity->physics_body.restitution,
                                   0.0,
                                   1.0,
                                   0.01,
                                   0.0);
  }
  if (!entity->script.script.empty()) {
    QtDockAddProperty(panel, "Script", entity->script.script);
  }
  return panel;
}

QtDockPanelContent BuildQtMaterialsDock(const vkpt::scene::SceneDocument& document,
                                        const vkpt::pathtracer::RTSceneData& scene,
                                        const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "materials", "Materials", true, 520.0f, 420.0f);
  QtDockAddProperty(panel, "authored materials", std::to_string(document.materials.size()));
  QtDockAddProperty(panel, "runtime materials", std::to_string(scene.materials.size()));
  for (const auto& material : document.materials) {
    std::ostringstream row;
    row << "#" << material.id << " "
        << (material.name.empty() ? "material" : material.name)
        << " albedo=(" << QtDockVec3(material.albedo) << ")"
        << " roughness=" << QtDockNumber(material.roughness, 2);
    if (material.emission_intensity > 0.0f) {
      row << " emissive=" << QtDockNumber(material.emission_intensity, 2);
    }
    QtDockAddRow(panel, row.str());
  }
  if (document.materials.empty()) {
    for (std::size_t i = 0; i < scene.materials.size(); ++i) {
      const auto& material = scene.materials[i];
      std::ostringstream row;
      row << "runtime[" << i << "] albedo=(" << QtDockVec3(material.albedo)
          << ") roughness=" << QtDockNumber(material.roughness, 2)
          << " emissive=" << QtDockBool(material.is_emissive());
      QtDockAddRow(panel, row.str());
    }
  }
  QtDockLimitRows(panel, 96u);
  return panel;
}

QtDockPanelContent BuildQtLightsDock(const vkpt::scene::SceneDocument& document,
                                     const vkpt::pathtracer::RTSceneData& scene,
                                     const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "lights", "Lights", true, 360.0f, 360.0f);
  const auto lightObjectCount = static_cast<std::size_t>(std::count_if(
      document.entities.begin(),
      document.entities.end(),
      [](const vkpt::scene::SceneEntityDefinition& entity) {
        return entity.has_light;
      })) + document.lights.size();
  QtDockAddProperty(panel, "light objects", std::to_string(lightObjectCount));
  if (!document.lights.empty()) {
    QtDockAddProperty(panel, "legacy lights", std::to_string(document.lights.size()));
  }
  QtDockAddProperty(panel, "runtime lights", std::to_string(scene.lights.size()));
  for (const auto& entity : document.entities) {
    if (!entity.has_light) {
      continue;
    }
    std::ostringstream row;
    row << QtEntityDisplayName(entity) << " #" << entity.id
        << " " << entity.light.type
        << " intensity=" << QtDockNumber(entity.light.intensity, 2)
        << " color=(" << QtDockVec3(entity.light.color) << ")";
    QtDockAddRow(panel, row.str());
  }
  for (std::size_t i = 0; i < scene.lights.size(); ++i) {
    const auto& light = scene.lights[i];
    std::ostringstream row;
    row << "runtime[" << i << "] pos=(" << QtDockVec3(light.position)
        << ") intensity=" << QtDockNumber(light.intensity, 2)
        << " radius=" << QtDockNumber(light.radius, 2);
    QtDockAddRow(panel, row.str());
  }
  if (panel.rows.empty()) {
    QtDockAddRow(panel, "No lights in the loaded document or render scene");
  }
  return panel;
}

QtDockPanelContent BuildQtCameraDock(const vkpt::scene::SceneDocument& document,
                                     const vkpt::pathtracer::RTSceneData& scene,
                                     const vkpt::editor::UiRuntimeState& runtime,
                                     const vkpt::editor::UiLayoutDocument& layout,
                                     const QtDockFrameStats& frame_stats,
                                     int active_shot_slot,
                                     const std::array<bool, 4>& saved_shot_slots) {
  auto panel = MakeQtDockPanel(layout, "camera", "Camera", true, 420.0f, 560.0f);
  QtDockAddProperty(panel, "Active", runtime.active_camera.empty() ? "runtime camera" : runtime.active_camera);
  QtDockAddProperty(panel, "Mode", frame_stats.camera_mode.empty() ? "authored" : frame_stats.camera_mode);
  QtDockAddProperty(panel, "Runtime focus", QtDockNumber(scene.camera_focus_distance, 2));
  QtDockAddButtonGroupedProperty(panel,
                                 "camera.mode.fps_toggle",
                                 "",
                                 frame_stats.camera_mode == "fps" ? "Exit FPS" : "Enter FPS",
                                 frame_stats.camera_mode == "fps" ? "Exit FPS" : "Enter FPS");
  if (frame_stats.camera_mode == "fps") {
    QtDockAddProperty(panel,
                      "FPS body",
                      frame_stats.fps_player_grounded ? "grounded" : "airborne");
    QtDockAddProperty(panel, "FPS speed", QtDockNumber(frame_stats.fps_player_speed, 2));
    QtDockAddProperty(panel, "FPS eye height", QtDockNumber(frame_stats.fps_player_eye_height, 2));
    QtDockAddProperty(panel, "Run", QtDockBool(frame_stats.fps_player_running));
    QtDockAddProperty(panel, "Crouch", QtDockBool(frame_stats.fps_player_crouching));
  }
  QtDockAddButtonGroupedProperty(panel,
                                 "camera.focus.pick",
                                 "",
                                 "Focus under cursor",
                                 "Focus Under Cursor");
  QtDockAddButtonGroupedProperty(panel,
                                 "camera.focus.selected",
                                 "",
                                 "Focus selected",
                                 "Focus Selected");
  const int clampedShotSlot = std::clamp(active_shot_slot, 0, 3);
  QtDockAddDropdownGroupedProperty(panel,
                                   "camera.shot.slot",
                                   "",
                                   "Shot slot",
                                   std::to_string(clampedShotSlot + 1),
                                   {"1", "2", "3", "4"});
  QtDockAddButtonGroupedProperty(panel,
                                 "camera.shot.save",
                                 "",
                                 "Save shot",
                                 "Save Shot");
  QtDockAddButtonGroupedProperty(panel,
                                 "camera.shot.recall",
                                 "",
                                 "Recall shot",
                                 "Recall Shot");
  std::ostringstream savedSlots;
  for (std::size_t i = 0; i < saved_shot_slots.size(); ++i) {
    if (i > 0u) {
      savedSlots << "  ";
    }
    savedSlots << (i + 1u) << ":" << (saved_shot_slots[i] ? "saved" : "empty");
  }
  QtDockAddProperty(panel, "Saved shots", savedSlots.str());
  QtDockAddProperty(panel, "Viewport tool", vkpt::editor::ToString(runtime.active_viewport_tool));
  bool addedCameraControls = false;
  for (const auto& entity : document.entities) {
    if (entity.has_camera) {
      QtDockAddProperty(panel, "Editing", QtEntityDisplayName(entity) + " #" + std::to_string(entity.id));
      QtDockAddPrimaryCameraControls(panel,
                                     "entity." + std::to_string(entity.id) + ".camera.",
                                     entity.camera);
      addedCameraControls = true;
      break;
    }
  }
  if (!addedCameraControls) {
    QtDockAddProperty(panel, "Lens", QtDockNumber(scene.camera_focal_length_mm, 1) + " mm");
    QtDockAddProperty(panel, "Aperture", QtDockNumber(scene.camera_aperture_radius, 3));
    QtDockAddProperty(panel, "Exposure", QtDockNumber(scene.camera_exposure_compensation, 2) + " EV");
    QtDockAddProperty(panel, "White balance", QtDockNumber(scene.camera_white_balance_kelvin, 0) + " K");
  }
  return panel;
}

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
