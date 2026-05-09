#include "animation/AnimationClip.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace vkpt::animation {

namespace {

bool TimesNonDecreasing(const std::vector<TranslationKey>& keys) {
  for (std::size_t i = 1; i < keys.size(); ++i) {
    if (keys[i].time < keys[i - 1].time) {
      return false;
    }
  }
  return true;
}
bool TimesNonDecreasing(const std::vector<RotationKey>& keys) {
  for (std::size_t i = 1; i < keys.size(); ++i) {
    if (keys[i].time < keys[i - 1].time) {
      return false;
    }
  }
  return true;
}
bool TimesNonDecreasing(const std::vector<ScaleKey>& keys) {
  for (std::size_t i = 1; i < keys.size(); ++i) {
    if (keys[i].time < keys[i - 1].time) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool validate(const AnimationClip& clip,
              std::int32_t skeleton_joint_count,
              std::vector<std::string>* issues) {
  bool ok = true;
  auto report = [&](std::string message) {
    ok = false;
    if (issues != nullptr) {
      issues->push_back(std::move(message));
    }
  };

  if (!std::isfinite(clip.duration_seconds) || clip.duration_seconds <= 0.0f) {
    report("clip '" + clip.name + "' has non-positive or non-finite duration");
  }

  for (std::size_t t = 0; t < clip.tracks.size(); ++t) {
    const auto& track = clip.tracks[t];
    if (track.joint_index < 0 || track.joint_index >= skeleton_joint_count) {
      report("clip '" + clip.name + "' track " + std::to_string(t) +
             " joint_index out of range");
      continue;
    }
    if (!TimesNonDecreasing(track.translation)) {
      report("clip '" + clip.name + "' track " + std::to_string(t) +
             " translation key times not non-decreasing");
    }
    if (!TimesNonDecreasing(track.rotation)) {
      report("clip '" + clip.name + "' track " + std::to_string(t) +
             " rotation key times not non-decreasing");
    }
    if (!TimesNonDecreasing(track.scale)) {
      report("clip '" + clip.name + "' track " + std::to_string(t) +
             " scale key times not non-decreasing");
    }
    for (const auto& key : track.translation) {
      if (!std::isfinite(key.time) || !std::isfinite(key.value.x) ||
          !std::isfinite(key.value.y) || !std::isfinite(key.value.z)) {
        report("clip '" + clip.name + "' track " + std::to_string(t) +
               " translation key has non-finite value");
        break;
      }
    }
    for (const auto& key : track.rotation) {
      if (!std::isfinite(key.time) || !std::isfinite(key.value.x) ||
          !std::isfinite(key.value.y) || !std::isfinite(key.value.z) ||
          !std::isfinite(key.value.w)) {
        report("clip '" + clip.name + "' track " + std::to_string(t) +
               " rotation key has non-finite value");
        break;
      }
    }
    for (const auto& key : track.scale) {
      if (!std::isfinite(key.time) || !std::isfinite(key.value.x) ||
          !std::isfinite(key.value.y) || !std::isfinite(key.value.z)) {
        report("clip '" + clip.name + "' track " + std::to_string(t) +
               " scale key has non-finite value");
        break;
      }
    }
  }

  return ok;
}

}  // namespace vkpt::animation
