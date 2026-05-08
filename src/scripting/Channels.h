#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include "core/sync/SpscRing.h"
#include "physics/Channels.h"
#include "scene/SceneWorld.h"
#include "scripting/ScriptRuntime.h"

namespace vkpt::scripting {

struct ScriptHookRequest {
  vkpt::core::StableEntityId entity = 0;
  ScriptLifecycleHook hook = ScriptLifecycleHook::OnUpdate;
  vkpt::core::FrameIndex frame = 0;
  ScriptExecutionContext context;
};

struct ScriptPlaySoundCmd {
  std::string event_name;
  vkpt::core::StableEntityId entity = 0;
  vkpt::scene::Vec3 position{};
  bool has_position = false;
  float volume = 1.0f;
};

struct ScriptEmitParticleCmd {
  std::string effect_name;
  vkpt::scene::Vec3 position{};
};

struct ScriptCmd {
  enum class Type : std::uint8_t {
    SpawnEntity,
    DestroyEntity,
    SetTransform,
    ApplyForce,
    PlaySound,
    SetMaterial,
    EmitParticle,
    SceneCommand,
  };

  Type type = Type::SceneCommand;
  std::variant<
      vkpt::scene::WorldCommandBuffer::Command,
      vkpt::physics::PhysicsApplyForceCmd,
      ScriptPlaySoundCmd,
      ScriptEmitParticleCmd> payload;
};

using ScriptHookRing = vkpt::core::sync::SpscRing<ScriptHookRequest>;
using ScriptCmdRing = vkpt::core::sync::SpscRing<ScriptCmd>;

ScriptCmd MakeScriptCmd(const vkpt::scene::WorldCommandBuffer::Command& command);
bool AppendScriptCmdToWorldCommands(const ScriptCmd& cmd, vkpt::scene::WorldCommandBuffer& commands);

class ScriptThread {
 public:
  explicit ScriptThread(std::size_t hook_capacity = 1024u, std::size_t command_capacity = 4096u);
  ~ScriptThread();

  ScriptThread(const ScriptThread&) = delete;
  ScriptThread& operator=(const ScriptThread&) = delete;

  void publish_world_snapshot(vkpt::scene::SceneWorld world);
  bool enqueue_hook(ScriptHookRequest request);
  std::size_t drain_commands(vkpt::scene::WorldCommandBuffer& commands);
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace vkpt::scripting
