#include "audio/AudioSystem.h"

#include "core/Logging.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <deque>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(PT_ENABLE_MINIAUDIO)
#include <miniaudio.h>
#endif

namespace vkpt::audio {
namespace {

constexpr float kPi = 3.14159265358979323846f;
std::atomic<IAudioSystem*> g_audioSystem{nullptr};

std::string LowerCopy(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

std::string NormalizeEventName(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool lastSeparator = false;
  for (const unsigned char c : text) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out.push_back(static_cast<char>(c));
      lastSeparator = false;
    } else if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(std::tolower(c)));
      lastSeparator = false;
    } else if (!lastSeparator && !out.empty()) {
      out.push_back('.');
      lastSeparator = true;
    }
  }
  while (!out.empty() && out.back() == '.') {
    out.pop_back();
  }
  return out.empty() ? "unnamed" : out;
}

std::uint64_t Fnv1a64(std::string_view text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash == 0 ? 1 : hash;
}

std::string EventNameFromUri(std::string_view uri) {
  const std::filesystem::path path{std::string(uri)};
  const auto stem = path.has_stem() ? path.stem().generic_string() : std::string(uri);
  return NormalizeEventName(stem);
}

std::filesystem::path ResolveUri(std::string_view uri, std::string_view scene_path) {
  const std::filesystem::path requested{std::string(uri)};
  if (requested.is_absolute()) {
    return requested.lexically_normal();
  }
  if (scene_path.empty()) {
    return requested.lexically_normal();
  }
  const std::filesystem::path scenePath{std::string(scene_path)};
  const auto base = scenePath.has_parent_path() ? scenePath.parent_path() : std::filesystem::current_path();
  return (base / requested).lexically_normal();
}

float BusDefaultGain(std::string_view bus) {
  const auto normalized = NormalizeEventName(bus);
  if (normalized == "music") return 0.55f;
  if (normalized == "ambience") return 0.45f;
  if (normalized == "ui") return 0.75f;
  if (normalized == "voice") return 0.85f;
  return 0.80f;
}

float Distance(const vkpt::scene::Vec3& a, const vkpt::scene::Vec3& b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  const float dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct SpatialMix {
  float gain = 1.0f;
  float pan = 0.0f;
  bool occluded = false;
};

SpatialMix CalculateSpatialMix(const AudioListenerState& listener,
                               const vkpt::scene::Vec3& position,
                               float minDistance,
                               float maxDistance) {
  SpatialMix mix;
  const float range = std::max(0.001f, maxDistance - minDistance);
  const float distance = Distance(listener.position, position);
  mix.gain = 1.0f - std::clamp((distance - minDistance) / range, 0.0f, 1.0f);
  const float dx = position.x - listener.position.x;
  mix.pan = std::clamp(dx / std::max(1.0f, maxDistance), -1.0f, 1.0f);
  mix.occluded = distance > (maxDistance * 0.75f);
  if (mix.occluded) {
    mix.gain *= 0.72f;
  }
  return mix;
}

void AppendSine(std::vector<float>& out,
                std::uint32_t sampleRate,
                float frequency,
                float seconds,
                float gain,
                float startPhase = 0.0f) {
  const auto frames = static_cast<std::size_t>(std::max(1.0f, seconds * static_cast<float>(sampleRate)));
  out.reserve(out.size() + frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sampleRate);
    const float env = std::sin(kPi * static_cast<float>(i) / static_cast<float>(frames));
    out.push_back(std::sin(startPhase + t * frequency * kPi * 2.0f) * gain * env);
  }
}

void AppendNoise(std::vector<float>& out,
                 std::uint32_t sampleRate,
                 float seconds,
                 float gain,
                 std::uint32_t seed) {
  const auto frames = static_cast<std::size_t>(std::max(1.0f, seconds * static_cast<float>(sampleRate)));
  std::uint32_t state = seed == 0 ? 1u : seed;
  out.reserve(out.size() + frames);
  for (std::size_t i = 0; i < frames; ++i) {
    state = state * 1664525u + 1013904223u;
    const float random = static_cast<float>((state >> 8u) & 0xffffu) / 32767.5f - 1.0f;
    const float attack = std::min(1.0f, static_cast<float>(i) / std::max(1.0f, frames * 0.08f));
    const float release = 1.0f - static_cast<float>(i) / static_cast<float>(frames);
    out.push_back(random * gain * attack * release);
  }
}

