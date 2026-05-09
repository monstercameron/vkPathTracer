#include "audio/AudioSystem.h"

#include "audio/AudioChannels.h"
#include "audio/IAudioDevice.h"
#include "core/Logging.h"
#include "core/health/Health.h"
#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "scene/SnapshotRing.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <deque>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(PT_ENABLE_MINIAUDIO)
#include <miniaudio.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace vkpt::audio {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr std::uint32_t kPcmPageSampleCapacity = 512u;
constexpr std::uint32_t kMaxStackStreamChannels = 8u;
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

std::uint64_t PackAudioHandle(AudioVoiceHandle handle) {
  return (static_cast<std::uint64_t>(handle.generation) << 32u) |
         static_cast<std::uint64_t>(handle.slot);
}

vkpt::scene::Vec3 ToSceneVec3(const vkpt::pathtracer::Vec3& value) {
  return {value.x, value.y, value.z};
}

using Clock = std::chrono::steady_clock;

std::uint64_t SteadyNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch()).count());
}

const char* AudioCmdKindName(AudioCmdKind kind) {
  switch (kind) {
    case AudioCmdKind::PlayOneShot: return "play_one_shot";
    case AudioCmdKind::StartLoop: return "start_loop";
    case AudioCmdKind::StopLoop: return "stop_loop";
    case AudioCmdKind::SetVolume: return "set_volume";
    case AudioCmdKind::SetListener: return "set_listener";
    case AudioCmdKind::PlayResolved: return "play_resolved";
    case AudioCmdKind::StopVoice: return "stop_voice";
    case AudioCmdKind::StopAll: return "stop_all";
    case AudioCmdKind::SetBusVolume: return "set_bus_volume";
    case AudioCmdKind::SetBusMuted: return "set_bus_muted";
  }
  return "unknown";
}

#if defined(_WIN32) && defined(PT_ENABLE_MINIAUDIO)
using AvSetMmThreadCharacteristicsFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
using AvRevertMmThreadCharacteristicsFn = BOOL(WINAPI*)(HANDLE);

struct WindowsAudioRtApi {
  HMODULE module = nullptr;
  AvSetMmThreadCharacteristicsFn set = nullptr;
  AvRevertMmThreadCharacteristicsFn revert = nullptr;
};

WindowsAudioRtApi LoadWindowsAudioRtApi() {
  WindowsAudioRtApi api;
  api.module = LoadLibraryW(L"Avrt.dll");
  if (api.module != nullptr) {
    api.set = reinterpret_cast<AvSetMmThreadCharacteristicsFn>(
        GetProcAddress(api.module, "AvSetMmThreadCharacteristicsW"));
    api.revert = reinterpret_cast<AvRevertMmThreadCharacteristicsFn>(
        GetProcAddress(api.module, "AvRevertMmThreadCharacteristics"));
  }
  return api;
}
#endif

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

float Dot(const vkpt::scene::Vec3& a, const vkpt::scene::Vec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

vkpt::scene::Vec3 NormalizeOr(const vkpt::scene::Vec3& value,
                              const vkpt::scene::Vec3& fallback) {
  const float lenSq = value.x * value.x + value.y * value.y + value.z * value.z;
  if (lenSq <= 1.0e-8f) {
    return fallback;
  }
  const float invLen = 1.0f / std::sqrt(lenSq);
  return {value.x * invLen, value.y * invLen, value.z * invLen};
}

AudioListenerState ListenerFromSnapshotCamera(
    const vkpt::pathtracer::RTCameraState& camera) {
  const auto position = ToSceneVec3(camera.position);
  const auto target = ToSceneVec3(camera.target);
  AudioListenerState listener;
  listener.position = position;
  listener.forward = NormalizeOr({target.x - position.x,
                                  target.y - position.y,
                                  target.z - position.z},
                                 {0.0f, 0.0f, -1.0f});
  listener.up = NormalizeOr(ToSceneVec3(camera.up), {0.0f, 1.0f, 0.0f});
  return listener;
}

float DopplerPitchFromSnapshotMotion(const vkpt::scene::RenderSceneSnapshot& snapshot,
                                     vkpt::core::StableEntityId entity,
                                     const AudioListenerState& listener,
                                     const vkpt::scene::Vec3& sourcePosition) {
  if (entity == 0u || snapshot.instance_motion.empty()) {
    return 1.0f;
  }
  constexpr float kSpeedOfSoundMetersPerSecond = 343.0f;
  const float dt = std::clamp(snapshot.camera.shutter_seconds, 1.0f / 240.0f, 1.0f / 15.0f);
  for (const auto& motion : snapshot.instance_motion.view()) {
    if (!motion.previous_valid || motion.current.entity_id != entity) {
      continue;
    }
    const auto previous = ToSceneVec3(motion.previous.translation);
    const auto current = ToSceneVec3(motion.current.translation);
    const vkpt::scene::Vec3 velocity{
        (current.x - previous.x) / dt,
        (current.y - previous.y) / dt,
        (current.z - previous.z) / dt};
    const auto toListener = NormalizeOr({listener.position.x - sourcePosition.x,
                                         listener.position.y - sourcePosition.y,
                                         listener.position.z - sourcePosition.z},
                                        {0.0f, 0.0f, -1.0f});
    const float radialVelocity = std::clamp(Dot(velocity, toListener), -120.0f, 120.0f);
    return std::clamp(kSpeedOfSoundMetersPerSecond /
                          (kSpeedOfSoundMetersPerSecond - radialVelocity),
                      0.5f,
                      2.0f);
  }
  return 1.0f;
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
  std::shared_ptr<const std::vector<float>> rt_samples;

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
  } else if (lower.find("ricochet") != std::string::npos ||
             lower.find("impact") != std::string::npos ||
             lower.find("hit") != std::string::npos) {
    appendMixed(380.0f, 0.045f, 0.62f, 0.085f, 0.55f);
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
  float min_distance = 1.0f;
  float max_distance = 24.0f;
  float priority = 0.5f;
  double cursor_frame = 0.0;

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
      ma_sound_stop(&sound);
      ma_sound_uninit(&sound);
    }
    if (buffer_initialized) {
      ma_audio_buffer_uninit(&buffer);
    }
  }
#endif
};

struct RtClip {
  AudioClipId id = 0;
  std::shared_ptr<const std::vector<float>> samples;
  std::uint32_t channels = 0;
  std::size_t frame_count = 0;
};

struct RtVoice {
  AudioVoiceHandle handle;
  AudioClipId clip = 0;
  vkpt::core::StableEntityId entity = 0;
  vkpt::scene::Vec3 position{0.0f, 0.0f, 0.0f};
  bool playing = false;
  bool loop = false;
  bool spatial = false;
  float volume = 1.0f;
  float pitch = 1.0f;
  float pan = 0.0f;
  float gain = 1.0f;
  float min_distance = 1.0f;
  float max_distance = 24.0f;
  float bus_gain = 1.0f;
  double cursor_frame = 0.0;
  std::uint64_t stream_id = 0;
  std::shared_ptr<VoicePcmRing> pcm_ring;
  PcmPage current_page{};
  std::uint32_t current_page_frame = 0;
};

struct RtMixState {
  std::uint64_t generation = 0;
  bool muted = false;
  AudioListenerState listener;
  std::vector<RtClip> clips;
  std::vector<RtVoice> voices;
};

struct AudioDecodeRequest {
  AudioClipId clip = 0;
  std::string uri;
  std::string file_path;
  std::uint32_t sample_rate = 44100;
  std::uint32_t channels = 1;
};

struct VoiceStreamFillRequest {
  std::uint64_t stream_id = 0;
  std::shared_ptr<VoicePcmRing> ring;
  std::shared_ptr<const std::vector<float>> samples;
  std::uint32_t channels = 1;
  bool loop = false;
  double start_frame = 0.0;
};

class NoopAudioDevice final : public IAudioDevice {
 public:
  ~NoopAudioDevice() override {
    close();
  }

  vkpt::core::Status open(const AudioDeviceConfig& config, AudioDeviceCallback callback) override {
    m_config = config;
    m_callback = callback;
    m_open = true;
    return vkpt::core::Status::ok();
  }

  void close() override {
    stop();
    m_open = false;
  }

  vkpt::core::Status start() override {
    if (!m_open) {
      return vkpt::core::Status::error(vkpt::core::StatusCode::NotReady,
                                       "noop audio device not open");
    }
    bool expected = false;
    if (m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      m_thread = std::thread([this] { callback_loop(); });
    }
    m_started = true;
    return vkpt::core::Status::ok();
  }

  void stop() override {
    if (m_running.exchange(false, std::memory_order_acq_rel) && m_thread.joinable()) {
      m_thread.join();
    }
    m_started = false;
  }

  std::string device_name() const override {
    return "no output device";
  }

