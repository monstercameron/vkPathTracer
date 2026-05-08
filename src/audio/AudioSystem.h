#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/Types.h"
#include "scene/SceneDocument.h"
#include "scene/SceneTypes.h"

namespace vkpt::audio {

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
  std::uint32_t buffer_frames = 1024;
  std::uint32_t queued_buffers = 3;
  std::size_t max_voices = 96;
  bool deterministic = true;
};

struct AudioListenerState {
  vkpt::scene::Vec3 position{0.0f, 0.0f, 0.0f};
  vkpt::scene::Vec3 forward{0.0f, 0.0f, -1.0f};
  vkpt::scene::Vec3 up{0.0f, 1.0f, 0.0f};
};

struct AudioPostEventDesc {
  std::string event_name;
  vkpt::core::StableEntityId entity = 0;
  vkpt::scene::Vec3 position{0.0f, 0.0f, 0.0f};
  std::string bus;
  bool has_position = false;
  bool spatial = false;
  bool loop = false;
  float volume = 1.0f;
  float pitch = 1.0f;
  float priority = 0.5f;
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
  std::vector<AudioBusDiagnostics> buses;
  std::vector<AudioVoiceDiagnostics> voices;
  std::vector<std::string> event_history;
};

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

  virtual bool initialize() = 0;
  virtual void shutdown() = 0;
  virtual bool load_scene_audio(const vkpt::scene::SceneDocument& document,
                                std::string_view scene_path) = 0;
  virtual AudioVoiceHandle post_event(const AudioPostEventDesc& desc) = 0;
  virtual void stop(AudioVoiceHandle handle) = 0;
  virtual void stop_all() = 0;
  virtual void set_listener(const AudioListenerState& listener) = 0;
  virtual void set_bus_volume(std::string_view bus, float volume) = 0;
  virtual void set_bus_muted(std::string_view bus, bool muted) = 0;
  virtual void update() = 0;
  virtual AudioDiagnostics diagnostics() const = 0;
};

std::unique_ptr<IAudioSystem> CreateAudioSystem(AudioSystemConfig config = {});
IAudioSystem* GlobalAudioSystem();
void SetGlobalAudioSystem(IAudioSystem* system);

}  // namespace vkpt::audio