struct ClipDef {
  AudioClipId id = 0;
  std::string uri;
  std::string name;
  std::string file_path;
  std::uint32_t sample_rate = 44100;
  std::uint32_t channels = 1;
  bool generated = false;
  bool loop_default = false;
  std::vector<float> samples;

  std::size_t frame_count() const {
    return channels == 0 ? 0 : samples.size() / channels;
  }
};

ClipDef GenerateToneClip(std::string_view uri, std::uint32_t sampleRate) {
  ClipDef clip;
  clip.id = Fnv1a64(uri);
  clip.uri = std::string(uri);
  clip.name = EventNameFromUri(uri);
  clip.sample_rate = sampleRate;
  clip.channels = 1;
  clip.generated = true;

  const auto lower = LowerCopy(uri);
  auto appendMixed = [&](float sineHz, float sineSeconds, float sineGain,
                         float noiseSeconds, float noiseGain) {
    std::vector<float> sine;
    AppendSine(sine, sampleRate, sineHz, sineSeconds, sineGain);
    std::vector<float> noise;
    AppendNoise(noise, sampleRate, noiseSeconds, noiseGain, static_cast<std::uint32_t>(clip.id));
    const auto frames = std::max(sine.size(), noise.size());
    clip.samples.assign(frames, 0.0f);
    for (std::size_t i = 0; i < frames; ++i) {
      if (i < sine.size()) clip.samples[i] += sine[i];
      if (i < noise.size()) clip.samples[i] += noise[i];
      clip.samples[i] = std::clamp(clip.samples[i], -1.0f, 1.0f);
    }
  };

  if (lower.find("footstep") != std::string::npos) {
    appendMixed(95.0f, 0.11f, 0.35f, 0.075f, 0.22f);
  } else if (lower.find("rifle") != std::string::npos || lower.find("fire") != std::string::npos) {
    appendMixed(145.0f, 0.085f, 0.58f, 0.060f, 0.48f);
  } else if (lower.find("shotgun") != std::string::npos) {
    appendMixed(82.0f, 0.16f, 0.74f, 0.14f, 0.65f);
  } else if (lower.find("pickup") != std::string::npos) {
    AppendSine(clip.samples, sampleRate, 660.0f, 0.09f, 0.40f);
    AppendSine(clip.samples, sampleRate, 990.0f, 0.10f, 0.34f);
  } else if (lower.find("terminal") != std::string::npos ||
             lower.find("objective") != std::string::npos) {
    AppendSine(clip.samples, sampleRate, 440.0f, 0.10f, 0.36f);
    AppendSine(clip.samples, sampleRate, 330.0f, 0.12f, 0.32f);
  } else if (lower.find("radar") != std::string::npos ||
             lower.find("ping") != std::string::npos) {
    AppendSine(clip.samples, sampleRate, 880.0f, 0.12f, 0.32f);
  } else if (lower.find("ambient") != std::string::npos ||
             lower.find("wind") != std::string::npos ||
             lower.find("hum") != std::string::npos) {
    clip.loop_default = true;
    clip.samples.resize(sampleRate);
    std::uint32_t state = static_cast<std::uint32_t>(clip.id);
    for (std::size_t i = 0; i < clip.samples.size(); ++i) {
      state = state * 1664525u + 1013904223u;
      const float t = static_cast<float>(i) / static_cast<float>(sampleRate);
      const float noise = static_cast<float>((state >> 8u) & 0xffffu) / 32767.5f - 1.0f;
      clip.samples[i] = std::sin(t * 82.0f * kPi * 2.0f) * 0.08f + noise * 0.025f;
    }
  } else {
    float frequency = 520.0f;
    float seconds = 0.16f;
    const auto colon = lower.find(':');
    if (colon != std::string::npos) {
      std::stringstream parser(std::string(lower.substr(colon + 1u)));
      parser >> frequency;
      if (parser.good() && parser.peek() == ':') {
        parser.ignore();
        float ms = 0.0f;
        parser >> ms;
        if (ms > 0.0f) {
          seconds = ms / 1000.0f;
        }
      }
    }
    AppendSine(clip.samples, sampleRate, frequency, seconds, 0.36f);
  }

  if (clip.samples.empty()) {
    clip.samples.push_back(0.0f);
  }
  return clip;
}

