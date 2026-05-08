#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/contracts/Lifecycle.h"

namespace vkpt::core::contracts {

enum class SubsystemHealth : std::uint8_t {
  Ok = 0,
  Degraded = 1,
  Failed = 2,
};

inline const char* SubsystemHealthName(SubsystemHealth status) noexcept {
  switch (status) {
    case SubsystemHealth::Ok:
      return "ok";
    case SubsystemHealth::Degraded:
      return "degraded";
    case SubsystemHealth::Failed:
      return "failed";
  }
  return "unknown";
}

struct StatusField {
  std::string name;
  std::string value;
};

struct TypedStatusFields {
  ComponentLifecycle lifecycle = ComponentLifecycle::Uninitialized;
  std::string last_error;
  std::uint64_t last_tick_ns = 0u;
  std::uint64_t ticks_total = 0u;
  std::uint64_t errors_total = 0u;
};

struct SubsystemStatus {
  std::string name;
  SubsystemHealth status = SubsystemHealth::Ok;
  std::uint64_t started_at_ns = 0u;
  std::uint64_t last_tick_ns = 0u;
  std::string last_error;
  std::uint64_t ticks_total = 0u;
  std::uint64_t errors_total = 0u;
  std::vector<StatusField> custom_fields;

  void set_custom(std::string key, std::string value) {
    for (auto& field : custom_fields) {
      if (field.name == key) {
        field.value = std::move(value);
        return;
      }
    }
    custom_fields.push_back({std::move(key), std::move(value)});
  }
};

inline SubsystemStatus MakeSubsystemStatus(std::string_view name,
                                           SubsystemHealth status = SubsystemHealth::Ok) {
  SubsystemStatus out;
  out.name = std::string(name);
  out.status = status;
  return out;
}

}  // namespace vkpt::core::contracts
