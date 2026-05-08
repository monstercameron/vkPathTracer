#include "editor/QtPanelModels.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#include "render/DebugViews.h"

namespace vkpt::editor {
namespace {

std::string BoolText(bool value) { return value ? "true" : "false"; }

std::string IdText(vkpt::core::StableId id) { return id == 0 ? std::string("none") : std::to_string(id); }

std::string FloatText(double value, int precision = 3) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

std::string ResolutionText(std::uint32_t width, std::uint32_t height) {
  return std::to_string(width) + " x " + std::to_string(height);
}

QtPanelProperty Property(std::string id, std::string label, std::string value, std::string group = {},
                         QtPanelPropertyKind kind = QtPanelPropertyKind::Text, bool editable = false) {
  QtPanelProperty property;
  property.id = std::move(id);
  property.label = std::move(label);
  property.value = std::move(value);
  property.group = std::move(group);
  property.kind = kind;
  property.editable = editable;
  return property;
}

QtPanelRow Row(std::string id, std::string label, std::string detail = {}, std::string icon = {},
               std::uint32_t depth = 0) {
  QtPanelRow row;
  row.id = std::move(id);
  row.label = std::move(label);
  row.detail = std::move(detail);
  row.icon = std::move(icon);
  row.depth = depth;
  return row;
}

void AddProperty(QtPanelModel& model, std::string id, std::string label, std::string value, std::string group = {},
                 QtPanelPropertyKind kind = QtPanelPropertyKind::Text, bool editable = false) {
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
  return entity.name.empty() ? "Entity " + IdText(entity.id) : entity.name;
}

QtPanelModel MakeModel(std::string panel_id, std::string title) {
  QtPanelModel model;
  model.panel_id = std::move(panel_id);
  model.title = std::move(title);
  return model;
}

}  // namespace

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
  if (context.benchmark != nullptr) {
    const auto& desc = context.benchmark->run_desc;
    auto row = Row("timeline.benchmark", "Benchmark Run", desc.scene_path, "benchmark");
    row.properties.push_back(Property("benchmark.duration", "Duration", FloatText(desc.duration), "Benchmark",
                                      QtPanelPropertyKind::Number));
    row.properties.push_back(Property("benchmark.warmup", "Warmup Frames", std::to_string(desc.warmup_frames),
                                      "Benchmark", QtPanelPropertyKind::Number));
    model.rows.push_back(std::move(row));
  }
  model.summary = model.rows.empty() ? "Static scene timeline" : std::to_string(model.rows.size()) + " timeline rows";
  return model;
}

QtPanelModel BuildScriptingPanelModel(const QtPanelBuildContext& context) {
  auto model = MakeModel("script_panel", "Scripting");
  const std::string runtimeMode = context.script_runtime_mode.empty()
      ? std::string("edit")
      : context.script_runtime_mode;
  const std::string runtimeStatus = context.script_runtime_status.empty()
      ? (context.runtime != nullptr && !context.runtime->status_message.empty()
             ? context.runtime->status_message
             : std::string("idle"))
      : context.script_runtime_status;

  AddProperty(model, "script.runtime.mode", "Runtime Mode", runtimeMode, "Runtime");
  AddProperty(model, "script.runtime.status", "Runtime Status", runtimeStatus, "Runtime");
  AddProperty(model,
              "script.runtime.viewport_input",
              "Viewport Input",
              BoolText(context.script_viewport_input_forwarding),
              "Runtime",
              QtPanelPropertyKind::Toggle);
  AddProperty(model, "script.runtime.run_live", "Run Live", "Run Live", "Live/Play",
              QtPanelPropertyKind::Command);
  AddProperty(model, "script.runtime.play", "Play", "Play", "Live/Play",
              QtPanelPropertyKind::Command);
  AddProperty(model, "script.runtime.stop", "Stop", "Stop", "Live/Play",
              QtPanelPropertyKind::Command);
  AddProperty(model,
              "script.runtime.send_viewport_input",
              "Send Viewport Input",
              "Send",
              "Live/Play",
              QtPanelPropertyKind::Command);
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


}  // namespace vkpt::editor