struct EventDef {
  AudioEventId id = 0;
  std::string name;
  std::vector<AudioClipId> clips;
  std::string bus = "sfx";
  bool spatial = false;
  bool loop = false;
  float volume = 1.0f;
  float pitch = 1.0f;
  float min_distance = 1.0f;
  float max_distance = 24.0f;
  float priority = 0.5f;
};

struct BusState {
  std::string name;
  float volume = 1.0f;
  bool muted = false;
  bool solo = false;
  std::uint64_t play_requests = 0;
};

struct RuntimeVoice {
  AudioVoiceHandle handle;
  AudioClipId clip = 0;
  std::string event_name;
  bool playing = false;
  bool loop = false;
  bool spatial = false;
  bool stolen = false;
  vkpt::core::StableEntityId entity = 0;
  std::string bus = "sfx";
  std::string clip_uri;
  vkpt::scene::Vec3 position{0.0f, 0.0f, 0.0f};
  float volume = 1.0f;
  float pitch = 1.0f;
  float pan = 0.0f;
  float gain = 1.0f;
  float priority = 0.5f;

#if defined(PT_ENABLE_MINIAUDIO)
  ma_sound sound{};
  ma_audio_buffer buffer{};
  bool sound_initialized = false;
  bool buffer_initialized = false;

  RuntimeVoice() = default;
  RuntimeVoice(const RuntimeVoice&) = delete;
  RuntimeVoice& operator=(const RuntimeVoice&) = delete;
  ~RuntimeVoice() {
    if (sound_initialized) {
      ma_sound_uninit(&sound);
    }
    if (buffer_initialized) {
      ma_audio_buffer_uninit(&buffer);
    }
  }
#endif
};

class EngineAudioSystem final : public IAudioSystem {
 public:
  explicit EngineAudioSystem(AudioSystemConfig config) : m_config(std::move(config)) {}
  ~EngineAudioSystem() override {
    shutdown();
  }

