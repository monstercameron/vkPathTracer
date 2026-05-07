#include "app/QtDockPanelsInternal.h"

#ifdef PT_ENABLE_QT

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace vkpt::app {

std::string_view QtTrimView(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::string QtTrim(std::string_view text) {
  return std::string(QtTrimView(text));
}

std::vector<std::string> QtSplitPropertyPath(std::string_view id) {
  std::vector<std::string> parts;
  parts.reserve(static_cast<std::size_t>(std::count(id.begin(), id.end(), '.')) + 1u);
  std::size_t start = 0;
  while (start <= id.size()) {
    const auto dot = id.find('.', start);
    const auto end = dot == std::string_view::npos ? id.size() : dot;
    parts.emplace_back(id.substr(start, end - start));
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
  return parts;
}

bool QtParseFloat(std::string_view text, float& out) {
  std::string_view numeric = QtTrimView(text);
  if (!numeric.empty() && numeric.front() == '+') {
    numeric.remove_prefix(1);
  }
  float value = 0.0f;
  const auto parsed = std::from_chars(numeric.data(), numeric.data() + numeric.size(), value);
  if (numeric.empty() ||
      parsed.ec != std::errc{} ||
      parsed.ptr != numeric.data() + numeric.size() ||
      !std::isfinite(value)) {
    return false;
  }
  out = value;
  return true;
}

void QtSkipFloatDelimiters(std::string_view text, std::size_t& cursor) {
  while (cursor < text.size() &&
         (text[cursor] == ',' || std::isspace(static_cast<unsigned char>(text[cursor])))) {
    ++cursor;
  }
}

bool QtParseFloatComponent(std::string_view text, std::size_t& cursor, float& out) {
  QtSkipFloatDelimiters(text, cursor);
  if (cursor >= text.size()) {
    return false;
  }
  const auto begin = cursor;
  while (cursor < text.size() &&
         text[cursor] != ',' &&
         !std::isspace(static_cast<unsigned char>(text[cursor]))) {
    ++cursor;
  }
  return QtParseFloat(text.substr(begin, cursor - begin), out);
}

bool QtParseStableId(std::string_view text, vkpt::core::StableId& out) {
  const auto trimmed = QtTrim(text);
  const auto lower = QtDockToLower(trimmed);
  if (trimmed.empty() || lower == "none" || lower == "#0 none") {
    out = 0;
    return true;
  }
  // Dock labels are displayed as "#id name"; parsing tolerates the label text
  // so dropdown values can round-trip directly from the UI.
  std::size_t begin = 0u;
  if (trimmed[begin] == '#') {
    ++begin;
  } else if (!std::isdigit(static_cast<unsigned char>(trimmed[begin]))) {
    const auto hash = trimmed.find('#');
    if (hash == std::string::npos) {
      return false;
    }
    begin = hash + 1u;
  }
  std::size_t end = begin;
  while (end < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[end]))) {
    ++end;
  }
  if (begin == end) {
    return false;
  }
  vkpt::core::StableId value = 0;
  const auto parsed = std::from_chars(trimmed.data() + begin, trimmed.data() + end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != trimmed.data() + end) {
    return false;
  }
  out = value;
  return true;
}

bool QtParseBool(std::string_view text, bool& out) {
  const auto value = QtDockToLower(QtTrim(text));
  if (value == "true" || value == "1" || value == "yes" || value == "on" || value == "enabled" ||
      value == "clicked") {
    out = true;
    return true;
  }
  if (value == "false" || value == "0" || value == "no" || value == "off" || value == "disabled") {
    out = false;
    return true;
  }
  return false;
}

bool QtParseVec3(std::string_view text, vkpt::scene::Vec3& out) {
  std::size_t cursor = 0u;
  vkpt::scene::Vec3 value{};
  if (!QtParseFloatComponent(text, cursor, value.x) ||
      !QtParseFloatComponent(text, cursor, value.y) ||
      !QtParseFloatComponent(text, cursor, value.z)) {
    return false;
  }
  out = value;
  return true;
}

bool QtParseQuat(std::string_view text, vkpt::scene::Quat& out) {
  std::size_t cursor = 0u;
  vkpt::scene::Quat value{};
  if (!QtParseFloatComponent(text, cursor, value.x) ||
      !QtParseFloatComponent(text, cursor, value.y) ||
      !QtParseFloatComponent(text, cursor, value.z) ||
      !QtParseFloatComponent(text, cursor, value.w)) {
    return false;
  }
  out = value;
  return true;
}

const vkpt::scene::SceneMaterialDefinition* FindQtSceneMaterial(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.materials.begin(),
                               document.materials.end(),
                               [&](const vkpt::scene::SceneMaterialDefinition& material) {
                                 return material.id == id;
                               });
  return it == document.materials.end() ? nullptr : &*it;
}

