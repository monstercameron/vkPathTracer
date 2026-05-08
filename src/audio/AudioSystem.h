#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/contracts/Determinism.h"
#include "core/contracts/Lifecycle.h"
#include "core/contracts/Result.h"
#include "core/Types.h"
#include "scene/SceneDocument.h"
#include "scene/SceneTypes.h"

namespace vkpt::scene {
struct RenderSceneSnapshot;
class SnapshotRing;
}

namespace vkpt::audio {

inline constexpr std::string_view kAudioSubsystemName = "audio";
inline constexpr std::string_view kAudioStatusTypeName = "AudioStatus";
inline constexpr std::string_view kAudioSnapshotConsumerContractName =
    "audio.snapshot_consumer.v1";
inline constexpr std::string_view kAudioCommandRingContractName = "audio.sound_ring.v1";

struct AudioNamingContract {
  std::string_view subsystem_name = kAudioSubsystemName;
  std::string_view status_type_name = kAudioStatusTypeName;
  std::string_view snapshot_consumer_contract = kAudioSnapshotConsumerContractName;
  std::string_view command_ring_contract = kAudioCommandRingContractName;
  std::string_view health_probe_name = kAudioSubsystemName;
  std::string_view lifecycle_field_name = "lifecycle";
  std::string_view last_error_field_name = "last_error";
  std::string_view flow_field_name = "current_flow_id";
};

inline constexpr AudioNamingContract kAudioNamingContract{};

[[nodiscard]] inline constexpr std::string_view AudioSubsystemName() noexcept {
  return kAudioSubsystemName;
}

using AudioClipId = std::uint64_t;
using AudioStreamId = std::uint64_t;
using AudioEventId = std::uint64_t;
using AudioBusId = std::uint64_t;
using AudioListenerId = std::uint64_t;

struct AudioVoiceHandle {
  std::uint32_t slot = 0;
  std::uint32_t generation = 0;

  explicit operator bool() const {
    return slot != 0 && generation != 0;
  }
};

struct AudioSystemConfig {
  std::string backend = "auto";
  bool muted = false;
  std::uint32_t sample_rate = 44100;
  std::uint32_t channels = 2;
  std::uint32_t buffer_frames = 256;
  std::uint32_t queued_buffers = 3;
  std::size_t max_voices = 96;
  bool deterministic = true;
  std::uint64_t determinism_base_seed = 0u;
  vkpt::core::FrameIndex determinism_frame_index = 0u;
  std::string determinism_scenario_id;

  void set_determinism(const vkpt::core::DeterminismContext& context) {
    const auto previous = determinism_context();
    deterministic = context.enabled;
    determinism_base_seed = context.base_seed;
    determinism_frame_index = context.frame_index;
    determinism_scenario_id = context.scenario_id;
    vkpt::core::EmitDeterminismChangedIfNeeded("audio", previous, determinism_context());
  }