  bool initialize() override {
    std::lock_guard lock(m_mutex);
    if (m_initialized) {
      return true;
    }

    m_config.sample_rate = std::max<std::uint32_t>(8000u, m_config.sample_rate);
    m_config.channels = std::clamp<std::uint32_t>(m_config.channels, 1u, 2u);
    m_diag = {};
    m_diag.sample_rate = m_config.sample_rate;
    m_diag.channels = m_config.channels;
    m_diag.muted = m_config.muted;
    m_diag.queued_commands = 0;
    m_diag.backend_name = select_backend_name();
    m_diag.device_name = "no output device";

#if defined(PT_ENABLE_MINIAUDIO)
    if (m_diag.backend_name == "miniaudio") {
      ma_engine_config engineConfig = ma_engine_config_init();
      engineConfig.channels = m_config.channels;
      engineConfig.sampleRate = m_config.sample_rate;
      engineConfig.listenerCount = 1;
      engineConfig.periodSizeInFrames = m_config.buffer_frames;
      const ma_result result = ma_engine_init(&engineConfig, &m_engine);
      if (result == MA_SUCCESS) {
        m_engineInitialized = true;
        m_diag.device_name = "miniaudio default device";
        ma_engine_set_volume(&m_engine, m_config.muted ? 0.0f : 1.0f);
      } else {
        m_diag.last_error = "ma_engine_init failed: " + std::to_string(static_cast<int>(result));
        vkpt::log::Logger::instance().log(vkpt::log::Severity::Warning, "audio", m_diag.last_error);
        m_diag.backend_name = "noop";
        m_diag.device_name = "no output device";
      }
    }
#endif

    m_initialized = true;
    m_diag.initialized = true;
    m_diag.state = m_diag.backend_name == "noop" ? "initialized_noop" : "initialized";
    set_listener_locked(m_listener);
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        "audio",
        "audio system initialized",
        {{"backend", m_diag.backend_name},
         {"device", m_diag.device_name},
         {"sample_rate", std::to_string(m_config.sample_rate)},
         {"channels", std::to_string(m_config.channels)},
         {"buffer_frames", std::to_string(m_config.buffer_frames)},
         {"queued_buffers", std::to_string(m_config.queued_buffers)},
         {"muted", m_config.muted ? "true" : "false"}});
    return true;
  }

  void shutdown() override {
    std::lock_guard lock(m_mutex);
    if (!m_initialized) {
      return;
    }
    m_voices.clear();
    append_history_locked("shutdown");
#if defined(PT_ENABLE_MINIAUDIO)
    if (m_engineInitialized) {
      ma_engine_uninit(&m_engine);
      m_engineInitialized = false;
    }
#endif
    m_initialized = false;
    m_diag.initialized = false;
    m_diag.active_voices = 0u;
    m_diag.state = "shutdown";
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Info, "audio", "audio system shutdown");
  }

  bool load_scene_audio(const vkpt::scene::SceneDocument& document,
                        std::string_view scene_path) override {
    std::lock_guard lock(m_mutex);
    m_clips.clear();
    m_clipByUri.clear();
    m_events.clear();
    m_voices.clear();
    m_eventHistory.clear();
    initialize_buses_locked();
    m_nextVoiceGeneration = 1u;
    m_nextVoiceSlot = 1u;

    std::size_t declaredAudioAssets = 0u;
    for (const auto& asset : document.assets) {
      const auto type = LowerCopy(asset.type);
      if (type.find("audio") == std::string::npos && type.find("sound") == std::string::npos) {
        continue;
      }
      ++declaredAudioAssets;
      const bool eventAsset = type.find("event") != std::string::npos ||
                              type.find("sfx") != std::string::npos ||
                              type.find("sound") != std::string::npos;
      const bool streamAsset = type.find("stream") != std::string::npos ||
                               type.find("music") != std::string::npos ||
                               type.find("ambience") != std::string::npos;
      const auto clipId = load_clip_locked(asset.uri, scene_path);
      if (clipId == 0) {
        continue;
      }
      EventDef event;
      event.name = NormalizeEventName(asset.name.empty() ? EventNameFromUri(asset.uri) : asset.name);
      event.id = Fnv1a64(event.name);
      event.clips.push_back(clipId);
      event.loop = streamAsset;
      event.spatial = type.find("spatial") != std::string::npos || type.find("emitter") != std::string::npos;
      if (type.find("ui") != std::string::npos) {
        event.bus = "ui";
      } else if (type.find("music") != std::string::npos) {
        event.bus = "music";
      } else if (streamAsset || type.find("ambience") != std::string::npos) {
        event.bus = "ambience";
      } else if (type.find("voice") != std::string::npos) {
        event.bus = "voice";
      } else {
        event.bus = "sfx";
      }
      event.priority = default_priority_for_bus(event.bus);
      if (eventAsset || streamAsset || !asset.name.empty()) {
        auto& stored = m_events[event.name];
        if (stored.name.empty()) {
          stored = std::move(event);
        } else {
          stored.clips.push_back(clipId);
          stored.loop = stored.loop || event.loop;
          stored.spatial = stored.spatial || event.spatial;
        }
      }
    }

    for (const auto& entity : document.entities) {
      if (entity.has_audio_emitter && entity.audio_emitter.enabled &&
          entity.audio_emitter.autoplay && !entity.audio_emitter.event.empty()) {
        AudioPostEventDesc desc;
        desc.event_name = entity.audio_emitter.event;
        desc.entity = entity.id;
        desc.spatial = entity.audio_emitter.spatial;
        desc.loop = entity.audio_emitter.loop;
        desc.volume = entity.audio_emitter.volume;
        desc.pitch = entity.audio_emitter.pitch;
        if (entity.has_transform) {
          desc.has_position = true;
          desc.position = entity.transform.translation;
        }
        post_event_locked(desc);
      }
      if (entity.has_audio_listener && entity.audio_listener.enabled && entity.has_transform) {
        m_listener.position = entity.transform.translation;
        set_listener_locked(m_listener);
      }
    }

    m_diag.loaded_clips = m_clips.size();
    m_diag.loaded_streams = static_cast<std::size_t>(std::count_if(m_events.begin(), m_events.end(), [](const auto& item) {
      return item.second.loop || item.second.bus == "music" || item.second.bus == "ambience";
    }));
    m_diag.events = m_events.size();
    m_diag.active_voices = active_voice_count_locked();
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        "audio",
        "scene audio loaded",
        {{"scene", std::string(scene_path)},
         {"declared_assets", std::to_string(declaredAudioAssets)},
         {"clips", std::to_string(m_clips.size())},
         {"events", std::to_string(m_events.size())},
         {"autoplay_voices", std::to_string(m_diag.active_voices)}});
    return true;
  }

  AudioVoiceHandle post_event(const AudioPostEventDesc& desc) override {
    std::lock_guard lock(m_mutex);
    return post_event_locked(desc);
  }

  void stop(AudioVoiceHandle handle) override {
    if (!handle) {
      return;
    }
    std::lock_guard lock(m_mutex);
    ++m_diag.stop_requests;
    for (auto& voice : m_voices) {
      if (voice && voice->handle.slot == handle.slot && voice->handle.generation == handle.generation) {
        stop_voice_locked(*voice);
      }
    }
    m_diag.active_voices = active_voice_count_locked();
  }

  void set_listener(const AudioListenerState& listener) override {
    std::lock_guard lock(m_mutex);
    m_listener = listener;
    ++m_diag.listener_updates;
    set_listener_locked(listener);
  }

  void set_bus_volume(std::string_view bus, float volume) override {
    std::lock_guard lock(m_mutex);
    auto& state = bus_state_locked(bus);
    state.volume = std::clamp(volume, 0.0f, 2.0f);
  }

  void set_bus_muted(std::string_view bus, bool muted) override {
    std::lock_guard lock(m_mutex);
    auto& state = bus_state_locked(bus);
    state.muted = muted;
  }

  void update() override {
    std::lock_guard lock(m_mutex);
    m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [&](const auto& voice) {
      return voice == nullptr || voice_finished_locked(*voice);
    }), m_voices.end());
    m_diag.active_voices = active_voice_count_locked();
    m_diag.loaded_clips = m_clips.size();
    m_diag.loaded_streams = static_cast<std::size_t>(std::count_if(m_events.begin(), m_events.end(), [](const auto& item) {
      return item.second.loop || item.second.bus == "music" || item.second.bus == "ambience";
    }));
    m_diag.events = m_events.size();
    ++m_diag.mixed_buffers;
  }

  AudioDiagnostics diagnostics() const override {
    std::lock_guard lock(m_mutex);
    auto out = m_diag;
    out.active_voices = active_voice_count_locked();
    out.loaded_clips = m_clips.size();
    out.events = m_events.size();
    out.queued_commands = 0;
    out.event_history_size = m_eventHistory.size();
    out.buses.clear();
    out.voices.clear();
    out.event_history.assign(m_eventHistory.begin(), m_eventHistory.end());
    for (const auto& item : m_buses) {
      out.buses.push_back(AudioBusDiagnostics{
          item.second.name,
          item.second.volume,
          item.second.muted,
          item.second.solo,
          item.second.play_requests});
    }
    for (const auto& voice : m_voices) {
      if (!voice || voice_finished_locked(*voice)) {
        continue;
      }
      out.voices.push_back(AudioVoiceDiagnostics{
          voice->handle,
          voice->event_name,
          voice->clip_uri,
          voice->bus,
          voice->entity,
          voice->playing,
          voice->loop,
          voice->spatial,
          voice->stolen,
          voice->volume,
          voice->pitch,
          voice->pan,
          voice->gain,
          voice->priority});
    }
    return out;
  }

 private:
  std::string select_backend_name() const {
    const auto requested = NormalizeEventName(m_config.backend.empty() ? "auto" : m_config.backend);
    if (requested == "noop" || requested == "none" || requested == "null" || m_config.muted) {
      return "noop";
    }
#if defined(PT_ENABLE_MINIAUDIO)
    if (requested == "auto" || requested == "miniaudio" || requested == "device" || requested == "system") {
      return "miniaudio";
    }
#endif
    return "noop";
  }

  AudioClipId load_clip_locked(std::string_view uri, std::string_view scene_path) {
    if (uri.empty()) {
      return 0;
    }
    const auto key = std::string(uri);
    const auto existing = m_clipByUri.find(key);
    if (existing != m_clipByUri.end()) {
      return existing->second;
    }

    ClipDef clip;
    if (LowerCopy(uri).starts_with("tone:")) {
      clip = GenerateToneClip(uri, m_config.sample_rate);
    } else {
      const auto resolved = ResolveUri(uri, scene_path);
      std::error_code ec;
      if (!std::filesystem::exists(resolved, ec) || ec) {
        m_diag.last_error = "audio asset missing, using generated placeholder: " + resolved.generic_string();
        vkpt::log::Logger::instance().log(vkpt::log::Severity::Warning, "audio", m_diag.last_error);
        clip = GenerateToneClip("tone:missing", m_config.sample_rate);
        clip.uri = std::string(uri);
        clip.name = EventNameFromUri(uri);
        clip.generated = true;
      } else {
        clip.id = Fnv1a64(resolved.generic_string());
        clip.uri = std::string(uri);
        clip.name = EventNameFromUri(uri);
        clip.file_path = resolved.generic_string();
        clip.sample_rate = m_config.sample_rate;
        clip.channels = m_config.channels;
      }
    }

    if (clip.id == 0) {
      clip.id = Fnv1a64(key);
    }
    m_clipByUri[key] = clip.id;
    m_clips[clip.id] = std::move(clip);
    return m_clipByUri[key];
  }

  AudioVoiceHandle allocate_handle_locked() {
    AudioVoiceHandle handle;
    handle.slot = m_nextVoiceSlot++;
    if (m_nextVoiceSlot == 0) {
      m_nextVoiceSlot = 1;
    }
    handle.generation = m_nextVoiceGeneration++;
    if (m_nextVoiceGeneration == 0) {
      m_nextVoiceGeneration = 1;
    }
    return handle;
  }

  AudioVoiceHandle post_event_locked(const AudioPostEventDesc& desc) {
    ++m_diag.play_requests;
    const auto eventName = NormalizeEventName(desc.event_name);
    const auto eventIt = m_events.find(eventName);
    if (eventIt == m_events.end() || eventIt->second.clips.empty()) {
      ++m_diag.failed_play_requests;
      m_diag.last_error = "audio event not found: " + eventName;
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Warning,
          "audio",
          m_diag.last_error,
          {{"entity", std::to_string(desc.entity)}});
      return {};
    }

    const auto& event = eventIt->second;
    const auto eventEntity = desc.entity;
    const auto sequence = m_config.deterministic
                              ? (event.name + ":" + std::to_string(eventEntity) + ":" + std::to_string(m_diag.play_requests))
                              : (event.name + ":" + std::to_string(m_diag.play_requests));
    const auto variantIndex = static_cast<std::size_t>(Fnv1a64(sequence) % event.clips.size());
    const auto clipIt = m_clips.find(event.clips[variantIndex]);
    if (clipIt == m_clips.end()) {
      ++m_diag.failed_play_requests;
      m_diag.last_error = "audio clip missing for event: " + event.name;
      return {};
    }

    const AudioVoiceHandle handle = allocate_handle_locked();
    const auto busName = NormalizeEventName(desc.bus.empty() ? event.bus : desc.bus);
    auto& bus = bus_state_locked(busName);
    ++bus.play_requests;
    append_history_locked(event.name + " -> " + clipIt->second.uri);
    if (m_voices.size() >= m_config.max_voices) {
      m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [&](const auto& voice) {
        return voice == nullptr || voice_finished_locked(*voice);
      }), m_voices.end());
      if (m_voices.size() >= m_config.max_voices) {
        auto victim = std::min_element(m_voices.begin(), m_voices.end(), [](const auto& lhs, const auto& rhs) {
          if (!lhs) return true;
          if (!rhs) return false;
          if (lhs->loop != rhs->loop) return !lhs->loop;
          return lhs->priority < rhs->priority;
        });
        if (victim == m_voices.end()) {
          ++m_diag.dropped_commands;
          return {};
        }
        if (*victim) {
          (*victim)->stolen = true;
          ++m_diag.stolen_voices;
        }
        m_voices.erase(victim);
      }
    }

    auto voice = std::make_unique<RuntimeVoice>();
    voice->handle = handle;
    voice->clip = clipIt->first;
    voice->event_name = event.name;
    voice->loop = desc.loop || event.loop || clipIt->second.loop_default;
    voice->spatial = desc.spatial || event.spatial;
    voice->entity = desc.entity;
    voice->bus = busName;
    voice->clip_uri = clipIt->second.uri;
    voice->playing = true;
    voice->priority = std::clamp(desc.priority > 0.0f ? desc.priority : event.priority, 0.0f, 1.0f);

    const auto position = desc.has_position ? desc.position : vkpt::scene::Vec3{};
    const auto spatial = CalculateSpatialMix(m_listener, position, event.min_distance, event.max_distance);
    const float busGain = bus.muted ? 0.0f : bus.volume * BusDefaultGain(busName);
    const float volume = std::max(0.0f, desc.volume * event.volume * busGain * (voice->spatial ? spatial.gain : 1.0f));
    const float pitch = std::clamp(desc.pitch * event.pitch, 0.25f, 4.0f);
    voice->volume = volume;
    voice->pitch = pitch;
    voice->position = position;
    voice->pan = voice->spatial ? spatial.pan : 0.0f;
    voice->gain = voice->spatial ? spatial.gain : 1.0f;

    if (m_config.muted || m_diag.backend_name == "noop") {
      m_voices.push_back(std::move(voice));
      m_diag.active_voices = active_voice_count_locked();
      return handle;
    }

    if (!start_voice_locked(*voice, clipIt->second, volume, pitch, event.min_distance, event.max_distance, position)) {
      ++m_diag.failed_play_requests;
      return {};
    }

    m_voices.push_back(std::move(voice));
    m_diag.active_voices = active_voice_count_locked();
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Debug,
        "audio",
        "audio event posted",
        {{"event", event.name},
         {"entity", std::to_string(desc.entity)},
         {"clip", clipIt->second.uri},
         {"bus", busName},
         {"spatial", voice->spatial ? "true" : "false"},
         {"loop", event.loop ? "true" : "false"}});
    return handle;
  }

  std::size_t active_voice_count_locked() const {
    return static_cast<std::size_t>(std::count_if(m_voices.begin(), m_voices.end(), [&](const auto& voice) {
      return voice != nullptr && !voice_finished_locked(*voice);
    }));
  }

  void set_listener_locked(const AudioListenerState& listener) {
#if defined(PT_ENABLE_MINIAUDIO)
    if (m_engineInitialized) {
      ma_engine_listener_set_position(&m_engine, 0, listener.position.x, listener.position.y, listener.position.z);
      ma_engine_listener_set_direction(&m_engine, 0, listener.forward.x, listener.forward.y, listener.forward.z);
      ma_engine_listener_set_world_up(&m_engine, 0, listener.up.x, listener.up.y, listener.up.z);
    }
#else
    (void)listener;
#endif
  }

  bool start_voice_locked(RuntimeVoice& voice,
                          const ClipDef& clip,
                          float volume,
                          float pitch,
                          float minDistance,
                          float maxDistance,
                          const vkpt::scene::Vec3& position) {
#if defined(PT_ENABLE_MINIAUDIO)
    if (!m_engineInitialized) {
      m_diag.last_error = "miniaudio engine is not initialized";
      return false;
    }

    ma_uint32 flags = MA_SOUND_FLAG_DECODE;
    if (!voice.spatial) {
      flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
    }

    ma_result result = MA_SUCCESS;
    if (clip.generated) {
      if (clip.samples.empty() || clip.channels == 0) {
        m_diag.last_error = "generated audio clip has no samples: " + clip.uri;
        return false;
      }
      ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
          ma_format_f32,
          clip.channels,
          static_cast<ma_uint64>(clip.frame_count()),
          const_cast<float*>(clip.samples.data()),
          nullptr);
      result = ma_audio_buffer_init(&bufferConfig, &voice.buffer);
      if (result != MA_SUCCESS) {
        m_diag.last_error = "ma_audio_buffer_init failed: " + std::to_string(static_cast<int>(result));
        return false;
      }
      voice.buffer_initialized = true;
      result = ma_sound_init_from_data_source(&m_engine, &voice.buffer, flags, nullptr, &voice.sound);
    } else {
      result = ma_sound_init_from_file(&m_engine, clip.file_path.c_str(), flags, nullptr, nullptr, &voice.sound);
    }

    if (result != MA_SUCCESS) {
      m_diag.last_error = "ma_sound init failed for " + clip.uri + ": " +
                          std::to_string(static_cast<int>(result));
      vkpt::log::Logger::instance().log(vkpt::log::Severity::Warning, "audio", m_diag.last_error);
      return false;
    }
    voice.sound_initialized = true;
    ma_sound_set_volume(&voice.sound, volume);
    ma_sound_set_pitch(&voice.sound, pitch);
    ma_sound_set_looping(&voice.sound, voice.loop ? MA_TRUE : MA_FALSE);
    if (voice.spatial) {
      ma_sound_set_spatialization_enabled(&voice.sound, MA_TRUE);
      ma_sound_set_position(&voice.sound, position.x, position.y, position.z);
      ma_sound_set_min_distance(&voice.sound, std::max(0.001f, minDistance));
      ma_sound_set_max_distance(&voice.sound, std::max(minDistance + 0.001f, maxDistance));
      ma_sound_set_attenuation_model(&voice.sound, ma_attenuation_model_linear);
    } else {
      ma_sound_set_spatialization_enabled(&voice.sound, MA_FALSE);
    }

    result = ma_sound_start(&voice.sound);
    if (result != MA_SUCCESS) {
      m_diag.last_error = "ma_sound_start failed: " + std::to_string(static_cast<int>(result));
      return false;
    }
    return true;