vkpt::scene::SceneMaterialDefinition* FindQtMutableSceneMaterial(
    vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.materials.begin(),
                               document.materials.end(),
                               [&](const vkpt::scene::SceneMaterialDefinition& material) {
                                 return material.id == id;
                               });
  return it == document.materials.end() ? nullptr : &*it;
}

const vkpt::scene::SceneGeometryDefinition* FindQtSceneGeometry(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.geometry.begin(),
                               document.geometry.end(),
                               [&](const vkpt::scene::SceneGeometryDefinition& geometry) {
                                 return geometry.id == id;
                               });
  return it == document.geometry.end() ? nullptr : &*it;
}

std::string QtStableIdDisplayLabel(std::string label, vkpt::core::StableId id) {
  if (id == 0u) {
    return "none";
  }
  if (label.empty()) {
    label = "id";
  }
  return "#" + std::to_string(id) + " " + label;
}

std::string QtMaterialDisplayLabel(const vkpt::scene::SceneDocument& document,
                                   vkpt::core::StableId id) {
  if (id == 0u) {
    return "none";
  }
  if (const auto* material = FindQtSceneMaterial(document, id)) {
    return QtStableIdDisplayLabel(material->name.empty() ? std::string("material") : material->name, id);
  }
  return QtStableIdDisplayLabel("missing material", id);
}

std::string QtGeometryDisplayLabel(const vkpt::scene::SceneDocument& document,
                                   vkpt::core::StableId id) {
  if (id == 0u) {
    return "none";
  }
  if (const auto* geometry = FindQtSceneGeometry(document, id)) {
    return QtStableIdDisplayLabel(geometry->primitive.empty() ? std::string("mesh") : geometry->primitive, id);
  }
  return QtStableIdDisplayLabel("missing mesh", id);
}

std::vector<std::string> QtMaterialIdOptions(const vkpt::scene::SceneDocument& document,
                                             vkpt::core::StableId current) {
  std::vector<std::string> options;
  options.reserve(document.materials.size() + 2u);
  options.push_back("none");
  bool hasCurrent = current == 0u;
  for (const auto& material : document.materials) {
    options.push_back(QtStableIdDisplayLabel(material.name.empty() ? std::string("material") : material.name,
                                             material.id));
    hasCurrent = hasCurrent || material.id == current;
  }
  if (!hasCurrent) {
    options.push_back(QtStableIdDisplayLabel("missing material", current));
  }
  return options;
}

std::vector<std::string> QtGeometryIdOptions(const vkpt::scene::SceneDocument& document,
                                             vkpt::core::StableId current) {
  std::vector<std::string> options;
  options.reserve(document.geometry.size() + 2u);
  options.push_back("none");
  bool hasCurrent = current == 0u;
  for (const auto& geometry : document.geometry) {
    options.push_back(QtStableIdDisplayLabel(geometry.primitive.empty() ? std::string("mesh") : geometry.primitive,
                                             geometry.id));
    hasCurrent = hasCurrent || geometry.id == current;
  }
  if (!hasCurrent) {
    options.push_back(QtStableIdDisplayLabel("missing mesh", current));
  }
  return options;
}

std::vector<std::string> QtLightTypeOptions() {
  return {"point", "directional", "spot"};
}

std::vector<std::string> QtSdfShapeOptions() {
  return {"sphere", "box", "rounded_box", "plane", "torus", "capsule"};
}

std::vector<std::string> QtPhysicsBodyTypeOptions() {
  return {"static", "dynamic", "kinematic"};
}

std::vector<std::string> QtPhysicsShapeOptions() {
  return {"box", "sphere", "capsule", "mesh"};
}

std::vector<std::string> QtBoolOptions() {
  return {"true", "false"};
}

std::vector<std::string> QtToneMapOptions() {
  return {"linear", "reinhard", "filmic_approx", "aces_approx"};
}

std::vector<std::string> QtOutputTransformOptions() {
  return {"gamma", "linear"};
}

