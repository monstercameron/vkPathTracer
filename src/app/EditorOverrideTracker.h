#pragma once

#include "core/Types.h"
#include "scene/SceneTypes.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vkpt::app {

inline std::string_view editor_override_component_name(vkpt::scene::ComponentKind kind) noexcept {
  switch (kind) {
    case vkpt::scene::ComponentKind::Identity:
      return "Identity";
    case vkpt::scene::ComponentKind::Transform:
      return "Transform";
    case vkpt::scene::ComponentKind::Hierarchy:
      return "Hierarchy";
    case vkpt::scene::ComponentKind::Camera:
      return "Camera";
    case vkpt::scene::ComponentKind::Light:
      return "Light";
    case vkpt::scene::ComponentKind::MeshRenderer:
      return "MeshRenderer";
    case vkpt::scene::ComponentKind::SdfPrimitive:
      return "SDFPrimitive";
    case vkpt::scene::ComponentKind::MaterialOverride:
      return "MaterialOverride";
    case vkpt::scene::ComponentKind::PhysicsBody:
      return "PhysicsBody";
    case vkpt::scene::ComponentKind::Script:
      return "Script";
    case vkpt::scene::ComponentKind::AudioListener:
      return "AudioListener";
    case vkpt::scene::ComponentKind::AudioEmitter:
      return "AudioEmitter";
    case vkpt::scene::ComponentKind::UiPanel:
      return "UiPanel";
    case vkpt::scene::ComponentKind::BenchmarkTag:
      return "BenchmarkTag";
    case vkpt::scene::ComponentKind::Count:
      return {};
  }
  return {};
}

struct EditorOverrideKey {
  vkpt::core::StableId entity = 0;
  std::string component;

  friend bool operator==(const EditorOverrideKey& lhs, const EditorOverrideKey& rhs) {
    return lhs.entity == rhs.entity && lhs.component == rhs.component;
  }
};

struct EditorOverrideKeyHash {
  std::size_t operator()(const EditorOverrideKey& key) const {
    const auto entity_hash = std::hash<vkpt::core::StableId>{}(key.entity);
    const auto component_hash = std::hash<std::string>{}(key.component);
    return entity_hash ^ (component_hash + 0x9e3779b97f4a7c15ull +
                          (entity_hash << 6) + (entity_hash >> 2));
  }
};

struct ActiveEditorOverride {
  EditorOverrideKey key;
  std::string source;
  vkpt::core::FrameIndex frame = 0;
};

struct EditorOverrideDiagnostic {
  vkpt::core::StableId entity = 0;
  std::string component;
  std::string source;
  vkpt::core::FrameIndex frame = 0;
  std::string active_source;
  vkpt::core::FrameIndex active_frame = 0;
};

class EditorOverrideTracker {
 public:
  bool begin_override(vkpt::core::StableId entity, std::string_view component,
                      std::string_view source, vkpt::core::FrameIndex frame) {
    const auto key = make_key(entity, component);
    if (!key) {
      return false;
    }
    m_active[*key] = ActiveEditorOverride{*key, std::string(source), frame};
    return true;
  }

  bool begin_override(vkpt::core::StableId entity,
                      vkpt::scene::ComponentKind component,
                      std::string_view source, vkpt::core::FrameIndex frame) {
    return begin_override(entity, editor_override_component_name(component),
                          source, frame);
  }

  bool end_override(vkpt::core::StableId entity, std::string_view component) {
    const auto key = make_key(entity, component);
    if (!key) {
      return false;
    }
    return m_active.erase(*key) > 0;
  }

  bool end_override(vkpt::core::StableId entity,
                    vkpt::scene::ComponentKind component) {
    return end_override(entity, editor_override_component_name(component));
  }

  bool has_override(vkpt::core::StableId entity,
                    std::string_view component) const {
    return active_override(entity, component).has_value();
  }

  bool has_override(vkpt::core::StableId entity,
                    vkpt::scene::ComponentKind component) const {
    return has_override(entity, editor_override_component_name(component));
  }

  bool should_suppress_script_command(vkpt::core::StableId entity,
                                      std::string_view component) const {
    return has_override(entity, component);
  }

  bool should_suppress_script_command(vkpt::core::StableId entity,
                                      vkpt::scene::ComponentKind component) const {
    return should_suppress_script_command(entity, editor_override_component_name(component));
  }

  std::optional<EditorOverrideDiagnostic> make_diagnostic(vkpt::core::StableId entity,
                                                          std::string_view component,
                                                          std::string_view source,
                                                          vkpt::core::FrameIndex frame) const {
    const auto active = active_override(entity, component);
    if (!active) {
      return std::nullopt;
    }
    return EditorOverrideDiagnostic{
        active->key.entity,
        active->key.component,
        std::string(source),
        frame,
        active->source,
        active->frame,
    };
  }

  std::optional<EditorOverrideDiagnostic> make_diagnostic(vkpt::core::StableId entity,
                                                          vkpt::scene::ComponentKind component,
                                                          std::string_view source,
                                                          vkpt::core::FrameIndex frame) const {
    return make_diagnostic(entity, editor_override_component_name(component), source, frame);
  }

  std::optional<EditorOverrideDiagnostic> record_suppressed_script_command(
      vkpt::core::StableId entity, std::string_view component,
      std::string_view source, vkpt::core::FrameIndex frame) {
    auto diagnostic = make_diagnostic(entity, component, source, frame);
    if (diagnostic) {
      m_diagnostics.push_back(*diagnostic);
    }
    return diagnostic;
  }

  std::optional<EditorOverrideDiagnostic> record_suppressed_script_command(
      vkpt::core::StableId entity, vkpt::scene::ComponentKind component,
      std::string_view source, vkpt::core::FrameIndex frame) {
    return record_suppressed_script_command(entity, editor_override_component_name(component), source, frame);
  }

  std::optional<ActiveEditorOverride> active_override(vkpt::core::StableId entity,
                                                      std::string_view component) const {
    const auto key = make_key(entity, component);
    if (!key) {
      return std::nullopt;
    }
    const auto it = m_active.find(*key);
    if (it == m_active.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  std::optional<ActiveEditorOverride> active_override(vkpt::core::StableId entity,
                                                      vkpt::scene::ComponentKind component) const {
    return active_override(entity, editor_override_component_name(component));
  }

  const std::vector<EditorOverrideDiagnostic>& diagnostics() const {
    return m_diagnostics;
  }

  std::size_t active_count() const { return m_active.size(); }

  void clear_diagnostics() { m_diagnostics.clear(); }

  void clear() {
    m_active.clear();
    m_diagnostics.clear();
  }

 private:
  static std::optional<EditorOverrideKey> make_key(vkpt::core::StableId entity,
                                                   std::string_view component) {
    if (entity == 0 || component.empty()) {
      return std::nullopt;
    }
    return EditorOverrideKey{entity, std::string(component)};
  }

  std::unordered_map<EditorOverrideKey, ActiveEditorOverride, EditorOverrideKeyHash> m_active;
  std::vector<EditorOverrideDiagnostic> m_diagnostics;
};

}  // namespace vkpt::app
