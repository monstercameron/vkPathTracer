#pragma once

#include <cstdint>
#include <string>

namespace vkpt::audio {

struct AudioDeviceConfig {
  std::uint32_t sample_rate = 44100;
  std::uint32_t channels = 2;
  std::uint32_t buffer_frames = 512;
  std::string requested_device = "default";
};

struct AudioDeviceCallback {
  using RenderFn = void (*)(void* user, float* output, std::uint32_t frames, std::uint32_t channels);
  RenderFn render = nullptr;
  void* user = nullptr;
};

class IAudioDevice {
 public:
  virtual ~IAudioDevice() = default;
  virtual bool open(const AudioDeviceConfig& config, AudioDeviceCallback callback) = 0;
  virtual void close() = 0;
  virtual bool start() = 0;
  virtual void stop() = 0;
  virtual std::string device_name() const = 0;
};

}  // namespace vkpt::audio