std::string QtToneMapName(vkpt::pathtracer::ToneMapMode mode) {
  switch (mode) {
    case vkpt::pathtracer::ToneMapMode::Reinhard:
      return "reinhard";
    case vkpt::pathtracer::ToneMapMode::FilmicApprox:
      return "filmic_approx";
    case vkpt::pathtracer::ToneMapMode::AcesApprox:
      return "aces_approx";
    case vkpt::pathtracer::ToneMapMode::Linear:
    default:
      return "linear";
  }
}

std::string QtOutputTransformName(vkpt::pathtracer::OutputTransformMode mode) {
  switch (mode) {
    case vkpt::pathtracer::OutputTransformMode::Linear:
      return "linear";
    case vkpt::pathtracer::OutputTransformMode::Gamma:
    default:
      return "gamma";
  }
}

bool QtParseToneMapMode(std::string_view text, vkpt::pathtracer::ToneMapMode& out) {
  const auto value = QtDockToLower(QtTrim(text));
  if (value == "linear") {
    out = vkpt::pathtracer::ToneMapMode::Linear;
    return true;
  }
  if (value == "reinhard") {
    out = vkpt::pathtracer::ToneMapMode::Reinhard;
    return true;
  }
  if (value == "filmic_approx" || value == "filmic approx" || value == "filmic") {
    out = vkpt::pathtracer::ToneMapMode::FilmicApprox;
    return true;
  }
  if (value == "aces_approx" || value == "aces approx" || value == "aces") {
    out = vkpt::pathtracer::ToneMapMode::AcesApprox;
    return true;
  }
  return false;
}

bool QtParseOutputTransformMode(std::string_view text,
                                vkpt::pathtracer::OutputTransformMode& out) {
  const auto value = QtDockToLower(QtTrim(text));
  if (value == "gamma") {
    out = vkpt::pathtracer::OutputTransformMode::Gamma;
    return true;
  }
  if (value == "linear") {
    out = vkpt::pathtracer::OutputTransformMode::Linear;
    return true;
  }
  return false;
}

void QtDockAddVec3Sliders(QtDockPanelContent& panel,
                          std::string_view prefix,
                          std::string_view group,
                          std::string_view label,
                          const vkpt::scene::Vec3& value,
                          const vkpt::scene::Vec3& defaults,
                          double minimum,
                          double maximum,
                          double step) {
  const std::string base(prefix);
  const std::string groupText(group);
  const std::string labelText(label);
  QtDockAddSliderGroupedProperty(panel,
                                 base + ".x",
                                 groupText,
                                 labelText + " X",
                                 value.x,
                                 minimum,
                                 maximum,
                                 step,
                                 defaults.x);
  QtDockAddSliderGroupedProperty(panel,
                                 base + ".y",
                                 groupText,
                                 labelText + " Y",
                                 value.y,
                                 minimum,
                                 maximum,
                                 step,
                                 defaults.y);
  QtDockAddSliderGroupedProperty(panel,
                                 base + ".z",
                                 groupText,
                                 labelText + " Z",
                                 value.z,
                                 minimum,
                                 maximum,
                                 step,
                                 defaults.z);
}

void QtDockAddInspectorTransformControls(QtDockPanelContent& panel,
                                         std::string_view prefix,
                                         const vkpt::scene::TransformComponent& transform) {
  const std::string base(prefix);
  QtDockAddVec3Sliders(panel,
                       base + "translation",
                       "Transform",
                       "Position",
                       transform.translation,
                       vkpt::scene::Vec3{0.0f, 0.0f, 0.0f},
                       -1000.0,
                       1000.0,
                       0.01);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "rotation.x",
                                 "Transform",
                                 "Rotation X",
                                 transform.rotation.x,
                                 -1.0,
                                 1.0,
                                 0.001,
                                 0.0);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "rotation.y",
                                 "Transform",
                                 "Rotation Y",
                                 transform.rotation.y,
                                 -1.0,
                                 1.0,
                                 0.001,
                                 0.0);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "rotation.z",
                                 "Transform",
                                 "Rotation Z",
                                 transform.rotation.z,
                                 -1.0,
                                 1.0,
                                 0.001,
                                 0.0);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "rotation.w",
                                 "Transform",
                                 "Rotation W",
                                 transform.rotation.w,
                                 -1.0,
                                 1.0,
                                 0.001,
                                 1.0);
  QtDockAddVec3Sliders(panel,
                       base + "scale",
                       "Transform",
                       "Scale",
                       transform.scale,
                       vkpt::scene::Vec3{1.0f, 1.0f, 1.0f},
                       0.001,
                       100.0,
                       0.01);
}