 private:
  void callback_loop() {
    const std::uint32_t frames = std::max<std::uint32_t>(1u, m_config.buffer_frames);
    const std::uint32_t channels = std::max<std::uint32_t>(1u, m_config.channels);
    const std::uint32_t sampleRate = std::max<std::uint32_t>(1u, m_config.sample_rate);
    const auto period = std::chrono::nanoseconds(
        (static_cast<std::uint64_t>(frames) * 1000000000ull) /
        static_cast<std::uint64_t>(sampleRate));
    std::vector<float> scratch(static_cast<std::size_t>(frames) *
                               static_cast<std::size_t>(channels));
    auto next = Clock::now();
    while (m_running.load(std::memory_order_acquire)) {
      std::fill(scratch.begin(), scratch.end(), 0.0f);
      if (m_callback.render != nullptr) {
        m_callback.render(m_callback.user, scratch.data(), frames, channels);
      }
      next += period;
      const auto now = Clock::now();
      if (next < now) {
        next = now + period;
      }
      std::this_thread::sleep_until(next);
    }
  }

  AudioDeviceConfig m_config;
  AudioDeviceCallback m_callback;
  std::atomic_bool m_running{false};
  std::thread m_thread;
  bool m_open = false;
  bool m_started = false;
};

#if defined(PT_ENABLE_MINIAUDIO)
class MiniaudioDevice final : public IAudioDevice {
 public:
  ~MiniaudioDevice() override {
    close();
  }

  vkpt::core::Status open(const AudioDeviceConfig& config, AudioDeviceCallback callback) override {
    close();
    m_config = config;
    m_callback = callback;
#if defined(_WIN32)
    m_rtApi = LoadWindowsAudioRtApi();
#endif

    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = std::max<std::uint32_t>(1u, config.channels);
    deviceConfig.sampleRate = std::max<std::uint32_t>(8000u, config.sample_rate);
    deviceConfig.periodSizeInFrames = std::max<std::uint32_t>(64u, config.buffer_frames);
    deviceConfig.dataCallback = &MiniaudioDevice::data_callback;
    deviceConfig.pUserData = this;

    const ma_result result = ma_device_init(nullptr, &deviceConfig, &m_device);
    if (result != MA_SUCCESS) {
      m_lastError = "ma_device_init failed: " + std::to_string(static_cast<int>(result));
      return vkpt::core::Status::error(vkpt::core::StatusCode::InternalError, m_lastError);
    }
    m_open = true;
    return vkpt::core::Status::ok();
  }

  void close() override {
    if (!m_open) {
      return;
    }
    ma_device_stop(&m_device);
    ma_device_uninit(&m_device);
    m_open = false;
    m_started = false;
  }

  vkpt::core::Status start() override {
    if (!m_open) {
      return vkpt::core::Status::error(vkpt::core::StatusCode::NotReady,
                                       "miniaudio device not open");
    }
    const ma_result result = ma_device_start(&m_device);
    if (result != MA_SUCCESS) {
      m_lastError = "ma_device_start failed: " + std::to_string(static_cast<int>(result));
      return vkpt::core::Status::error(vkpt::core::StatusCode::InternalError, m_lastError);
    }
    m_started = true;
    return vkpt::core::Status::ok();
  }

  void stop() override {
    if (m_open && m_started) {
      ma_device_stop(&m_device);
      m_started = false;
    }
  }

  std::string device_name() const override {
    return m_open ? "miniaudio playback device" : "miniaudio device closed";
  }

  const std::string& last_error() const {
    return m_lastError;
  }

 private:
  static void data_callback(ma_device* device,
                            void* output,
                            const void* input,
                            ma_uint32 frameCount) {
    (void)input;
    if (output == nullptr || device == nullptr) {
      return;
    }
    auto* self = static_cast<MiniaudioDevice*>(device->pUserData);
    const auto outputChannels = std::max<ma_uint32>(1u, device->playback.channels);
    auto* out = static_cast<float*>(output);
    std::memset(out, 0, static_cast<std::size_t>(frameCount) *
                         static_cast<std::size_t>(outputChannels) *
                         sizeof(float));
    if (self == nullptr || self->m_callback.render == nullptr) {
      return;
    }
    self->promote_callback_thread();
    self->m_callback.render(self->m_callback.user,
                            out,
                            static_cast<std::uint32_t>(frameCount),
                            static_cast<std::uint32_t>(outputChannels));
  }

  void promote_callback_thread() {
#if defined(_WIN32)
    if (m_rtApi.set == nullptr) {
      return;
    }
    thread_local HANDLE rtHandle = nullptr;
    if (rtHandle == nullptr) {
      DWORD taskIndex = 0;
      rtHandle = m_rtApi.set(L"Pro Audio", &taskIndex);
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
#endif
  }

  AudioDeviceConfig m_config;
  AudioDeviceCallback m_callback;
  ma_device m_device{};
  bool m_open = false;
  bool m_started = false;
  std::string m_lastError;
#if defined(_WIN32)
  WindowsAudioRtApi m_rtApi;
#endif
};
#endif

struct AudioHealthState {
  std::atomic<bool> initialized{false};
  std::atomic<std::uint64_t> last_underrun_ns{0};
  std::atomic<std::uint64_t> last_callback_start_ns{0};
  std::atomic<std::uint64_t> errors_total{0};
  mutable std::mutex mutex;
  std::string last_error;
};

class EngineAudioSystem final : public IAudioSystem {
 public:
  explicit EngineAudioSystem(AudioSystemConfig config) : m_config(std::move(config)) {}
  ~EngineAudioSystem() override {
    shutdown();
  }

  vkpt::core::Status initialize() override {
    std::lock_guard lock(m_mutex);
    if (m_initialized) {
      return vkpt::core::Status::ok("audio system already initialized");
    }
    if (!vkpt::core::contracts::assert_state(
            "IAudioSystem::initialize",
            m_lifecycle.load(std::memory_order_relaxed),
            {vkpt::core::contracts::ComponentLifecycle::Uninitialized})) {
      return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument,
                                       "initialize called from invalid audio lifecycle state");
    }
    m_lifecycle.store(vkpt::core::contracts::ComponentLifecycle::Initializing,
                      std::memory_order_relaxed);

    m_config.sample_rate = std::max<std::uint32_t>(8000u, m_config.sample_rate);
    m_config.channels = std::clamp<std::uint32_t>(m_config.channels, 1u, 2u);
    const auto maxFramesFor10Ms = std::max<std::uint32_t>(64u, m_config.sample_rate / 100u);
    if (m_config.buffer_frames == 0u) {
      m_config.buffer_frames = std::min<std::uint32_t>(256u, maxFramesFor10Ms);
    } else {
      m_config.buffer_frames =
          std::clamp<std::uint32_t>(m_config.buffer_frames, 64u, maxFramesFor10Ms);
    }
    bind_audio_metrics_locked();
    m_diag = {};
    m_diag.sample_rate = m_config.sample_rate;
    m_diag.channels = m_config.channels;
    m_diag.muted = m_config.muted;
    m_diag.queued_commands = 0;
    m_diag.backend_name = select_backend_name();
    m_diag.device_name = "no output device";

    AudioDeviceConfig deviceConfig;
    deviceConfig.sample_rate = m_config.sample_rate;
    deviceConfig.channels = m_config.channels;
    deviceConfig.buffer_frames = m_config.buffer_frames;
    AudioDeviceCallback deviceCallback;
    deviceCallback.render = &EngineAudioSystem::audio_device_render_callback;
    deviceCallback.user = this;
    std::optional<std::string> backendFallbackWarning;

#if defined(PT_ENABLE_MINIAUDIO)
    if (m_diag.backend_name == "miniaudio") {
      auto device = std::make_unique<MiniaudioDevice>();
      if (device->open(deviceConfig, deviceCallback).is_ok() && device->start().is_ok()) {
        m_hardwarePlaybackEnabled = true;
        m_diag.device_name = device->device_name();
        m_audioDevice = std::move(device);
      } else {
        m_diag.last_error = device->last_error().empty()
                                ? "miniaudio playback device open failed"
                                : device->last_error();
        vkpt::log::Logger::instance().log(vkpt::log::Severity::Warning, "audio", m_diag.last_error);
        backendFallbackWarning = m_diag.last_error;
        m_diag.backend_name = "noop";
        m_diag.device_name = "no output device";
      }
    }
#endif
    if (!m_audioDevice) {
      auto device = std::make_unique<NoopAudioDevice>();
      (void)device->open(deviceConfig, deviceCallback);
      (void)device->start();
      // open()/start() return Status; ignored here because NoopAudioDevice
      // cannot fail in practice and we already committed to the noop path.
      m_audioDevice = std::move(device);
      m_hardwarePlaybackEnabled = false;
    }
    m_decodeThread = std::jthread([this](std::stop_token stop) {
      decode_thread_loop(stop);
    });

