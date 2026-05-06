#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "scene/SceneTypes.h"

namespace vkpt::scene {

struct JsonValue;

class JsonParser {
 public:
  [[nodiscard]] static std::optional<JsonValue> parse(std::string_view text);
  [[nodiscard]] static std::string stringify(const JsonValue& value);
};

struct JsonValue {
  enum class Kind : std::uint8_t { Null, Boolean, Number, String, Array, Object };

  Kind kind = Kind::Null;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  std::vector<JsonValue> array;
  std::unordered_map<std::string, JsonValue> object;
};

std::string stringify(const JsonValue& value, bool pretty = false);

bool read_string(const JsonValue& object, std::string_view key, std::string& out);
bool read_u64(const JsonValue& object, std::string_view key, std::uint64_t& out);
bool read_u32(const JsonValue& object, std::string_view key, std::uint32_t& out);
bool read_float(const JsonValue& object, std::string_view key, float& out);
bool read_bool(const JsonValue& object, std::string_view key, bool& out);
bool read_vec3(const JsonValue& object, std::string_view key, Vec3& out);
bool read_vec3_list(const JsonValue& object, std::string_view key, std::vector<Vec3>& out);
bool read_vec2_list(const JsonValue& object, std::string_view key, std::vector<Vec2>& out);
bool read_u32_list(const JsonValue& object, std::string_view key, std::vector<std::uint32_t>& out);
bool read_quat(const JsonValue& object, std::string_view key, Quat& out);

}  // namespace vkpt::scene
