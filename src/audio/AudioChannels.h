#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "audio/AudioSystem.h"
#include "core/sync/MpscRing.h"
#include "core/sync/SpscRing.h"

namespace vkpt::audio {

enum class AudioCmdKind : std::uint8_t {
  PlayOneShot,
  StartLoop,
  StopLoop,
  SetVolume,
  PlayResolved,
  StopVoice,
  StopAll,
  SetListener,
  SetBusVolume,
  SetBusMuted
};

struct AudioCmd {
  AudioCmdKind kind = AudioCmdKind::PlayResolved;
  std::uint64_t cmd_id = 0;
  std::uint64_t snapshot_gen = 0;
  const char* producer = "engine";
  AudioVoiceHandle handle{};
  AudioClipId clip = 0;
  AudioListenerState listener{};
  vkpt::core::StableEntityId entity = 0;
  vkpt::scene::Vec3 position{0.0f, 0.0f, 0.0f};
  std::string event_name;
  std::string clip_uri;
  std::string bus = "sfx";
  float volume = 1.0f;
  float pitch = 1.0f;
  float pan = 0.0f;
  float gain = 1.0f;
  float min_distance = 1.0f;
  float max_distance = 24.0f;
  float priority = 0.5f;
  bool loop = false;
  bool spatial = false;
  bool bus_muted = false;
};

class SoundRing {
 public:
  explicit SoundRing(std::size_t capacity) : m_ring(capacity) {}

  bool try_push(AudioCmd cmd) {
    if (m_ring.try_push(std::move(cmd))) {
      return true;
    }
    AudioCmd dropped;
    if (m_ring.try_pop(dropped)) {
      m_dropped_oldest.fetch_add(1u, std::memory_order_relaxed);
      return m_ring.try_push(std::move(cmd));
    }
    return false;
  }

  bool try_pop(AudioCmd& out) {
    return m_ring.try_pop(out);
  }

  std::size_t capacity() const { return m_ring.capacity(); }
  std::size_t depth() const { return m_ring.depth(); }
  std::size_t dropped_total() const {
    return m_dropped_oldest.load(std::memory_order_relaxed);
  }

 private:
  ::vkpt::core::sync::MpscRing<AudioCmd> m_ring;
  std::atomic<std::size_t> m_dropped_oldest{0u};
};

struct PcmPage {
  std::uint64_t stream_id = 0;
  std::uint32_t frames = 0;
  std::uint32_t channels = 0;
  float samples[512] = {};
};

using VoicePcmRing = ::vkpt::core::sync::SpscRing<PcmPage>;

}  // namespace vkpt::audio