    m_initialized = true;
    m_diag.initialized = true;
    m_diag.state = m_diag.backend_name == "noop" ? "initialized_noop" : "initialized";
    set_listener_locked(m_listener);
    publish_rt_state_locked();
    register_health_probe_locked();
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
    VKP_LOG(Info,
            "audio",
            "config",
            "backend",
            m_diag.backend_name,
            "device",
            m_diag.device_name,
            "sample_rate",
            static_cast<std::uint64_t>(m_config.sample_rate),
            "buf_frames",
            static_cast<std::uint64_t>(m_config.buffer_frames));
    VKP_LIFECYCLE_STARTED("audio",
                          "backend",
                          m_diag.backend_name,
                          "sample_rate",
                          static_cast<std::uint64_t>(m_config.sample_rate),
                          "buffer_frames",
                          static_cast<std::uint64_t>(m_config.buffer_frames));
    m_lifecycle.store(backendFallbackWarning.has_value()
                          ? vkpt::core::contracts::ComponentLifecycle::Degraded
                          : vkpt::core::contracts::ComponentLifecycle::Ready,
                      std::memory_order_relaxed);
    auto status = vkpt::core::Status::ok("audio system initialized");
    if (backendFallbackWarning.has_value()) {
      status.warnings.push_back(*backendFallbackWarning);
    }
    return status;
  }