#else
    (void)voice;
    (void)clip;
    (void)volume;
    (void)pitch;
    (void)minDistance;
    (void)maxDistance;
    (void)position;
    m_diag.last_error = "PT_ENABLE_MINIAUDIO is disabled";
    return false;
#endif
  }

  void initialize_buses_locked() {
    m_buses.clear();
    for (const std::string_view bus : {"master", "sfx", "music", "ui", "voice", "ambience", "debug"}) {
      auto& state = m_buses[std::string(bus)];
      state.name = std::string(bus);
      state.volume = 1.0f;
    }
  }

  BusState& bus_state_locked(std::string_view bus) {
    const auto key = NormalizeEventName(bus.empty() ? "sfx" : bus);
    auto& state = m_buses[key];
    if (state.name.empty()) {
      state.name = key;
      state.volume = 1.0f;
    }
    return state;
  }

  float default_priority_for_bus(std::string_view bus) const {
    const auto key = NormalizeEventName(bus);
    if (key == "ui") return 0.95f;
    if (key == "music") return 0.90f;
    if (key == "voice") return 0.80f;
    if (key == "ambience") return 0.25f;
    return 0.60f;
  }

  void append_history_locked(std::string text) {
    constexpr std::size_t kMaxHistory = 128u;
    m_eventHistory.push_back(std::move(text));
    while (m_eventHistory.size() > kMaxHistory) {
      m_eventHistory.pop_front();
    }
  }

  void stop_voice_locked(RuntimeVoice& voice) {
    voice.playing = false;
#if defined(PT_ENABLE_MINIAUDIO)
    if (voice.sound_initialized) {
      ma_sound_stop(&voice.sound);
    }
#endif
  }

  bool voice_finished_locked(const RuntimeVoice& voice) const {
    if (!voice.playing) {
      return true;
    }
#if defined(PT_ENABLE_MINIAUDIO)
    if (voice.sound_initialized && !voice.loop && ma_sound_at_end(&voice.sound)) {
      return true;
    }
#endif
    return false;
  }

  AudioSystemConfig m_config;
  mutable std::mutex m_mutex;
  bool m_initialized = false;
  AudioListenerState m_listener;
  std::unordered_map<AudioClipId, ClipDef> m_clips;
  std::unordered_map<std::string, AudioClipId> m_clipByUri;
  std::unordered_map<std::string, EventDef> m_events;
  std::unordered_map<std::string, BusState> m_buses;
  std::deque<std::string> m_eventHistory;
  std::vector<std::unique_ptr<RuntimeVoice>> m_voices;
  std::uint32_t m_nextVoiceSlot = 1u;
  std::uint32_t m_nextVoiceGeneration = 1u;
  AudioDiagnostics m_diag;
#if defined(PT_ENABLE_MINIAUDIO)
  ma_engine m_engine{};
  bool m_engineInitialized = false;
#endif
};

}  // namespace

std::unique_ptr<IAudioSystem> CreateAudioSystem(AudioSystemConfig config) {
  return std::make_unique<EngineAudioSystem>(std::move(config));
}

IAudioSystem* GlobalAudioSystem() {
  return g_audioSystem.load(std::memory_order_acquire);
}

void SetGlobalAudioSystem(IAudioSystem* system) {
  g_audioSystem.store(system, std::memory_order_release);
}

}  // namespace vkpt::audio