void QtDockAddPrimaryCameraControls(QtDockPanelContent& panel,
                                    std::string_view prefix,
                                    const vkpt::scene::CameraComponent& camera) {
  const std::string base(prefix);
  QtDockAddSliderGroupedProperty(panel, base + "fov", "Camera", "FOV", camera.fov, 1.0, 179.0, 0.1, 60.0);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "near_plane",
                                 "Camera",
                                 "Near plane",
                                 camera.near_plane,
                                 0.001,
                                 1000.0,
                                 0.001,
                                 0.1);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "far_plane",
                                 "Camera",
                                 "Far plane",
                                 camera.far_plane,
                                 0.01,
                                 100000.0,
                                 1.0,
                                 1000.0);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "focal_length_mm",
                                 "Lens",
                                 "Focal length",
                                 camera.focal_length_mm,
                                 1.0,
                                 1000.0,
                                 0.1,
                                 35.0,
                                 "mm");
  QtDockAddSliderGroupedProperty(panel,
                                 base + "sensor_width_mm",
                                 "Lens",
                                 "Sensor width",
                                 camera.sensor_width_mm,
                                 1.0,
                                 200.0,
                                 0.1,
                                 36.0,
                                 "mm");
  QtDockAddSliderGroupedProperty(panel,
                                 base + "sensor_height_mm",
                                 "Lens",
                                 "Sensor height",
                                 camera.sensor_height_mm,
                                 1.0,
                                 200.0,
                                 0.1,
                                 24.0,
                                 "mm");
  QtDockAddSliderGroupedProperty(panel,
                                 base + "aperture_radius",
                                 "Exposure",
                                 "Aperture radius",
                                 camera.aperture_radius,
                                 0.0,
                                 100.0,
                                 0.001,
                                 0.0);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "focus_distance",
                                 "Exposure",
                                 "Focus distance",
                                 camera.focus_distance,
                                 0.0,
                                 100000.0,
                                 0.01,
                                 0.0);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "f_stop",
                                 "Exposure",
                                 "F-stop",
                                 camera.f_stop,
                                 0.0,
                                 256.0,
                                 0.1,
                                 0.0);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "shutter_seconds",
                                 "Exposure",
                                 "Shutter",
                                 camera.shutter_seconds,
                                 0.000001,
                                 60.0,
                                 0.001,
                                 0.0166667,
                                 "s");
  QtDockAddSliderGroupedProperty(panel,
                                 base + "iso",
                                 "Exposure",
                                 "ISO",
                                 camera.iso,
                                 1.0,
                                 1048576.0,
                                 1.0,
                                 100.0);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "exposure_compensation",
                                 "Exposure",
                                 "Exposure compensation",
                                 camera.exposure_compensation,
                                 -32.0,
                                 32.0,
                                 0.01,
                                 0.0,
                                 "EV");
  QtDockAddSliderGroupedProperty(panel,
                                 base + "white_balance_kelvin",
                                 "Exposure",
                                 "White balance",
                                 camera.white_balance_kelvin,
                                 1000.0,
                                 40000.0,
                                 10.0,
                                 6500.0,
                                 "K");
  QtDockAddSliderGroupedProperty(panel,
                                 base + "iris_blade_count",
                                 "Iris",
                                 "Blade count",
                                 camera.iris_blade_count,
                                 0.0,
                                 64.0,
                                 1.0,
                                 0.0);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "iris_rotation_degrees",
                                 "Iris",
                                 "Iris rotation",
                                 camera.iris_rotation_degrees,
                                 -360.0,
                                 360.0,
                                 1.0,
                                 0.0,
                                 "deg");
  QtDockAddSliderGroupedProperty(panel,
                                 base + "iris_roundness",
                                 "Iris",
                                 "Iris roundness",
                                 camera.iris_roundness,
                                 0.0,
                                 1.0,
                                 0.01,
                                 1.0);
  QtDockAddSliderGroupedProperty(panel,
                                 base + "anamorphic_squeeze",
                                 "Iris",
                                 "Anamorphic squeeze",
                                 camera.anamorphic_squeeze,
                                 0.01,
                                 100.0,
                                 0.01,
                                 1.0);
}

void QtDockAddRow(QtDockPanelContent& panel, std::string row) {
  panel.rows.push_back(std::move(row));
}

void QtDockAddTreeRow(QtDockPanelContent& panel, QtDockTreeRow row) {
  panel.tree_rows.push_back(std::move(row));
}