  void shutdown() override {
    std::unique_ptr<IAudioDevice> device;
    {
      std::lock_guard lock(m_mutex);
      if (!m_initialized) {
        return;
      }
      m_lifecycle.store(vkpt::core::contracts::ComponentLifecycle::ShuttingDown,
                        std::memory_order_relaxed);
      clear_runtime_voices_locked();
      append_history_locked("shutdown");
      m_hardwarePlaybackEnabled = false;
      device = std::move(m_audioDevice);
      m_initialized = false;
      m_diag.initialized = false;
      m_diag.active_voices = 0u;
      m_diag.state = "shutdown";
      m_lifecycle.store(vkpt::core::contracts::ComponentLifecycle::Uninitialized,
                        std::memory_order_relaxed);
      if (m_decodeThread.joinable()) {
        m_decodeThread.request_stop();
        m_decodeCv.notify_all();
      }
      publish_rt_state_locked();
    }
    vkpt::core::health::HealthRegistry::instance().unregister_probe(
        std::string(kAudioSubsystemName));
    m_healthProbeRegistered.store(false, std::memory_order_release);
    if (m_decodeThread.joinable()) {
      m_decodeThread.join();
    }
    if (device) {
      device->stop();
      device->close();
    }
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Info, "audio", "audio system shutdown");
    VKP_LIFECYCLE_STOPPED("audio");
  }

  vkpt::core::Status load_scene_audio(const vkpt::scene::SceneDocument& document,
                                      std::string_view scene_path) override {
    std::lock_guard lock(m_mutex);
    if (!m_initialized) {
      m_diag.last_error = "load_scene_audio called before initialize";
      return vkpt::core::Status::error(vkpt::core::StatusCode::NotReady, m_diag.last_error);
    }
    if (!vkpt::core::contracts::assert_state(
            "IAudioSystem::load_scene_audio",
            m_lifecycle.load(std::memory_order_relaxed),
            {vkpt::core::contracts::ComponentLifecycle::Ready,
             vkpt::core::contracts::ComponentLifecycle::Degraded})) {
      m_diag.last_error = "load_scene_audio called from invalid audio lifecycle state";
      return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument, m_diag.last_error);
    }
    clear_runtime_voices_locked();
    m_clips.clear();
    m_clipByUri.clear();
    m_events.clear();
    m_eventHistory.clear();
    m_stolenEventHandles.clear();
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
      vkpt::log::Logger::instance().log(vkpt::log::Severity::Debug,
                                         "audio",
                                         "loading scene audio asset",
                                         {{"uri", asset.uri}, {"event", event.name}});
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
        vkpt::log::Logger::instance().log(
            vkpt::log::Severity::Debug,
            "audio",
            "starting autoplay audio emitter",
            {{"event", entity.audio_emitter.event}, {"entity", std::to_string(entity.id)}});
        AudioTrackedEventDesc desc;
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
    update_voice_metrics_locked();
    publish_rt_state_locked();
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        "audio",
        "scene audio loaded",
        {{"scene", std::string(scene_path)},
         {"declared_assets", std::to_string(declaredAudioAssets)},
         {"clips", std::to_string(m_clips.size())},
         {"events", std::to_string(m_events.size())},
         {"autoplay_voices", std::to_string(m_diag.active_voices)}});
    return vkpt::core::Status::ok("scene audio loaded");
  }

  vkpt::core::Status post_one_shot_event(const AudioOneShotEventDesc& desc) override {
    std::lock_guard lock(m_mutex);
    if (!m_initialized) {
      m_diag.last_error = "post_one_shot_event called before initialize";
      return vkpt::core::Status::error(vkpt::core::StatusCode::NotReady, m_diag.last_error);
    }
    AudioTrackedEventDesc tracked;
    static_cast<AudioOneShotEventDesc&>(tracked) = desc;
    tracked.loop = false;
    const auto handle = post_event_locked(tracked);
    if (!handle) {
      return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument,
                                       m_diag.last_error.empty()
                                           ? "audio one-shot event was not posted"
                                           : m_diag.last_error);
    }
    return vkpt::core::Status::ok("audio one-shot event posted");
  }

  AudioEventHandle post_tracked_event(const AudioTrackedEventDesc& desc) override {
    std::lock_guard lock(m_mutex);
    if (!m_initialized) {
      m_diag.last_error = "post_tracked_event called before initialize";
      ++m_diag.failed_play_requests;
      return {};
    }
    return post_event_locked(desc);
  }

  AudioStatus status() const override {
    const auto diag = diagnostics();
    AudioStatus out;
    out.lifecycle = m_lifecycle.load(std::memory_order_relaxed);
    out.backend_name = diag.backend_name;
    out.device_name = diag.device_name;
    out.state = diag.state;
    out.last_error = diag.last_error;
    out.last_tick_ns = m_lastCallbackStartNs.load(std::memory_order_relaxed);
    out.ticks_total = diag.mixed_buffers;
    out.errors_total = diag.failed_play_requests + diag.dropped_commands +
                       diag.underruns + diag.stream_ring_underruns;
    out.initialized = diag.initialized;
    out.muted = diag.muted;
    out.sample_rate = diag.sample_rate;
    out.channels = diag.channels;
    out.buffer_frames = m_config.buffer_frames;
    out.loaded_clips = diag.loaded_clips;
    out.events = diag.events;
    out.active_voices = diag.active_voices;
    out.voices_max = m_config.max_voices;
    out.queued_commands = diag.queued_commands;
    out.play_requests = diag.play_requests;
    out.failed_play_requests = diag.failed_play_requests;
    out.dropped_commands = diag.dropped_commands;
    out.underruns = diag.underruns;
    out.last_underrun_ns = m_lastUnderrunNs.load(std::memory_order_relaxed);
    out.last_callback_start_ns = m_lastCallbackStartNs.load(std::memory_order_relaxed);
    out.snapshot_generation = m_rtSnapshotGeneration.load(std::memory_order_relaxed);
    out.snapshot_age_ms = m_rtSnapshotAgeMs.load(std::memory_order_relaxed);
    out.current_flow_id = m_currentFlowId.load(std::memory_order_relaxed);
    publish_health_state(out);
    return out;
  }

  void cancel(AudioEventHandle handle) override {
    if (!handle) {
      return;
    }
    std::lock_guard lock(m_mutex);
    ++m_diag.stop_requests;
    AudioCmd cmd;
    cmd.kind = AudioCmdKind::StopVoice;
    cmd.cmd_id = m_nextAudioCmdId++;
    cmd.handle = handle;
    enqueue_audio_cmd_locked(std::move(cmd));
    drain_audio_commands_locked();
  }

  void set_volume(AudioEventHandle handle, float volume) override {
    if (!handle) {
      return;
    }
    std::lock_guard lock(m_mutex);
    AudioCmd cmd;
    cmd.kind = AudioCmdKind::SetVolume;
    cmd.cmd_id = m_nextAudioCmdId++;
    cmd.handle = handle;
    cmd.volume = std::clamp(volume, 0.0f, 2.0f);
    enqueue_audio_cmd_locked(std::move(cmd));
    drain_audio_commands_locked();
  }

  AudioEventState query_state(AudioEventHandle handle) const override {
    AudioEventState out;
    out.handle = handle;
    if (!handle) {
      return out;
    }
    std::lock_guard lock(m_mutex);
    for (const auto& voice : m_voices) {
      if (!voice || voice->handle.slot != handle.slot ||
          voice->handle.generation != handle.generation) {
        continue;
      }
      out.event_name = voice->event_name;
      out.valid = true;
      out.playing = voice->playing && !voice_finished_locked(*voice);
      out.loop = voice->loop;
      out.stolen = voice->stolen;
      out.volume = voice->volume;
      return out;
    }
    out.stolen = m_stolenEventHandles.find(PackAudioHandle(handle)) != m_stolenEventHandles.end();
    return out;
  }

  void stop(AudioEventHandle handle) override {
    cancel(handle);
  }

  void stop_all() override {
    std::lock_guard lock(m_mutex);
    AudioCmd cmd;
    cmd.kind = AudioCmdKind::StopAll;
    cmd.cmd_id = m_nextAudioCmdId++;
    enqueue_audio_cmd_locked(std::move(cmd));
    append_history_locked("stop_all");
    drain_audio_commands_locked();
  }

  void set_listener(const AudioListenerState& listener) override {
    std::lock_guard lock(m_mutex);
    ++m_diag.listener_updates;
    AudioCmd cmd;
    cmd.kind = AudioCmdKind::SetListener;
    cmd.cmd_id = m_nextAudioCmdId++;
    cmd.listener = listener;
    enqueue_audio_cmd_locked(std::move(cmd));
    drain_audio_commands_locked();
  }

  void set_bus_volume(std::string_view bus, float volume) override {
    std::lock_guard lock(m_mutex);
    AudioCmd cmd;
    cmd.kind = AudioCmdKind::SetBusVolume;
    cmd.cmd_id = m_nextAudioCmdId++;
    cmd.bus = NormalizeEventName(bus.empty() ? "sfx" : bus);
    cmd.volume = volume;
    enqueue_audio_cmd_locked(std::move(cmd));
    drain_audio_commands_locked();
  }

  void set_bus_muted(std::string_view bus, bool muted) override {
    std::lock_guard lock(m_mutex);
    AudioCmd cmd;
    cmd.kind = AudioCmdKind::SetBusMuted;
    cmd.cmd_id = m_nextAudioCmdId++;
    cmd.bus = NormalizeEventName(bus.empty() ? "sfx" : bus);
    cmd.bus_muted = muted;
    enqueue_audio_cmd_locked(std::move(cmd));
    drain_audio_commands_locked();
  }

  void set_snapshot_ring(vkpt::scene::SnapshotRing* snapshots) override {
    const auto current = m_snapshotRing.load(std::memory_order_acquire);
    if (current == snapshots) {
      return;
    }
    const std::uint32_t readerId = snapshots == nullptr
        ? vkpt::scene::SnapshotRing::kInvalidReader
        : snapshots->register_reader("audio");
    std::lock_guard lock(m_mutex);
    m_snapshotRing.store(snapshots, std::memory_order_release);
    m_snapshotReaderId.store(readerId, std::memory_order_release);
    publish_rt_state_locked();
  }

  void consume_snapshot(const vkpt::scene::RenderSceneSnapshot& snapshot) override {
    const auto listener = ListenerFromSnapshotCamera(snapshot.camera);
    std::lock_guard lock(m_mutex);
    m_currentFlowId.store(snapshot.generation, std::memory_order_relaxed);
    m_listener = listener;
    ++m_diag.listener_updates;
    record_snapshot_metrics(snapshot);
    set_listener_locked(m_listener);
    VKP_LOG(Debug,
            "audio",
            "snapshot_consumed",
            "flow_id",
            snapshot.generation,
            "listener_updates",
            m_diag.listener_updates);
  }

  void update() override {
    std::lock_guard lock(m_mutex);
    drain_audio_commands_locked();
    sync_runtime_voice_state_from_rt_locked();
    const auto voicesBeforeCleanup = m_voices.size();
    m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [&](const auto& voice) {
      return voice == nullptr || voice_finished_locked(*voice);
    }), m_voices.end());
    m_diag.active_voices = active_voice_count_locked();
    m_diag.loaded_clips = m_clips.size();
    m_diag.loaded_streams = static_cast<std::size_t>(std::count_if(m_events.begin(), m_events.end(), [](const auto& item) {
      return item.second.loop || item.second.bus == "music" || item.second.bus == "ambience";
    }));
    m_diag.events = m_events.size();
    if (m_voices.size() != voicesBeforeCleanup) {
      publish_rt_state_locked();
    }
  }

  AudioDiagnostics diagnostics() const override {
    std::lock_guard lock(m_mutex);
    auto out = m_diag;
    out.active_voices = active_voice_count_locked();
    out.loaded_clips = m_clips.size();
    out.events = m_events.size();
    out.queued_commands = m_soundRing.depth();
    out.mixed_buffers += m_rtMixedBuffers.load(std::memory_order_relaxed);
    out.underruns += m_rtUnderruns.load(std::memory_order_relaxed);
    out.streaming_voices = m_rtStreamingVoices.load(std::memory_order_relaxed);
    out.decode_jobs_completed = m_rtDecodeJobsCompleted.load(std::memory_order_relaxed);
    out.stream_pages_produced = m_rtStreamPagesProduced.load(std::memory_order_relaxed);
    out.stream_pages_consumed = m_rtStreamPagesConsumed.load(std::memory_order_relaxed);
    out.stream_ring_underruns = m_rtStreamRingUnderruns.load(std::memory_order_relaxed);
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
  void enqueue_decode_request(AudioDecodeRequest request) {
    {
      std::lock_guard lock(m_decodeMutex);
      m_decodeRequests.push_back(std::move(request));
    }
    m_decodeCv.notify_one();
  }

  void enqueue_stream_fill_request(VoiceStreamFillRequest request) {
    if (!request.ring || !request.samples || request.samples->empty() || request.channels == 0u) {
      return;
    }
    {
      std::lock_guard lock(m_decodeMutex);
      m_streamFillRequests.push_back(std::move(request));
    }
    m_decodeCv.notify_one();
  }

  void decode_thread_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
      AudioDecodeRequest decodeRequest;
      VoiceStreamFillRequest streamRequest;
      bool hasDecodeRequest = false;
      bool hasStreamRequest = false;
      {
        std::unique_lock lock(m_decodeMutex);
        m_decodeCv.wait(lock, [&] {
          return stop.stop_requested() ||
                 !m_decodeRequests.empty() ||
                 !m_streamFillRequests.empty();
        });
        if (stop.stop_requested() &&
            m_decodeRequests.empty() &&
            m_streamFillRequests.empty()) {
          break;
        }
        if (!m_decodeRequests.empty()) {
          decodeRequest = std::move(m_decodeRequests.front());
          m_decodeRequests.pop_front();
          hasDecodeRequest = true;
        } else if (!m_streamFillRequests.empty()) {
          streamRequest = std::move(m_streamFillRequests.front());
          m_streamFillRequests.pop_front();
          hasStreamRequest = true;
        }
      }

      if (hasDecodeRequest) {
        process_decode_request(std::move(decodeRequest));
      } else if (hasStreamRequest) {
        fill_stream_ring(std::move(streamRequest));
      }
    }
  }

  void process_decode_request(AudioDecodeRequest request) {
    ClipDef decoded;
    bool decodedFile = false;
#if defined(PT_ENABLE_MINIAUDIO)
    ma_decoder_config decoderConfig =
        ma_decoder_config_init(ma_format_f32, request.channels, request.sample_rate);
    ma_uint64 frameCount = 0;
    void* pcmFrames = nullptr;
    const ma_result decodeResult =
        ma_decode_file(request.file_path.c_str(), &decoderConfig, &frameCount, &pcmFrames);
    if (decodeResult == MA_SUCCESS && pcmFrames != nullptr && frameCount > 0u) {
      decoded.id = request.clip;
      decoded.uri = request.uri;
      decoded.name = EventNameFromUri(request.uri);
      decoded.file_path = request.file_path;
      decoded.sample_rate = request.sample_rate;
      decoded.channels = request.channels;
      decoded.generated = false;
      const auto sampleCount =
          static_cast<std::size_t>(frameCount) * static_cast<std::size_t>(decoded.channels);
      const auto* samples = static_cast<const float*>(pcmFrames);
      decoded.samples.assign(samples, samples + sampleCount);
      ma_free(pcmFrames, nullptr);
      decodedFile = true;
    } else if (pcmFrames != nullptr) {
      ma_free(pcmFrames, nullptr);
    }
#else
    (void)request;
#endif
    if (!decodedFile) {
      decoded = GenerateToneClip("tone:decode_thread_fallback", m_config.sample_rate);
      decoded.id = request.clip;
      decoded.uri = request.uri;
      decoded.name = EventNameFromUri(request.uri);
      decoded.file_path = request.file_path;
      decoded.generated = true;
    }
    decoded.rt_samples = std::make_shared<const std::vector<float>>(decoded.samples);
    m_rtDecodeJobsCompleted.fetch_add(1u, std::memory_order_relaxed);
    {
      std::lock_guard lock(m_mutex);
      const auto it = m_clips.find(request.clip);
      if (it != m_clips.end()) {
        it->second = std::move(decoded);
        publish_rt_state_locked();
      }
    }
  }

  void fill_stream_ring(VoiceStreamFillRequest request) {
    if (!request.ring || !request.samples || request.samples->empty()) {
      return;
    }
    const std::uint32_t channels = std::max<std::uint32_t>(1u, request.channels);
    const auto frameCount = request.samples->size() / channels;
    if (frameCount == 0u) {
      return;
    }
    const std::uint32_t framesPerPage =
        std::max<std::uint32_t>(1u, kPcmPageSampleCapacity / channels);
    double startFrame = request.start_frame;
    if (!std::isfinite(startFrame) || startFrame < 0.0) {
      startFrame = 0.0;
    }
    if (request.loop) {
      startFrame = std::fmod(startFrame, static_cast<double>(frameCount));
      if (startFrame < 0.0) {
        startFrame = 0.0;
      }
    } else {
      startFrame = std::min(startFrame, static_cast<double>(frameCount));
    }
    std::size_t cursor = static_cast<std::size_t>(startFrame);
    std::uint64_t pagesProduced = 0u;
    while (pagesProduced < request.ring->capacity()) {
      PcmPage page;
      page.stream_id = request.stream_id;
      page.channels = channels;
      while (page.frames < framesPerPage) {
        if (cursor >= frameCount) {
          if (!request.loop) {
            break;
          }
          cursor = 0u;
        }
        const auto sourceBase = cursor * channels;
        const auto destBase =
            static_cast<std::size_t>(page.frames) * static_cast<std::size_t>(channels);
        if (destBase + channels > kPcmPageSampleCapacity) {
          break;
        }
        for (std::uint32_t channel = 0u; channel < channels; ++channel) {
          page.samples[destBase + channel] = (*request.samples)[sourceBase + channel];
        }
        ++page.frames;
        ++cursor;
      }
      if (page.frames == 0u || !request.ring->try_push(page)) {
        break;
      }
      ++pagesProduced;
      if (!request.loop && cursor >= frameCount) {
        break;
      }
    }
    if (pagesProduced > 0u) {
      m_rtStreamPagesProduced.fetch_add(pagesProduced, std::memory_order_relaxed);
    }
  }

  void bind_audio_metrics_locked() {
    auto& registry = vkpt::core::metrics::MetricsRegistry::instance();
    m_metricCallbackLatencyUs = &registry.histogram("vkp.audio.callback_latency_us");
    m_metricCallbackJitterUs = &registry.histogram("vkp.audio.callback_jitter_us");
    m_metricMixedBuffers = &registry.counter("vkp.audio.mixed_buffers_total");
    m_metricUnderruns = &registry.counter("vkp.audio.underruns_total");
    m_metricSoundRingDrops = &registry.counter("vkp.audio.soundring_dropped_total");
    m_metricSoundRingDepth = &registry.gauge("vkp.audio.soundring_depth");
    m_metricActiveVoices = &registry.gauge("vkp.audio.active_voices");
    m_metricVoicesActive = &registry.gauge("vkp.audio.voices_active");
    m_metricEventsPosted = &registry.counter("vkp.audio.events_posted_total");
    m_metricSnapshotAgeMs = &registry.gauge("vkp.audio.snapshot_age_ms");
    m_metricSnapshotGeneration = &registry.gauge("vkp.audio.snapshot_generation");
  }

  void update_audio_queue_metrics_locked() {
    m_diag.queued_commands = m_soundRing.depth();
    if (m_metricSoundRingDepth != nullptr) {
      m_metricSoundRingDepth->set(static_cast<double>(m_diag.queued_commands));
    }
  }

  void register_health_probe_locked() {
    bool expected = false;
    if (!m_healthProbeRegistered.compare_exchange_strong(expected,
                                                         true,
                                                         std::memory_order_acq_rel)) {
      return;
    }
    AudioStatus initialStatus;
    initialStatus.initialized = m_initialized;
    initialStatus.last_error = m_diag.last_error;
    initialStatus.last_underrun_ns = m_lastUnderrunNs.load(std::memory_order_relaxed);
    initialStatus.last_callback_start_ns = m_lastCallbackStartNs.load(std::memory_order_relaxed);
    initialStatus.errors_total = m_diag.failed_play_requests + m_diag.dropped_commands +
                                 m_diag.underruns + m_diag.stream_ring_underruns;
    publish_health_state(initialStatus);
    std::weak_ptr<AudioHealthState> weakHealth = m_healthState;
    vkpt::core::health::HealthRegistry::instance().register_probe(
        std::make_shared<vkpt::core::health::FunctionProbe>(
            std::string(kAudioSubsystemName),
            [weakHealth] {
              const auto health = weakHealth.lock();
              if (!health) {
                return vkpt::core::health::Report{
                    vkpt::core::health::Status::Ok,
                    "audio probe expired"};
              }
              const auto nowNs = SteadyNowNs();
              const auto lastUnderrunNs =
                  health->last_underrun_ns.load(std::memory_order_relaxed);
              if (lastUnderrunNs != 0u &&
                  nowNs >= lastUnderrunNs &&
                  nowNs - lastUnderrunNs <= 5000000000ull) {
                return vkpt::core::health::Report{
                    vkpt::core::health::Status::Failed,
                    "recent audio underrun"};
              }
              if (health->errors_total.load(std::memory_order_relaxed) > 0u) {
                std::string lastError;
                {
                  std::scoped_lock lock(health->mutex);
                  lastError = health->last_error;
                }
                return vkpt::core::health::Report{
                    vkpt::core::health::Status::Degraded,
                    lastError.empty() ? "audio errors observed" : lastError};
              }
              const auto lastCallbackStartNs =
                  health->last_callback_start_ns.load(std::memory_order_relaxed);
              if (health->initialized.load(std::memory_order_relaxed) &&
                  lastCallbackStartNs != 0u &&
                  nowNs > lastCallbackStartNs + 2000000000ull) {
                return vkpt::core::health::Report{
                    vkpt::core::health::Status::Degraded,
                    "audio callback stalled"};
              }
              return vkpt::core::health::Report{
                  vkpt::core::health::Status::Ok,
                  health->initialized.load(std::memory_order_relaxed)
                      ? "audio ready"
                      : "audio uninitialized"};
            }));
  }

  void publish_health_state(const AudioStatus& status) const {
    if (!m_healthState) {
      return;
    }
    m_healthState->initialized.store(status.initialized, std::memory_order_relaxed);
    m_healthState->last_underrun_ns.store(status.last_underrun_ns, std::memory_order_relaxed);
    m_healthState->last_callback_start_ns.store(status.last_callback_start_ns,
                                                std::memory_order_relaxed);
    m_healthState->errors_total.store(status.errors_total, std::memory_order_relaxed);
    {
      std::scoped_lock lock(m_healthState->mutex);
      m_healthState->last_error = status.last_error;
    }
  }

  void update_voice_metrics_locked() {
    m_diag.active_voices = active_voice_count_locked();
    if (m_metricActiveVoices != nullptr) {
      m_metricActiveVoices->set(static_cast<double>(m_diag.active_voices));
    }
    if (m_metricVoicesActive != nullptr) {
      m_metricVoicesActive->set(static_cast<double>(m_diag.active_voices));
    }
  }

  void record_snapshot_metrics(const vkpt::scene::RenderSceneSnapshot& snapshot) {
    m_rtSnapshotGeneration.store(snapshot.generation, std::memory_order_relaxed);
    if (m_metricSnapshotGeneration != nullptr) {
      m_metricSnapshotGeneration->set(static_cast<double>(snapshot.generation));
    }
    if (snapshot.wall_time_ns != 0u) {
      const auto nowNs = vkpt::scene::SnapshotWallTimeNowNs();
      const auto ageMs = nowNs > snapshot.wall_time_ns
          ? static_cast<double>(nowNs - snapshot.wall_time_ns) / 1000000.0
          : 0.0;
      m_rtSnapshotAgeMs.store(static_cast<std::uint64_t>(ageMs), std::memory_order_relaxed);
      if (m_metricSnapshotAgeMs != nullptr) {
        m_metricSnapshotAgeMs->set(ageMs);
      }
    } else {
      m_rtSnapshotAgeMs.store(0u, std::memory_order_relaxed);
      if (m_metricSnapshotAgeMs != nullptr) {
        m_metricSnapshotAgeMs->set(0.0);
      }
    }
  }

  void sync_runtime_voice_state_from_rt_locked() {
    auto state = m_rtMixState.load(std::memory_order_acquire);
    if (!state) {
      return;
    }
    for (auto& voice : m_voices) {
      if (!voice) {
        continue;
      }
      const auto rtVoice = std::find_if(state->voices.begin(), state->voices.end(), [&](const RtVoice& item) {
        return item.handle.slot == voice->handle.slot && item.handle.generation == voice->handle.generation;
      });
      if (rtVoice == state->voices.end()) {
        continue;
      }
      if (std::isfinite(rtVoice->cursor_frame)) {
        voice->cursor_frame = std::max(0.0, rtVoice->cursor_frame);
      }
      if (!rtVoice->playing && !voice->loop) {
        voice->playing = false;
      }
    }
  }

  void publish_rt_state_locked() {
    sync_runtime_voice_state_from_rt_locked();
    auto state = std::make_shared<RtMixState>();
    state->generation = ++m_rtStateGeneration;
    state->muted = m_config.muted;
    state->listener = m_listener;
    state->clips.reserve(m_clips.size());
    for (const auto& item : m_clips) {
      const auto& clip = item.second;
      if (!clip.rt_samples || clip.rt_samples->empty() || clip.channels == 0u) {
        continue;
      }
      state->clips.push_back(RtClip{
          clip.id,
          clip.rt_samples,
          clip.channels,
          clip.frame_count()});
    }

    state->voices.reserve(m_voices.size());
    for (const auto& voice : m_voices) {
      if (!voice || voice_finished_locked(*voice)) {
        continue;
      }
      const auto clipIt = m_clips.find(voice->clip);
      if (clipIt == m_clips.end() || !clipIt->second.rt_samples) {
        continue;
      }
      const auto busIt = m_buses.find(voice->bus);
      const float busGain = busIt == m_buses.end()
          ? BusDefaultGain(voice->bus)
          : (busIt->second.muted ? 0.0f : busIt->second.volume * BusDefaultGain(busIt->second.name));
      auto pcmRing = std::make_shared<VoicePcmRing>(256u);
      const std::uint64_t streamId = m_nextStreamId++;
      enqueue_stream_fill_request(VoiceStreamFillRequest{
          streamId,
          pcmRing,
          clipIt->second.rt_samples,
          clipIt->second.channels,
          voice->loop,
          voice->cursor_frame});
      state->voices.push_back(RtVoice{
          voice->handle,
          voice->clip,
          voice->entity,
          voice->position,
          voice->playing,
          voice->loop,
          voice->spatial,
          voice->volume,
          voice->pitch,
          voice->pan,
          voice->gain,
          voice->min_distance,
          voice->max_distance,
          busGain,
          voice->cursor_frame,
          streamId,
          std::move(pcmRing)});
    }
    m_rtStreamingVoices.store(state->voices.size(), std::memory_order_relaxed);
    m_rtMixState.store(std::move(state), std::memory_order_release);
  }

  bool m_hardware_playback_enabled_locked() const {
    return m_hardwarePlaybackEnabled;
  }

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
        vkpt::log::Logger::instance().log(vkpt::log::Severity::Debug,
                                           "audio",
                                           "queueing background audio decode",
                                           {{"uri", std::string(uri)},
                                            {"path", clip.file_path}});
        auto pending = GenerateToneClip("tone:decode_pending", m_config.sample_rate);
        clip.samples = std::move(pending.samples);
        enqueue_decode_request(AudioDecodeRequest{
            clip.id,
            std::string(uri),
            clip.file_path,
            m_config.sample_rate,
            m_config.channels});
      }
    }

    if (clip.id == 0) {
      clip.id = Fnv1a64(key);
    }
    clip.rt_samples = std::make_shared<const std::vector<float>>(clip.samples);
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

  AudioEventHandle post_event_locked(const AudioTrackedEventDesc& desc) {
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
      VKP_LOG(Warn,
              "audio",
              "voice_allocation_failed",
              "asset_id",
              static_cast<std::uint64_t>(0),
              "reason",
              "asset_missing",
              "event",
              eventName);
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
      VKP_LOG(Warn,
              "audio",
              "voice_allocation_failed",
              "asset_id",
              static_cast<std::uint64_t>(event.clips[variantIndex]),
              "reason",
              "asset_missing",
              "event",
              event.name);
      return {};
    }

    const auto busName = NormalizeEventName(desc.bus.empty() ? event.bus : desc.bus);
    auto& bus = bus_state_locked(busName);
    ++bus.play_requests;
    append_history_locked(event.name + " -> " + clipIt->second.uri);

    const auto position = desc.has_position ? desc.position : vkpt::scene::Vec3{};
    const auto spatial = CalculateSpatialMix(m_listener, position, event.min_distance, event.max_distance);
    const bool spatialEnabled = desc.spatial || event.spatial;
    const float volume = std::max(0.0f, desc.volume * event.volume);
    const float pitch = std::clamp(desc.pitch * event.pitch, 0.25f, 4.0f);
    const bool looped = desc.loop || event.loop || clipIt->second.loop_default;
    const bool longLived = looped || busName == "music" || busName == "ambience";
    if (longLived) {
      sync_runtime_voice_state_from_rt_locked();
      for (auto& voice : m_voices) {
        if (!voice || voice_finished_locked(*voice) || voice->event_name != event.name ||
            voice->bus != busName || voice->entity != desc.entity) {
          continue;
        }
        voice->loop = voice->loop || looped;
        voice->spatial = spatialEnabled;
        voice->volume = volume;
        voice->pitch = pitch;
        voice->position = position;
        voice->pan = spatialEnabled ? spatial.pan : 0.0f;
        voice->gain = spatialEnabled ? spatial.gain : 1.0f;
        voice->min_distance = event.min_distance;
        voice->max_distance = event.max_distance;
        voice->priority = std::clamp(desc.priority > 0.0f ? desc.priority : event.priority, 0.0f, 1.0f);
        publish_rt_state_locked();
        vkpt::log::Logger::instance().log(
            vkpt::log::Severity::Debug,
            "audio",
            "audio loop already active",
            {{"event", event.name},
             {"entity", std::to_string(desc.entity)},
             {"bus", busName}});
        return voice->handle;
      }
    }

    const AudioVoiceHandle handle = allocate_handle_locked();

    AudioCmd cmd;
    cmd.kind = AudioCmdKind::PlayResolved;
    cmd.cmd_id = m_nextAudioCmdId++;
    cmd.handle = handle;
    cmd.clip = clipIt->first;
    cmd.entity = desc.entity;
    cmd.position = position;
    cmd.event_name = event.name;
    cmd.clip_uri = clipIt->second.uri;
    cmd.bus = busName;
    cmd.volume = volume;
    cmd.pitch = pitch;
    cmd.pan = spatialEnabled ? spatial.pan : 0.0f;
    cmd.gain = spatialEnabled ? spatial.gain : 1.0f;
    cmd.min_distance = event.min_distance;
    cmd.max_distance = event.max_distance;
    cmd.priority = std::clamp(desc.priority > 0.0f ? desc.priority : event.priority, 0.0f, 1.0f);
    cmd.loop = looped;
    cmd.spatial = spatialEnabled;
    if (!enqueue_audio_cmd_locked(std::move(cmd))) {
      ++m_diag.failed_play_requests;
      VKP_LOG(Warn,
              "audio",
              "voice_allocation_failed",
              "asset_id",
              static_cast<std::uint64_t>(clipIt->first),
              "reason",
              "backend_error",
              "event",
              event.name);
      return {};
    }
    drain_audio_commands_locked();
    if (m_metricEventsPosted != nullptr) {
      m_metricEventsPosted->inc();
    }
    VKP_LOG(Debug,
            "audio",
            "voice_allocated",
            "asset_id",
            static_cast<std::uint64_t>(clipIt->first),
            "voice_count",
            static_cast<std::uint64_t>(m_diag.active_voices),
            "voice_slot",
            static_cast<std::uint64_t>(handle.slot));
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Debug,
        "audio",
        m_diag.backend_name == "noop" || m_config.muted ? "audio event posted" : "audio event queued",
        {{"event", event.name},
         {"entity", std::to_string(desc.entity)},
         {"clip", clipIt->second.uri},
         {"bus", busName},
         {"spatial", spatialEnabled ? "true" : "false"},
         {"loop", looped ? "true" : "false"},
         {"hardware", m_hardware_playback_enabled_locked() ? "true" : "false"}});
    return handle;
  }

  bool enqueue_audio_cmd_locked(AudioCmd cmd) {
    const AudioCmdKind cmdKind = cmd.kind;
    const auto droppedBefore = m_soundRing.dropped_total();
    if (!m_soundRing.try_push(std::move(cmd))) {
      ++m_diag.dropped_commands;
      if (m_metricSoundRingDrops != nullptr) {
        m_metricSoundRingDrops->inc();
      }
      update_audio_queue_metrics_locked();
      VKP_LOG(Warn,
              "audio",
              "event_dropped",
              "cmd_type",
              AudioCmdKindName(cmdKind),
              "reason",
              "sound_ring_full",
              "dropped_total",
              m_diag.dropped_commands);
      return false;
    }
    const auto droppedAfter = m_soundRing.dropped_total();
    if (droppedAfter > droppedBefore) {
      const auto droppedDelta = droppedAfter - droppedBefore;
      m_diag.dropped_commands += droppedDelta;
      if (m_metricSoundRingDrops != nullptr) {
        m_metricSoundRingDrops->inc(static_cast<std::uint64_t>(droppedDelta));
      }
      VKP_LOG(Warn,
              "audio",
              "event_dropped",
              "cmd_type",
              AudioCmdKindName(cmdKind),
              "reason",
              "sound_ring_overwrite",
              "dropped_total",
              m_diag.dropped_commands);
    }
    update_audio_queue_metrics_locked();
    return true;
  }

  void drain_audio_commands_locked() {
    bool changed = false;
    AudioCmd cmd;
    while (m_soundRing.try_pop(cmd)) {
      switch (cmd.kind) {
        case AudioCmdKind::PlayOneShot:
        case AudioCmdKind::StartLoop:
        case AudioCmdKind::PlayResolved:
          changed = apply_play_cmd_locked(cmd) || changed;
          break;
        case AudioCmdKind::StopLoop:
        case AudioCmdKind::StopVoice:
          changed = apply_stop_cmd_locked(cmd.handle) || changed;
          break;
        case AudioCmdKind::SetVolume:
          changed = apply_set_volume_cmd_locked(cmd.handle, cmd.volume) || changed;
          break;
        case AudioCmdKind::StopAll:
          clear_runtime_voices_locked();
          changed = true;
          break;
        case AudioCmdKind::SetListener:
          m_listener = cmd.listener;
          set_listener_locked(m_listener);
          changed = true;
          break;
        case AudioCmdKind::SetBusVolume: {
          auto& state = bus_state_locked(cmd.bus);
          state.volume = std::clamp(cmd.volume, 0.0f, 2.0f);
          changed = true;
          break;
        }
        case AudioCmdKind::SetBusMuted: {
          auto& state = bus_state_locked(cmd.bus);
          state.muted = cmd.bus_muted;
          changed = true;
          break;
        }
      }
    }
    update_audio_queue_metrics_locked();
    if (changed) {
      update_voice_metrics_locked();
      publish_rt_state_locked();
    }
  }

  bool apply_play_cmd_locked(const AudioCmd& cmd) {
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
          VKP_LOG(Warn,
                  "audio",
                  "voice_allocation_failed",
                  "asset_id",
                  static_cast<std::uint64_t>(cmd.clip),
                  "reason",
                  "voices_full",
                  "voice_count",
                  static_cast<std::uint64_t>(m_voices.size()));
          return false;
        }
        if (*victim) {
          (*victim)->stolen = true;
          m_stolenEventHandles.insert(PackAudioHandle((*victim)->handle));
          VKP_LOG(Debug,
                  "audio",
                  "voice_freed",
                  "asset_id",
                  static_cast<std::uint64_t>((*victim)->clip),
                  "voice_count",
                  static_cast<std::uint64_t>(m_voices.size()),
                  "reason",
                  "stolen");
          stop_voice_locked(**victim);
          ++m_diag.stolen_voices;
        }
        m_voices.erase(victim);
      }
    }

    auto voice = std::make_unique<RuntimeVoice>();
    voice->handle = cmd.handle;
    voice->clip = cmd.clip;
    voice->event_name = cmd.event_name;
    voice->loop = cmd.loop;
    voice->spatial = cmd.spatial;
    voice->entity = cmd.entity;
    voice->bus = cmd.bus;
    voice->clip_uri = cmd.clip_uri;
    voice->playing = true;
    voice->priority = cmd.priority;
    voice->volume = cmd.volume;
    voice->pitch = cmd.pitch;
    voice->position = cmd.position;
    voice->pan = cmd.pan;
    voice->gain = cmd.gain;
    voice->min_distance = cmd.min_distance;
    voice->max_distance = cmd.max_distance;
    m_voices.push_back(std::move(voice));
    return true;
  }

  bool apply_stop_cmd_locked(AudioVoiceHandle handle) {
    bool changed = false;
    for (auto& voice : m_voices) {
      if (voice && voice->handle.slot == handle.slot && voice->handle.generation == handle.generation) {
        stop_voice_locked(*voice);
        changed = true;
      }
    }
    return changed;
  }

  bool apply_set_volume_cmd_locked(AudioVoiceHandle handle, float volume) {
    bool changed = false;
    for (auto& voice : m_voices) {
      if (voice && voice->handle.slot == handle.slot && voice->handle.generation == handle.generation) {
        voice->volume = std::clamp(volume, 0.0f, 2.0f);
        changed = true;
      }
    }
    return changed;
  }

  std::size_t active_voice_count_locked() const {
    return static_cast<std::size_t>(std::count_if(m_voices.begin(), m_voices.end(), [&](const auto& voice) {
      return voice != nullptr && !voice_finished_locked(*voice);
    }));
  }

  void set_listener_locked(const AudioListenerState& listener) {
    (void)listener;
  }

  void record_callback_metrics(std::uint64_t callbackStartNs,
                               std::uint32_t frameCount) {
    const auto callbackEndNs = SteadyNowNs();
    if (m_metricCallbackLatencyUs != nullptr && callbackEndNs >= callbackStartNs) {
      m_metricCallbackLatencyUs->record((callbackEndNs - callbackStartNs) / 1000u);
    }
    const auto previousStart =
        m_lastCallbackStartNs.exchange(callbackStartNs, std::memory_order_relaxed);
    if (previousStart != 0u && callbackStartNs > previousStart &&
        m_metricCallbackJitterUs != nullptr && m_config.sample_rate != 0u) {
      const auto actualPeriodNs = callbackStartNs - previousStart;
      const auto expectedPeriodNs =
          (static_cast<std::uint64_t>(frameCount) * 1000000000ull) /
          static_cast<std::uint64_t>(m_config.sample_rate);
      const auto jitterNs = actualPeriodNs > expectedPeriodNs
          ? actualPeriodNs - expectedPeriodNs
          : expectedPeriodNs - actualPeriodNs;
      m_metricCallbackJitterUs->record(jitterNs / 1000u);
    }
  }

  static void audio_device_render_callback(void* user,
                                           float* output,
                                           std::uint32_t frameCount,
                                           std::uint32_t outputChannels) {
    auto* self = static_cast<EngineAudioSystem*>(user);
    if (self != nullptr) {
      self->mix_device_output(output, frameCount, outputChannels);
    }
  }

  void mix_device_output(float* out, std::uint32_t frameCount, std::uint32_t outputChannels) {
    const auto callbackStartNs = SteadyNowNs();
    auto state = m_rtMixState.load(std::memory_order_acquire);
    if (!state || state->muted || out == nullptr || frameCount == 0u) {
      record_callback_metrics(callbackStartNs, frameCount);
      return;
    }

    AudioListenerState listener = state->listener;
    vkpt::scene::RenderSceneSnapshot::Ptr snapshot;
    if (auto* snapshots = m_snapshotRing.load(std::memory_order_acquire)) {
      const auto readerId = m_snapshotReaderId.load(std::memory_order_acquire);
      snapshot = readerId == vkpt::scene::SnapshotRing::kInvalidReader
          ? snapshots->current()
          : snapshots->current(readerId);
      if (snapshot) {
        listener = ListenerFromSnapshotCamera(snapshot->camera);
        record_snapshot_metrics(*snapshot);
      }
    }

    std::size_t activeVoices = 0u;
    for (auto& voice : state->voices) {
      if (!voice.playing) {
        continue;
      }
      const auto clipIt = std::find_if(state->clips.begin(), state->clips.end(), [&](const RtClip& clip) {
        return clip.id == voice.clip;
      });
      if (clipIt == state->clips.end()) {
        voice.playing = false;
        continue;
      }

      const auto& clip = *clipIt;
      const auto clipFrames = clip.frame_count;
      if (!clip.samples || clip.samples->empty() || clip.channels == 0u || clipFrames == 0u) {
        voice.playing = false;
        continue;
      }

      vkpt::scene::Vec3 sourcePosition = voice.position;
      if (snapshot && voice.entity != 0u) {
        for (const auto& instance : snapshot->instances.view()) {
          if (instance.entity_id == voice.entity) {
            sourcePosition = ToSceneVec3(instance.translation);
            break;
          }
        }
      }
      const SpatialMix spatial = voice.spatial
          ? CalculateSpatialMix(listener, sourcePosition, voice.min_distance, voice.max_distance)
          : SpatialMix{voice.gain, voice.pan, false};
      const float dopplerPitch = snapshot
          ? DopplerPitchFromSnapshotMotion(*snapshot, voice.entity, listener, sourcePosition)
          : 1.0f;
      ++activeVoices;
      const auto& samples = *clip.samples;
      const double pitchStep = std::max(0.01, static_cast<double>(voice.pitch * dopplerPitch));
      for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        if (voice.cursor_frame >= static_cast<double>(clipFrames)) {
          if (voice.loop) {
            voice.cursor_frame = std::fmod(voice.cursor_frame, static_cast<double>(clipFrames));
          } else {
            voice.playing = false;
            break;
          }
        }

        const auto sourceFrame = static_cast<std::size_t>(voice.cursor_frame);
        const auto sourceBase = sourceFrame * static_cast<std::size_t>(clip.channels);
        const auto destBase = static_cast<std::size_t>(frame) * static_cast<std::size_t>(outputChannels);
        float streamFrame[kMaxStackStreamChannels] = {};
        bool hasStreamFrame = false;
        if (voice.pcm_ring) {
          if (voice.current_page_frame >= voice.current_page.frames) {
            if (voice.pcm_ring->try_pop(voice.current_page)) {
              voice.current_page_frame = 0u;
              m_rtStreamPagesConsumed.fetch_add(1u, std::memory_order_relaxed);
            }
          }
          if (voice.current_page_frame < voice.current_page.frames &&
              voice.current_page.channels > 0u) {
            const auto streamBase =
                static_cast<std::size_t>(voice.current_page_frame) *
                static_cast<std::size_t>(voice.current_page.channels);
            const auto streamChannels = std::min<std::uint32_t>(
                std::min<std::uint32_t>(outputChannels, voice.current_page.channels),
                kMaxStackStreamChannels);
            for (std::uint32_t channel = 0u; channel < streamChannels; ++channel) {
              streamFrame[channel] = voice.current_page.samples[streamBase + channel];
            }
            for (std::uint32_t channel = streamChannels;
                 channel < std::min<std::uint32_t>(outputChannels, kMaxStackStreamChannels);
                 ++channel) {
              streamFrame[channel] = streamFrame[streamChannels == 0u ? 0u : streamChannels - 1u];
            }
            ++voice.current_page_frame;
            hasStreamFrame = true;
          }
        }
        for (std::uint32_t channel = 0; channel < outputChannels; ++channel) {
          const auto sourceChannel =
              std::min<std::size_t>(channel, static_cast<std::size_t>(clip.channels - 1u));
          float channelGain = voice.volume * voice.bus_gain * spatial.gain;
          if (outputChannels >= 2u) {
            if (channel == 0u && spatial.pan > 0.0f) {
              channelGain *= 1.0f - std::clamp(spatial.pan, 0.0f, 1.0f);
            } else if (channel == 1u && spatial.pan < 0.0f) {
              channelGain *= 1.0f + std::clamp(spatial.pan, -1.0f, 0.0f);
            }
          }
          const float sourceSample =
              hasStreamFrame && channel < kMaxStackStreamChannels
                  ? streamFrame[channel]
                  : samples[sourceBase + sourceChannel];
          out[destBase + channel] += sourceSample * channelGain;
        }
        voice.cursor_frame += hasStreamFrame ? 1.0 : pitchStep;
      }
    }

    const auto sampleCount = static_cast<std::size_t>(frameCount) *
                             static_cast<std::size_t>(outputChannels);
    for (std::size_t i = 0; i < sampleCount; ++i) {
      out[i] = std::tanh(std::clamp(out[i], -8.0f, 8.0f));
    }
    m_rtMixedBuffers.fetch_add(1, std::memory_order_relaxed);
    if (m_metricMixedBuffers != nullptr) {
      m_metricMixedBuffers->inc();
    }
    if (m_metricActiveVoices != nullptr) {
      m_metricActiveVoices->set(static_cast<double>(activeVoices));
    }
    record_callback_metrics(callbackStartNs, frameCount);
  }

  bool start_voice_locked(RuntimeVoice& voice,
                          const ClipDef& clip,
                          float volume,
                          float pitch,
                          float minDistance,
                          float maxDistance,
                          const vkpt::scene::Vec3& position) {
#if defined(PT_ENABLE_MINIAUDIO)
    (void)voice;
    (void)clip;
    (void)volume;
    (void)pitch;
    (void)minDistance;
    (void)maxDistance;
    (void)position;
    m_diag.last_error = "legacy ma_sound playback path is disabled";
    return false;
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
    if (voice.playing) {
      VKP_LOG(Debug,
              "audio",
              "voice_freed",
              "asset_id",
              static_cast<std::uint64_t>(voice.clip),
              "voice_count",
              static_cast<std::uint64_t>(active_voice_count_locked()),
              "reason",
              "stopped");
    }
    voice.playing = false;
#if defined(PT_ENABLE_MINIAUDIO)
    if (voice.sound_initialized) {
      ma_sound_stop(&voice.sound);
    }
#endif
  }

  void clear_runtime_voices_locked() {
    for (auto& voice : m_voices) {
      if (voice) {
        stop_voice_locked(*voice);
      }
    }
    m_voices.clear();
    m_diag.active_voices = 0u;
    if (m_metricActiveVoices != nullptr) {
      m_metricActiveVoices->set(0.0);
    }
    if (m_metricVoicesActive != nullptr) {
      m_metricVoicesActive->set(0.0);
    }
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
  std::atomic<vkpt::core::contracts::ComponentLifecycle> m_lifecycle{
      vkpt::core::contracts::ComponentLifecycle::Uninitialized};
  AudioListenerState m_listener;
  std::unordered_map<AudioClipId, ClipDef> m_clips;
  std::unordered_map<std::string, AudioClipId> m_clipByUri;
  std::unordered_map<std::string, EventDef> m_events;
  std::unordered_map<std::string, BusState> m_buses;
  std::deque<std::string> m_eventHistory;
  std::unordered_set<std::uint64_t> m_stolenEventHandles;
  std::vector<std::unique_ptr<RuntimeVoice>> m_voices;
  std::uint32_t m_nextVoiceSlot = 1u;
  std::uint32_t m_nextVoiceGeneration = 1u;
  AudioDiagnostics m_diag;
  SoundRing m_soundRing{1024u};
  std::uint64_t m_nextAudioCmdId = 1u;
  std::uint64_t m_rtStateGeneration = 0;
  std::atomic<std::shared_ptr<RtMixState>> m_rtMixState;
  std::atomic<std::uint64_t> m_rtMixedBuffers{0};
  std::atomic<std::uint64_t> m_rtUnderruns{0};
  bool m_hardwarePlaybackEnabled = false;
  std::unique_ptr<IAudioDevice> m_audioDevice;
  std::atomic<vkpt::scene::SnapshotRing*> m_snapshotRing{nullptr};
  std::atomic<std::uint32_t> m_snapshotReaderId{vkpt::scene::SnapshotRing::kInvalidReader};
  std::atomic<std::uint64_t> m_rtSnapshotGeneration{0};
  std::atomic<std::uint64_t> m_rtSnapshotAgeMs{0};
  std::atomic<std::uint64_t> m_currentFlowId{0};
  std::atomic<std::uint64_t> m_lastCallbackStartNs{0};
  std::atomic<std::uint64_t> m_lastUnderrunNs{0};
  std::atomic<bool> m_healthProbeRegistered{false};
  std::shared_ptr<AudioHealthState> m_healthState = std::make_shared<AudioHealthState>();
  std::mutex m_decodeMutex;
  std::condition_variable m_decodeCv;
  std::deque<AudioDecodeRequest> m_decodeRequests;
  std::deque<VoiceStreamFillRequest> m_streamFillRequests;
  std::jthread m_decodeThread;
  std::uint64_t m_nextStreamId = 1u;
  std::atomic<std::size_t> m_rtStreamingVoices{0u};
  std::atomic<std::uint64_t> m_rtDecodeJobsCompleted{0u};
  std::atomic<std::uint64_t> m_rtStreamPagesProduced{0u};
  std::atomic<std::uint64_t> m_rtStreamPagesConsumed{0u};
  std::atomic<std::uint64_t> m_rtStreamRingUnderruns{0u};
  vkpt::core::metrics::Histogram* m_metricCallbackLatencyUs = nullptr;
  vkpt::core::metrics::Histogram* m_metricCallbackJitterUs = nullptr;
  vkpt::core::metrics::Counter* m_metricMixedBuffers = nullptr;
  vkpt::core::metrics::Counter* m_metricUnderruns = nullptr;
  vkpt::core::metrics::Counter* m_metricSoundRingDrops = nullptr;
  vkpt::core::metrics::Gauge* m_metricSoundRingDepth = nullptr;
  vkpt::core::metrics::Gauge* m_metricActiveVoices = nullptr;
  vkpt::core::metrics::Gauge* m_metricVoicesActive = nullptr;
  vkpt::core::metrics::Counter* m_metricEventsPosted = nullptr;
  vkpt::core::metrics::Gauge* m_metricSnapshotAgeMs = nullptr;
  vkpt::core::metrics::Gauge* m_metricSnapshotGeneration = nullptr;
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