  vkpt::core::DeterminismContext determinism_context() const {
    return vkpt::core::MakeDeterminismContext(deterministic,
                                              determinism_base_seed,
                                              determinism_frame_index,
                                              determinism_scenario_id);
  }
};

struct AudioListenerState {
  vkpt::scene::Vec3 position{0.0f, 0.0f, 0.0f};
  vkpt::scene::Vec3 forward{0.0f, 0.0f, -1.0f};
  vkpt::scene::Vec3 up{0.0f, 1.0f, 0.0f};
};

using AudioEventHandle = AudioVoiceHandle;

struct AudioOneShotEventDesc {
  std::string event_name;
  vkpt::core::StableEntityId entity = 0;
  vkpt::scene::Vec3 position{0.0f, 0.0f, 0.0f};
  std::string bus;
  bool has_position = false;
  bool spatial = false;
  float volume = 1.0f;
  float pitch = 1.0f;
  float priority = 0.5f;
};

struct AudioTrackedEventDesc : AudioOneShotEventDesc {
  bool loop = false;
};

struct AudioEventState {
  AudioEventHandle handle;
  std::string event_name;
  bool valid = false;
  bool playing = false;
  bool loop = false;
  bool stolen = false;
  float volume = 1.0f;
};

struct AudioBusDiagnostics {
  std::string name;
  float volume = 1.0f;
  bool muted = false;
  bool solo = false;
  std::uint64_t play_requests = 0;
};

struct AudioVoiceDiagnostics {
  AudioVoiceHandle handle;
  std::string event_name;
  std::string clip_uri;
  std::string bus;
  vkpt::core::StableEntityId entity = 0;
  bool playing = false;
  bool loop = false;
  bool spatial = false;
  bool stolen = false;
  float volume = 1.0f;
  float pitch = 1.0f;
  float pan = 0.0f;
  float gain = 1.0f;
  float priority = 0.5f;
};

struct AudioDiagnostics {
  std::string backend_name = "none";
  std::string device_name = "none";
  std::string state = "uninitialized";
  std::string last_error;
  bool initialized = false;
  bool muted = false;
  std::uint32_t sample_rate = 0;
  std::uint32_t channels = 0;
  std::size_t loaded_clips = 0;
  std::size_t loaded_streams = 0;
  std::size_t events = 0;
  std::size_t active_voices = 0;
  std::size_t streaming_voices = 0;
  std::size_t queued_commands = 0;
  std::size_t event_history_size = 0;
  std::uint64_t play_requests = 0;
  std::uint64_t stop_requests = 0;
  std::uint64_t failed_play_requests = 0;
  std::uint64_t stolen_voices = 0;
  std::uint64_t dropped_commands = 0;
  std::uint64_t listener_updates = 0;
  std::uint64_t mixed_buffers = 0;
  std::uint64_t underruns = 0;
  std::uint64_t decode_jobs_completed = 0;
  std::uint64_t stream_pages_produced = 0;
  std::uint64_t stream_pages_consumed = 0;
  std::uint64_t stream_ring_underruns = 0;
  std::vector<AudioBusDiagnostics> buses;
  std::vector<AudioVoiceDiagnostics> voices;
  std::vector<std::string> event_history;
};

struct AudioStatus {
  std::string name = std::string(kAudioSubsystemName);
  vkpt::core::contracts::ComponentLifecycle lifecycle =
      vkpt::core::contracts::ComponentLifecycle::Uninitialized;
  std::string backend_name = "none";
  std::string device_name = "none";
  std::string state = "uninitialized";
  std::string last_error;
  std::uint64_t last_tick_ns = 0;
  std::uint64_t ticks_total = 0;
  std::uint64_t errors_total = 0;
  bool initialized = false;
  bool muted = false;
  std::uint32_t sample_rate = 0;
  std::uint32_t channels = 0;
  std::uint32_t buffer_frames = 0;
  std::size_t loaded_clips = 0;
  std::size_t events = 0;
  std::size_t active_voices = 0;
  std::size_t voices_max = 0;
  std::size_t queued_commands = 0;
  std::uint64_t play_requests = 0;
  std::uint64_t failed_play_requests = 0;
  std::uint64_t dropped_commands = 0;
  std::uint64_t underruns = 0;
  std::uint64_t last_underrun_ns = 0;
  std::uint64_t last_callback_start_ns = 0;
  std::uint64_t snapshot_generation = 0;
  std::uint64_t snapshot_age_ms = 0;
  std::uint64_t current_flow_id = 0;
};

using AudioSystemStatus = AudioStatus;
using AudioSystemDiagnostics = AudioDiagnostics;

class IAudioBackend {
 public:
  virtual ~IAudioBackend() = default;
};

class IAudioAssetStore {
 public:
  virtual ~IAudioAssetStore() = default;
};

class IAudioCommandQueue {
 public:
  virtual ~IAudioCommandQueue() = default;
};

class IAudioDiagnostics {
 public:
  virtual ~IAudioDiagnostics() = default;
  virtual AudioDiagnostics diagnostics() const = 0;
};

class IAudioSystem : public IAudioDiagnostics {
 public:
  virtual ~IAudioSystem() = default;

  // State machine:
  // state\method    initialize load_scene_audio post_* set_snapshot consume_snapshot update shutdown
  // Uninitialized   ->Ready    error            error  error        error            noop   noop
  // Ready           noop       ok               ok     ok           ok               ok     ->Uninitialized
  // Degraded        noop       ok               ok     ok           ok               ok     ->Uninitialized
  // Failed          error      error            error  error        error            noop   ->Uninitialized
  virtual vkpt::core::Status initialize() = 0;
  virtual void shutdown() = 0;
  virtual vkpt::core::Status load_scene_audio(const vkpt::scene::SceneDocument& document,
                                              std::string_view scene_path) = 0;
  virtual vkpt::core::Status post_one_shot_event(const AudioOneShotEventDesc& desc) = 0;
  virtual AudioEventHandle post_tracked_event(const AudioTrackedEventDesc& desc) = 0;
  virtual void cancel(AudioEventHandle handle) = 0;
  virtual void set_volume(AudioEventHandle handle, float volume) = 0;
  virtual AudioEventState query_state(AudioEventHandle handle) const = 0;
  virtual void stop(AudioEventHandle handle) = 0;
  virtual void stop_all() = 0;
  virtual void set_listener(const AudioListenerState& listener) = 0;
  virtual void set_bus_volume(std::string_view bus, float volume) = 0;
  virtual void set_bus_muted(std::string_view bus, bool muted) = 0;
  virtual void set_snapshot_ring(vkpt::scene::SnapshotRing* snapshots) = 0;
  virtual void consume_snapshot(const vkpt::scene::RenderSceneSnapshot& snapshot) = 0;
  virtual void update() = 0;
  virtual AudioStatus status() const = 0;
  virtual AudioDiagnostics diagnostics() const = 0;
};

std::unique_ptr<IAudioSystem> CreateAudioSystem(AudioSystemConfig config = {});
IAudioSystem* GlobalAudioSystem();
void SetGlobalAudioSystem(IAudioSystem* system);

}  // namespace vkpt::audio