const vkpt::editor::UiPanelState* FindQtLayoutPanel(
    const vkpt::editor::UiLayoutDocument& layout,
    std::string_view panel_id) {
  const auto it = std::find_if(layout.panels.begin(),
                               layout.panels.end(),
                               [&](const vkpt::editor::UiPanelState& panel) {
                                 return panel.id == panel_id;
                               });
  return it == layout.panels.end() ? nullptr : &*it;
}

QtDockPanelContent MakeQtDockPanel(const vkpt::editor::UiLayoutDocument& layout,
                                   std::string_view id,
                                   std::string_view title,
                                   bool default_visible,
                                   float default_width,
                                   float default_height) {
  QtDockPanelContent panel;
  panel.id = std::string(id);
  panel.title = std::string(title);
  panel.visible = default_visible;
  panel.width = default_width;
  panel.height = default_height;
  // Persisted layout state overrides only dock chrome and size; panel contents
  // are rebuilt every frame from the current scene/runtime model.
  if (const auto* state = FindQtLayoutPanel(layout, id)) {
    panel.visible = state->visible;
    panel.docked = state->docked;
    panel.floating = state->floating;
    panel.collapsed = state->collapsed;
    panel.width = state->width > 0.0f ? state->width : default_width;
    panel.height = state->height > 0.0f ? state->height : default_height;
  }
  return panel;
}

const vkpt::scene::SceneEntityDefinition* FindQtSceneEntity(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.entities.begin(),
                               document.entities.end(),
                               [&](const vkpt::scene::SceneEntityDefinition& entity) {
                                 return entity.id == id;
                               });
  return it == document.entities.end() ? nullptr : &*it;
}

const vkpt::scene::SceneSdfPrimitiveDefinition* FindQtSceneSdfPrimitive(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.sdf_primitives.begin(),
                               document.sdf_primitives.end(),
                               [&](const vkpt::scene::SceneSdfPrimitiveDefinition& primitive) {
                                 return primitive.id == id;
                               });
  return it == document.sdf_primitives.end() ? nullptr : &*it;
}

vkpt::scene::PhysicsBodyComponent QtDefaultDynamicPhysicsBody() {
  vkpt::scene::PhysicsBodyComponent body;
  body.enabled = true;
  body.mass = 1.0f;
  body.dynamic = true;
  body.body_type = "dynamic";
  body.shape = "box";
  body.friction = 0.5f;
  body.restitution = 0.0f;
  body.gravity_scale = 1.0f;
  body.trigger = false;
  body.allow_sleeping = true;
  body.continuous_collision = false;
  return body;
}

std::string QtEntityComponentSummary(const vkpt::scene::SceneEntityDefinition& entity) {
  std::vector<std::string> components;
  components.reserve(7u);
  if (entity.has_transform) components.push_back("Transform");
  if (entity.has_mesh) components.push_back("Mesh");
  if (entity.has_light) components.push_back("Light");
  if (entity.has_camera) components.push_back("Camera");
  if (!entity.script.script.empty()) components.push_back("Script");
  if (entity.has_physics_body) components.push_back(entity.physics_body.enabled ? "Physics" : "Physics Off");
  if (entity.has_audio_listener) components.push_back("Audio Listener");
  if (entity.has_audio_emitter) components.push_back("Audio Emitter");
  if (entity.has_ui_panel) components.push_back("UI Panel");
  return components.empty() ? "Entity" : QtDockJoin(components, ", ");
}

std::string QtEntityDisplayName(const vkpt::scene::SceneEntityDefinition& entity) {
  if (!entity.name.empty()) {
    return entity.name;
  }
  return "Entity " + std::to_string(entity.id);
}

vkpt::core::StableId QtPrimarySelectionId(const vkpt::editor::SelectionState& selection) {
  return selection.selected_entity_ids.empty() ? 0 : selection.selected_entity_ids.front();
}

const vkpt::editor::Bounds* FindQtSelectionBounds(
    const vkpt::editor::SelectionState& selection,
    vkpt::core::StableId entity_id) {
  for (const auto& bounds : selection.per_item_bounds) {
    if (bounds.entity_id == entity_id) {
      return &bounds.bounds;
    }
  }
  return nullptr;
}

void QtDockLimitRows(QtDockPanelContent& panel, std::size_t max_rows) {
  if (panel.rows.size() > max_rows) {
    panel.rows.resize(max_rows);
  }
  if (panel.tree_rows.size() > max_rows) {
    panel.tree_rows.resize(max_rows);
  }
}

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
