#include "audio/AudioSystem.h"
#include "core/Logging.h"
#include "core/contracts/Determinism.h"
#include "jobs/JobSystem.h"
#include "pathtracer/PathTracer.h"
#include "physics/PhysicsWorld.h"
#include "scripting/ScriptRuntime.h"

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "determinism propagation smoke failed: " << message << "\n";
    return false;
  }
  return true;
}

bool HasField(const vkpt::log::LogEvent& event,
              const std::string& key,
              const std::string& value) {
  for (const auto& field : event.fields) {
    if (field.key == key && field.value == value) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  auto sink = std::make_unique<vkpt::log::RingBufferSink>(128u);
  auto* sink_ptr = sink.get();
  vkpt::log::Logger::instance().add_sink(std::move(sink));

  const auto context =
      vkpt::core::MakeDeterminismContext(true, 0xD37E500Du, 7u, "determinism-smoke");

  vkpt::core::EmitDeterminismChanged("app", context);

  vkpt::pathtracer::RenderSettings render_settings;
  render_settings.set_determinism(context);
  render_settings.set_determinism(context);

  vkpt::pathtracer::PathTraceSettings path_trace_settings;
  path_trace_settings.set_determinism(context);
  path_trace_settings.set_determinism(context);

  vkpt::scripting::ScriptExecutionContext script_context;
  script_context.set_determinism(context);
  script_context.set_determinism(context);

  vkpt::physics::PhysicsStepConfig physics_config;
  physics_config.set_determinism(context);
  physics_config.set_determinism(context);

  vkpt::audio::AudioSystemConfig audio_config;
  audio_config.set_determinism(context);
  audio_config.set_determinism(context);

  vkpt::jobs::JobSystem jobs(1u);
  jobs.set_determinism(context);
  jobs.set_determinism(context);
  const auto job_status = jobs.status();
  jobs.shutdown();

  const std::vector<std::string> expected_events = {
      "app.determinism_changed",
      "render.determinism_changed",
      "pathtracer.determinism_changed",
      "scripts.determinism_changed",
      "physics.determinism_changed",
      "audio.determinism_changed",
      "jobs.determinism_changed",
  };

  std::unordered_map<std::string, std::size_t> counts;
  for (const auto& event : sink_ptr->snapshot()) {
    ++counts[event.message];
    if (event.message.find(".determinism_changed") != std::string::npos) {
      if (!Check(HasField(event, "enabled", "true"),
                 "determinism event should include enabled=true") ||
          !Check(HasField(event, "base_seed", std::to_string(context.base_seed)),
                 "determinism event should include base_seed") ||
          !Check(HasField(event, "frame_index", std::to_string(context.frame_index)),
                 "determinism event should include frame_index") ||
          !Check(HasField(event, "scenario_id", context.scenario_id),
                 "determinism event should include scenario_id")) {
        return 1;
      }
    }
  }

  for (const auto& event_name : expected_events) {
    if (!Check(counts[event_name] == 1u,
               ("expected exactly one " + event_name + " event").c_str())) {
      return 1;
    }
  }

  if (!Check(render_settings.determinism_context() == context,
             "RenderSettings should retain startup determinism context") ||
      !Check(path_trace_settings.determinism_context() == context,
             "PathTraceSettings should retain startup determinism context") ||
      !Check(script_context.determinism_context() == context,
             "ScriptExecutionContext should retain startup determinism context") ||
      !Check(physics_config.determinism_context() == context,
             "PhysicsStepConfig should retain startup determinism context") ||
      !Check(audio_config.determinism_context() == context,
             "AudioSystemConfig should retain startup determinism context") ||
      !Check(job_status.deterministic &&
                 job_status.determinism_base_seed == context.base_seed &&
                 job_status.determinism_frame_index == context.frame_index &&
                 job_status.determinism_scenario_id == context.scenario_id &&
                 job_status.current_flow_id == context.frame_index,
             "JobSystemStatus should retain startup determinism context and flow id")) {
    return 1;
  }

  std::cout << "determinism propagation smoke: ok\n";
  return 0;
}
