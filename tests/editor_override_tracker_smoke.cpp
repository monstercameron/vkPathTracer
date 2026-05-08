#include "app/EditorOverrideTracker.h"

#include <iostream>
#include <string>

namespace {

bool Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "editor override tracker smoke failed: " << message << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  vkpt::app::EditorOverrideTracker tracker;

  if (!Check(!tracker.begin_override(0, vkpt::scene::ComponentKind::Transform,
                                     "gizmo", 9),
             "entity id zero should not create an override") ||
      !Check(!tracker.begin_override(42, "", "gizmo", 9),
             "empty component strings should not create an override") ||
      !Check(tracker.active_count() == 0u,
             "invalid overrides should leave the tracker empty")) {
    return 1;
  }

  if (!Check(tracker.begin_override(42, vkpt::scene::ComponentKind::Transform,
                                    "gizmo", 10),
             "transform override should begin") ||
      !Check(tracker.has_override(42, "Transform"),
             "kind overload should share the canonical string key") ||
      !Check(tracker.should_suppress_script_command(
                 42, vkpt::scene::ComponentKind::Transform),
             "matching script transform writes should be suppressed") ||
      !Check(!tracker.should_suppress_script_command(
                 42, vkpt::scene::ComponentKind::Camera),
             "other components on the same entity should not be suppressed") ||
      !Check(!tracker.should_suppress_script_command(
                 7, vkpt::scene::ComponentKind::Transform),
             "same component on another entity should not be suppressed")) {
    return 1;
  }

  auto diagnostic = tracker.record_suppressed_script_command(
      42, vkpt::scene::ComponentKind::Transform, "lua:on_update", 11);
  if (!Check(diagnostic.has_value(),
             "suppressed script command should produce a diagnostic") ||
      !Check(diagnostic->entity == 42, "diagnostic should retain entity id") ||
      !Check(diagnostic->component == "Transform",
             "diagnostic should retain component") ||
      !Check(diagnostic->source == "lua:on_update",
             "diagnostic should retain script command source") ||
      !Check(diagnostic->frame == 11,
             "diagnostic should retain script command frame") ||
      !Check(diagnostic->active_source == "gizmo",
             "diagnostic should retain active editor source") ||
      !Check(diagnostic->active_frame == 10,
             "diagnostic should retain active editor frame") ||
      !Check(tracker.diagnostics().size() == 1u,
             "recorded diagnostics should be retained")) {
    return 1;
  }

  const auto no_diagnostic = tracker.record_suppressed_script_command(
      42, vkpt::scene::ComponentKind::Light, "lua:on_update", 12);
  if (!Check(
          !no_diagnostic.has_value(),
          "non-conflicting script command should not produce a diagnostic") ||
      !Check(tracker.diagnostics().size() == 1u,
             "non-conflicting commands should not grow diagnostic history")) {
    return 1;
  }

  if (!Check(tracker.begin_override(42, "Transform", "property_panel", 13),
             "restarting an active override should update owner metadata")) {
    return 1;
  }
  diagnostic = tracker.make_diagnostic(42, "Transform", "lua:late_update", 14);
  if (!Check(diagnostic.has_value() &&
                 diagnostic->active_source == "property_panel" &&
                 diagnostic->active_frame == 13,
             "diagnostic should use the latest active override metadata")) {
    return 1;
  }

  if (!Check(tracker.end_override(42, vkpt::scene::ComponentKind::Transform),
             "ending an active override should return true") ||
      !Check(!tracker.end_override(42, vkpt::scene::ComponentKind::Transform),
             "ending a missing override should return false") ||
      !Check(!tracker.should_suppress_script_command(42, "Transform"),
             "ended override should stop suppressing script writes")) {
    return 1;
  }

  if (!Check(tracker.begin_override(77, vkpt::scene::ComponentKind::Light,
                                    "light_panel", 20),
             "light override should begin") ||
      !Check(tracker.should_suppress_script_command(77, "Light"),
             "string queries should match kind-created light overrides")) {
    return 1;
  }

  tracker.clear_diagnostics();
  if (!Check(tracker.diagnostics().empty(),
             "clear_diagnostics should preserve active overrides but clear "
             "history") ||
      !Check(tracker.active_count() == 1u,
             "clear_diagnostics should not remove active overrides")) {
    return 1;
  }

  tracker.clear();
  if (!Check(tracker.active_count() == 0u,
             "clear should remove active overrides") ||
      !Check(tracker.diagnostics().empty(),
             "clear should remove diagnostics")) {
    return 1;
  }

  std::cout << "editor override tracker smoke: ok\n";
  return 0;
}
