#pragma once

#include "editor/UiModels.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "scene/Json.h"

namespace vkpt::editor {

inline std::string EscapeJson(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

inline std::string_view LayoutName(LayoutPreset preset) {
  switch (preset) {
    case LayoutPreset::Default:
      return "Default";
    case LayoutPreset::Benchmark:
      return "Benchmark";
    case LayoutPreset::MaterialAuthoring:
      return "Material Authoring";
    case LayoutPreset::Scripting:
      return "Scripting";
    case LayoutPreset::AssetManagement:
      return "Asset Management";
    case LayoutPreset::DebugProfiler:
      return "Debug/Profiler";
    case LayoutPreset::MinimalViewport:
      return "Minimal Viewport";
    case LayoutPreset::FullscreenViewportWithOverlay:
      return "Fullscreen Viewport With Overlay";
    default:
      return "Default";
  }
}

inline std::string ToLower(std::string_view text) {
  std::string lowered = std::string(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lowered;
}

inline bool IsIn(const std::vector<std::string_view>& values, std::string_view key) {
  for (const auto value : values) {
    if (value == key) {
      return true;
    }
  }
  return false;
}

inline SelectionSource ParseSelectionSource(std::string_view source) {
  if (source == "viewport") {
    return SelectionSource::Viewport;
  }
  if (source == "scene_tree") {
    return SelectionSource::SceneTree;
  }
  if (source == "inspector") {
    return SelectionSource::Inspector;
  }
  if (source == "asset_browser") {
    return SelectionSource::AssetBrowser;
  }
  if (source == "script_panel") {
    return SelectionSource::ScriptPanel;
  }
  return SelectionSource::Unknown;
}

inline bool ReadJsonBool(const vkpt::scene::JsonValue& value, const std::string& key, bool& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Boolean) {
    return false;
  }
  out = it->second.boolean;
  return true;
}

inline bool ReadJsonString(const vkpt::scene::JsonValue& value, const std::string& key, std::string& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::String) {
    return false;
  }
  out = it->second.string;
  return true;
}

inline bool ReadJsonArray(const vkpt::scene::JsonValue& value, const std::string& key, std::vector<std::string>& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Array) {
    return false;
  }
  out.clear();
  out.reserve(it->second.array.size());
  for (const auto& entry : it->second.array) {
    if (entry.kind != vkpt::scene::JsonValue::Kind::String) {
      return false;
    }
    out.push_back(entry.string);
  }
  return true;
}

inline bool ReadJsonStableIdArray(const vkpt::scene::JsonValue& value,
                                  const std::string& key,
                                  std::vector<vkpt::core::StableId>& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Array) {
    return false;
  }
  out.clear();
  out.reserve(it->second.array.size());
  for (const auto& entry : it->second.array) {
    if (entry.kind != vkpt::scene::JsonValue::Kind::Number ||
        entry.number < 0.0 ||
        !std::isfinite(entry.number)) {
      return false;
    }
    out.push_back(static_cast<vkpt::core::StableId>(entry.number));
  }
  return true;
}

inline bool ReadJsonUInt64(const vkpt::scene::JsonValue& value, const std::string& key, std::uint64_t& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Number) {
    return false;
  }
  if (it->second.number < 0.0 || !std::isfinite(it->second.number)) {
    return false;
  }
  out = static_cast<std::uint64_t>(it->second.number);
  return true;
}

inline bool ReadJsonFloat(const vkpt::scene::JsonValue& value, const std::string& key, float& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Number) {
    return false;
  }
  out = static_cast<float>(it->second.number);
  return true;
}

inline bool ReadJsonVec3(const vkpt::scene::JsonValue& value, Vec3& out) {
  return ReadJsonFloat(value, "x", out.x) &&
         ReadJsonFloat(value, "y", out.y) &&
         ReadJsonFloat(value, "z", out.z);
}

inline bool ReadJsonBounds(const vkpt::scene::JsonValue& value, const std::string& key, Bounds& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto min_it = it->second.object.find("min");
  const auto max_it = it->second.object.find("max");
  if (min_it == it->second.object.end() || max_it == it->second.object.end() ||
      min_it->second.kind != vkpt::scene::JsonValue::Kind::Object ||
      max_it->second.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  Bounds parsed;
  if (!ReadJsonVec3(min_it->second, parsed.min) ||
      !ReadJsonVec3(max_it->second, parsed.max)) {
    return false;
  }
  ReadJsonBool(it->second, "valid", parsed.valid);
  out = parsed;
  return true;
}

}  // namespace vkpt::editor
