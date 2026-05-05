#include "scene/Scene.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

#include <cstdlib>
#include <cstring>

namespace vkpt::scene {

namespace {

std::uint32_t component_kind_mask(ComponentKind kind) {
  return 1u << static_cast<std::size_t>(kind);
}

uint8_t authority_rank(TransformAuthority authority) {
  switch (authority) {
    case TransformAuthority::BenchmarkFrozen:
      return 5;
    case TransformAuthority::PhysicsControlled:
      return 4;
    case TransformAuthority::AnimationControlled:
      return 3;
    case TransformAuthority::ScriptControlled:
      return 2;
    case TransformAuthority::EditorControlled:
      return 1;
    case TransformAuthority::Authored:
    default:
      return 0;
  }
}

class JsonParserImpl {
 public:
  explicit JsonParserImpl(std::string_view text) : m_text(text) {}
  std::optional<JsonValue> parse() {
    skip_ws();
    auto result = parse_value();
    skip_ws();
    if (!m_valid || m_position != m_text.size()) {
      return {};
    }
    return result;
  }

 private:
  static bool is_ws(char ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; }

  char peek() const {
    if (m_position >= m_text.size()) {
      return '\0';
    }
    return m_text[m_position];
  }

  bool consume(char ch) {
    if (peek() == ch) {
      ++m_position;
      return true;
    }
    return false;
  }

  bool expect(char ch) {
    if (consume(ch)) {
      return true;
    }
    m_valid = false;
    return false;
  }

  void skip_ws() {
    while (is_ws(peek())) {
      ++m_position;
    }
  }

  JsonValue parse_value() {
    skip_ws();
    const char head = peek();
    if (head == '{') {
      return parse_object();
    }
    if (head == '[') {
      return parse_array();
    }
    if (head == '"') {
      return parse_string();
    }
    if (head == '-' || (head >= '0' && head <= '9')) {
      return parse_number();
    }
    if (head == 't' || head == 'f' || head == 'n') {
      return parse_literal();
    }
    m_valid = false;
    return JsonValue{};
  }

  JsonValue parse_literal() {
    JsonValue value;
    if (m_text.substr(m_position, 4) == "true") {
      m_position += 4;
      value.kind = JsonValue::Kind::Boolean;
      value.boolean = true;
      return value;
    }
    if (m_text.substr(m_position, 5) == "false") {
      m_position += 5;
      value.kind = JsonValue::Kind::Boolean;
      value.boolean = false;
      return value;
    }
    if (m_text.substr(m_position, 4) == "null") {
      m_position += 4;
      value.kind = JsonValue::Kind::Null;
      return value;
    }
    m_valid = false;
    return value;
  }

  JsonValue parse_string() {
    JsonValue value;
    value.kind = JsonValue::Kind::String;
    if (!expect('"')) {
      return value;
    }
    while (m_position < m_text.size()) {
      char ch = m_text[m_position++];
      if (ch == '"') {
        return value;
      }
      if (ch == '\\') {
        char e = peek();
        if (e == '\0') {
          m_valid = false;
          return value;
        }
        ++m_position;
        switch (e) {
          case '"':
            value.string.push_back('"');
            break;
          case '\\':
            value.string.push_back('\\');
            break;
          case '/':
            value.string.push_back('/');
            break;
          case 'b':
            value.string.push_back('\b');
            break;
          case 'f':
            value.string.push_back('\f');
            break;
          case 'n':
            value.string.push_back('\n');
            break;
          case 'r':
            value.string.push_back('\r');
            break;
          case 't':
            value.string.push_back('\t');
            break;
          default:
            m_valid = false;
            return value;
        }
      } else {
        if (static_cast<unsigned char>(ch) < 0x20u) {
          m_valid = false;
          return value;
        }
        value.string.push_back(ch);
      }
    }
    m_valid = false;
    return value;
  }

  JsonValue parse_number() {
    JsonValue value;
    value.kind = JsonValue::Kind::Number;
    const auto start = m_position;
    if (peek() == '-') {
      ++m_position;
    }
    const auto integer_start = m_position;
    if (peek() == '0') {
      ++m_position;
    } else {
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        ++m_position;
      }
    }
    if (m_position == integer_start) {
      m_valid = false;
      return value;
    }
    if (peek() == '.') {
      ++m_position;
      const auto fractional_start = m_position;
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        ++m_position;
      }
      if (m_position == fractional_start) {
        m_valid = false;
        return value;
      }
    }
    if (peek() == 'e' || peek() == 'E') {
      ++m_position;
      if (peek() == '+' || peek() == '-') {
        ++m_position;
      }
      const auto exponent_start = m_position;
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        ++m_position;
      }
      if (m_position == exponent_start) {
        m_valid = false;
        return value;
      }
    }
    value.number = std::strtod(std::string(m_text.substr(start, m_position - start)).c_str(), nullptr);
    if (!std::isfinite(value.number)) {
      m_valid = false;
    }
    return value;
  }

  JsonValue parse_array() {
    JsonValue value;
    value.kind = JsonValue::Kind::Array;
    if (!expect('[')) {
      return value;
    }
    skip_ws();
    if (consume(']')) {
      return value;
    }
    while (m_valid) {
      value.array.push_back(parse_value());
      skip_ws();
      if (consume(']')) {
        return value;
      }
      if (!expect(',')) {
        return value;
      }
      skip_ws();
    }
    return value;
  }

  JsonValue parse_object() {
    JsonValue value;
    value.kind = JsonValue::Kind::Object;
    if (!expect('{')) {
      return value;
    }
    skip_ws();
    if (consume('}')) {
      return value;
    }
    while (m_valid) {
      skip_ws();
      if (peek() != '"') {
        m_valid = false;
        return value;
      }
      auto key = parse_string();
      skip_ws();
      if (!expect(':')) {
        return value;
      }
      skip_ws();
      value.object.emplace(key.string, parse_value());
      skip_ws();
      if (consume('}')) {
        return value;
      }
      if (!expect(',')) {
        return value;
      }
      skip_ws();
    }
    return value;
  }

  std::string_view m_text;
  std::size_t m_position = 0;
  bool m_valid = true;
};

void stringify_impl(const JsonValue& value, std::ostringstream& out, bool pretty, std::size_t depth);

void emit_indent(std::ostringstream& out, bool pretty, std::size_t depth) {
  if (!pretty) {
    return;
  }
  out << "\n";
  for (std::size_t i = 0; i < depth; ++i) {
    out << "  ";
  }
}

void escape_json(std::ostringstream& out, std::string_view text) {
  for (char ch : text) {
    switch (ch) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        out << ch;
        break;
    }
  }
}

void stringify_impl(const JsonValue& value, std::ostringstream& out, bool pretty, std::size_t depth) {
  switch (value.kind) {
    case JsonValue::Kind::Null:
      out << "null";
      break;
    case JsonValue::Kind::Boolean:
      out << (value.boolean ? "true" : "false");
      break;
    case JsonValue::Kind::Number: {
      std::ostringstream num;
      num << std::fixed << std::setprecision(6) << value.number;
      out << num.str();
      break;
    }
    case JsonValue::Kind::String:
      out << '"';
      escape_json(out, value.string);
      out << '"';
      break;
    case JsonValue::Kind::Array:
      out << "[";
      if (!value.array.empty()) {
        for (std::size_t i = 0; i < value.array.size(); ++i) {
          if (i) {
            out << ",";
          }
          emit_indent(out, pretty, depth + 1);
          stringify_impl(value.array[i], out, pretty, depth + 1);
        }
        emit_indent(out, pretty, depth);
      }
      out << "]";
      break;
    case JsonValue::Kind::Object:
      out << "{";
      if (!value.object.empty()) {
        std::size_t i = 0;
        for (const auto& entry : value.object) {
          if (i++) {
            out << ",";
          }
          emit_indent(out, pretty, depth + 1);
          out << "\"";
          escape_json(out, entry.first);
          out << "\":";
          if (pretty) out << " ";
          stringify_impl(entry.second, out, pretty, depth + 1);
        }
        emit_indent(out, pretty, depth);
      }
      out << "}";
      break;
  }
}

std::string stringify(const JsonValue& value, bool pretty = false) {
  std::ostringstream out;
  stringify_impl(value, out, pretty, 0);
  return out.str();
}

bool read_string(const JsonValue& object, std::string_view key, std::string& out) {
  auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != JsonValue::Kind::String) {
    return false;
  }
  out = it->second.string;
  return true;
}

bool read_u64(const JsonValue& object, std::string_view key, std::uint64_t& out) {
  auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != JsonValue::Kind::Number ||
      it->second.number < 0.0 ||
      it->second.number > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
    return false;
  }
  out = static_cast<std::uint64_t>(it->second.number);
  return true;
}

bool read_u32(const JsonValue& object, std::string_view key, std::uint32_t& out) {
  std::uint64_t value = 0;
  if (!read_u64(object, key, value)) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

bool read_float(const JsonValue& object, std::string_view key, float& out) {
  auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != JsonValue::Kind::Number || !std::isfinite(it->second.number)) {
    return false;
  }
  out = static_cast<float>(it->second.number);
  return true;
}

bool read_bool(const JsonValue& object, std::string_view key, bool& out) {
  auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != JsonValue::Kind::Boolean) {
    return false;
  }
  out = it->second.boolean;
  return true;
}

bool read_vec3(const JsonValue& object, std::string_view key, Vec3& out) {
  auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != JsonValue::Kind::Array || it->second.array.size() != 3) {
    return false;
  }
  if (std::any_of(it->second.array.begin(), it->second.array.end(), [](const JsonValue& value) {
        return value.kind != JsonValue::Kind::Number || !std::isfinite(value.number);
      })) {
    return false;
  }
  out.x = static_cast<float>(it->second.array[0].number);
  out.y = static_cast<float>(it->second.array[1].number);
  out.z = static_cast<float>(it->second.array[2].number);
  return true;
}

bool read_vec3_list(const JsonValue& object, std::string_view key, std::vector<Vec3>& out) {
  out.clear();
  auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != JsonValue::Kind::Array) {
    return false;
  }
  out.reserve(it->second.array.size());
  for (const auto& element : it->second.array) {
    if (element.kind != JsonValue::Kind::Array || element.array.size() != 3) {
      return false;
    }
    if (std::any_of(element.array.begin(), element.array.end(), [](const JsonValue& value) {
          return value.kind != JsonValue::Kind::Number || !std::isfinite(value.number);
        })) {
      return false;
    }
    Vec3 value{};
    value.x = static_cast<float>(element.array[0].number);
    value.y = static_cast<float>(element.array[1].number);
    value.z = static_cast<float>(element.array[2].number);
    out.push_back(value);
  }
  return true;
}

bool read_u32_list(const JsonValue& object, std::string_view key, std::vector<std::uint32_t>& out) {
  out.clear();
  auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != JsonValue::Kind::Array) {
    return false;
  }
  out.reserve(it->second.array.size());
  for (const auto& element : it->second.array) {
    if (element.kind != JsonValue::Kind::Number || element.number < 0.0 ||
        element.number > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
      return false;
    }
    out.push_back(static_cast<std::uint32_t>(element.number));
  }
  return true;
}

bool read_quat(const JsonValue& object, std::string_view key, Quat& out) {
  auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != JsonValue::Kind::Array || it->second.array.size() != 4) {
    return false;
  }
  if (std::any_of(it->second.array.begin(), it->second.array.end(), [](const JsonValue& value) {
        return value.kind != JsonValue::Kind::Number || !std::isfinite(value.number);
      })) {
    return false;
  }
  out.x = static_cast<float>(it->second.array[0].number);
  out.y = static_cast<float>(it->second.array[1].number);
  out.z = static_cast<float>(it->second.array[2].number);
  out.w = static_cast<float>(it->second.array[3].number);
  return true;
}

TransformComponent read_transform(const JsonValue& object) {
  TransformComponent transform;
  read_vec3(object, "translation", transform.translation);
  read_quat(object, "rotation", transform.rotation);
  read_vec3(object, "scale", transform.scale);
  return transform;
}

uint64_t monotonic_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
}

Mat4 identity_matrix() {
  Mat4 out{};
  out.values[0] = 1.0f;
  out.values[5] = 1.0f;
  out.values[10] = 1.0f;
  out.values[15] = 1.0f;
  return out;
}

Quat normalize_quat(Quat q) {
  const float len_sq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  if (len_sq <= 0.0f || !std::isfinite(len_sq)) {
    return {};
  }
  const float inv_len = 1.0f / std::sqrt(len_sq);
  q.x *= inv_len;
  q.y *= inv_len;
  q.z *= inv_len;
  q.w *= inv_len;
  return q;
}

Quat multiply_quat(const Quat& lhs, const Quat& rhs) {
  return normalize_quat({
      lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
      lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
      lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
      lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z});
}

Mat4 multiply_matrix(const Mat4& lhs, const Mat4& rhs) {
  Mat4 out{};
  for (std::size_t col = 0u; col < 4u; ++col) {
    for (std::size_t row = 0u; row < 4u; ++row) {
      float value = 0.0f;
      for (std::size_t k = 0u; k < 4u; ++k) {
        value += lhs.values[k * 4u + row] * rhs.values[col * 4u + k];
      }
      out.values[col * 4u + row] = value;
    }
  }
  return out;
}

Mat4 make_transform_matrix(const TransformComponent& transform) {
  const auto q = normalize_quat(transform.rotation);
  const float xx = q.x * q.x;
  const float yy = q.y * q.y;
  const float zz = q.z * q.z;
  const float xy = q.x * q.y;
  const float xz = q.x * q.z;
  const float yz = q.y * q.z;
  const float wx = q.w * q.x;
  const float wy = q.w * q.y;
  const float wz = q.w * q.z;

  Mat4 out = identity_matrix();
  out.values[0] = (1.0f - 2.0f * (yy + zz)) * transform.scale.x;
  out.values[1] = (2.0f * (xy + wz)) * transform.scale.x;
  out.values[2] = (2.0f * (xz - wy)) * transform.scale.x;

  out.values[4] = (2.0f * (xy - wz)) * transform.scale.y;
  out.values[5] = (1.0f - 2.0f * (xx + zz)) * transform.scale.y;
  out.values[6] = (2.0f * (yz + wx)) * transform.scale.y;

  out.values[8] = (2.0f * (xz + wy)) * transform.scale.z;
  out.values[9] = (2.0f * (yz - wx)) * transform.scale.z;
  out.values[10] = (1.0f - 2.0f * (xx + yy)) * transform.scale.z;

  out.values[12] = transform.translation.x;
  out.values[13] = transform.translation.y;
  out.values[14] = transform.translation.z;
  return out;
}

Vec3 matrix_translation(const Mat4& matrix) {
  return {matrix.values[12], matrix.values[13], matrix.values[14]};
}

bool finite_vec3(const Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite_quat(const Quat& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

bool valid_transform_values(const TransformComponent& transform) {
  return finite_vec3(transform.translation) && finite_vec3(transform.scale) && finite_quat(transform.rotation) &&
         transform.scale.x != 0.0f && transform.scale.y != 0.0f && transform.scale.z != 0.0f;
}

vkpt::core::Hash256 hash_scene_blob(std::string_view blob) {
  constexpr std::uint64_t kFNVOffset = 1469598103934665603ull;
  constexpr std::uint64_t kFNVPrime = 1099511628211ull;
  std::uint64_t hash = kFNVOffset;
  vkpt::core::Hash256 out{};
  for (unsigned char ch : blob) {
    hash ^= ch;
    hash *= kFNVPrime;
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::uint8_t>((hash >> ((i % 8) * 8)) & 0xffu);
  }
  return out;
}

std::uint64_t allocate_id(const std::unordered_set<vkpt::core::StableId>& used) {
  std::uint64_t candidate = 1;
  while (used.contains(candidate)) {
    ++candidate;
  }
  return candidate;
}

}  // namespace

std::string_view to_string(ComponentKind kind) {
  switch (kind) {
    case ComponentKind::Identity:
      return "Identity";
    case ComponentKind::Transform:
      return "Transform";
    case ComponentKind::Hierarchy:
      return "Hierarchy";
    case ComponentKind::Camera:
      return "Camera";
    case ComponentKind::Light:
      return "Light";
    case ComponentKind::MeshRenderer:
      return "MeshRenderer";
    case ComponentKind::SdfPrimitive:
      return "SDFPrimitive";
    case ComponentKind::MaterialOverride:
      return "MaterialOverride";
    case ComponentKind::PhysicsBody:
      return "PhysicsBody";
    case ComponentKind::Script:
      return "Script";
    case ComponentKind::Animation:
      return "Animation";
    case ComponentKind::BenchmarkTag:
      return "BenchmarkTag";
    default:
      return "Unknown";
  }
}

std::string_view to_string(TransformAuthority authority) {
  switch (authority) {
    case TransformAuthority::BenchmarkFrozen:
      return "BenchmarkFrozen";
    case TransformAuthority::PhysicsControlled:
      return "PhysicsControlled";
    case TransformAuthority::AnimationControlled:
      return "AnimationControlled";
    case TransformAuthority::ScriptControlled:
      return "ScriptControlled";
    case TransformAuthority::EditorControlled:
      return "EditorControlled";
    case TransformAuthority::Authored:
      return "Authored";
    default:
      return "Authored";
  }
}

std::string_view to_string(FrameStage stage) {
  switch (stage) {
    case FrameStage::FrameBegin:
      return "FrameBegin";
    case FrameStage::Input:
      return "Input";
    case FrameStage::CommandCollection:
      return "CommandCollection";
    case FrameStage::FixedUpdate:
      return "FixedUpdate";
    case FrameStage::VariableUpdate:
      return "VariableUpdate";
    case FrameStage::TransformAssembly:
      return "TransformAssembly";
    case FrameStage::SceneMutationApply:
      return "SceneMutationApply";
    case FrameStage::RenderPreparation:
      return "RenderPreparation";
    case FrameStage::RenderSubmit:
      return "RenderSubmit";
    case FrameStage::PresentOrExport:
      return "PresentOrExport";
    case FrameStage::FrameEnd:
      return "FrameEnd";
    default:
      return "FrameBegin";
  }
}

std::optional<JsonValue> JsonParser::parse(std::string_view text) {
  JsonParserImpl parser(text);
  return parser.parse();
}

std::string JsonParser::stringify(const JsonValue& value) {
  return vkpt::scene::stringify(value, false);
}

const FrameContext& FrameLifecycleController::context() const {
  return m_context;
}

const std::vector<FrameStageTiming>& FrameLifecycleController::timings() const {
  return m_timings;
}

void FrameLifecycleController::begin_frame(vkpt::core::FrameIndex frame, double delta_seconds, bool deterministic) {
  if (m_stageOpen) {
    end_stage(m_context.stage);
  }
  m_context.frame = frame;
  m_context.delta_seconds = delta_seconds;
  m_context.deterministic = deterministic;
  begin_stage(FrameStage::FrameBegin);
}

void FrameLifecycleController::begin_stage(FrameStage stage) {
  if (m_stageOpen) {
    end_stage(m_context.stage);
  }
  m_context.stage = stage;
  m_stageStartNs = monotonic_ns();
  m_stageOpen = true;
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Debug, "frame", "stage begin",
                                    {{"stage", std::string(to_string(stage))}},
                                    m_context.frame);
}

void FrameLifecycleController::end_stage(FrameStage stage) {
  if (!m_stageOpen) {
    return;
  }
  const auto end_ns = monotonic_ns();
  m_timings.push_back(FrameStageTiming{m_context.frame, stage, m_stageStartNs, end_ns});
  m_stageOpen = false;
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Debug, "frame", "stage end",
                                    {{"stage", std::string(to_string(stage))},
                                     {"duration_ns", std::to_string(end_ns >= m_stageStartNs ? end_ns - m_stageStartNs : 0u)}},
                                    m_context.frame);
}

void FrameLifecycleController::end_frame() {
  if (m_stageOpen) {
    end_stage(m_context.stage);
  }
  begin_stage(FrameStage::FrameEnd);
  end_stage(FrameStage::FrameEnd);
}

void FrameLifecycleController::clear_history() {
  m_timings.clear();
}

WorldSystemScheduler::WorldSystemScheduler(std::vector<WorldSystemPhase> phaseOrder) {
  if (!phaseOrder.empty()) {
    m_phaseOrder = std::move(phaseOrder);
    return;
  }
  m_phaseOrder = {WorldSystemPhase::PreFrame,
                  WorldSystemPhase::Input,
                  WorldSystemPhase::ScriptEarly,
                  WorldSystemPhase::AnimationSample,
                  WorldSystemPhase::PhysicsFixed,
                  WorldSystemPhase::TransformAssembly,
                  WorldSystemPhase::SceneCommandApply,
                  WorldSystemPhase::RenderExtract,
                  WorldSystemPhase::PostFrame};
}

bool WorldSystemScheduler::register_system(WorldSystemSpec spec) {
  if (spec.name.empty()) {
    return false;
  }
  m_systems.push_back(std::move(spec));
  return true;
}

std::vector<std::string> WorldSystemScheduler::validate() const {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < m_systems.size(); ++i) {
    for (std::size_t j = i + 1; j < m_systems.size(); ++j) {
      const auto& lhs = m_systems[i];
      const auto& rhs = m_systems[j];
      if (lhs.phase != rhs.phase) {
        continue;
      }
      const auto rw = lhs.readMask & rhs.writeMask;
      const auto ww = lhs.writeMask & rhs.writeMask;
      const auto wr = lhs.writeMask & rhs.readMask;
      if (rw == 0 && ww == 0 && wr == 0) {
        continue;
      }
      for (std::size_t b = 0; b < static_cast<std::size_t>(ComponentKind::Count); ++b) {
        const auto mask = 1u << b;
        const bool w = (lhs.writeMask & mask) != 0 || (rhs.writeMask & mask) != 0;
        const bool r = (lhs.readMask & mask) != 0 || (rhs.readMask & mask) != 0;
        if (w && (lhs.writeMask & mask) && (rhs.writeMask & mask)) {
          out.push_back("write-write conflict between " + lhs.name + " and " + rhs.name + " on " +
                        std::string(to_string(static_cast<ComponentKind>(b))));
        } else if (rw || wr || (w && r)) {
          if ((lhs.writeMask & mask) && (rhs.readMask & mask)) {
            out.push_back("write-read conflict between " + lhs.name + " and " + rhs.name + " on " +
                          std::string(to_string(static_cast<ComponentKind>(b))));
          } else if ((rhs.writeMask & mask) && (lhs.readMask & mask)) {
            out.push_back("read-write conflict between " + lhs.name + " and " + rhs.name + " on " +
                          std::string(to_string(static_cast<ComponentKind>(b))));
          }
        }
      }
    }
  }
  return out;
}

const std::vector<WorldSystemPhase>& WorldSystemScheduler::phase_order() const {
  return m_phaseOrder;
}

const std::vector<WorldSystemSpec>& WorldSystemScheduler::systems() const {
  return m_systems;
}

std::vector<WorldSystemConflict> WorldSystemScheduler::conflicts() const {
  std::vector<WorldSystemConflict> list;
  for (std::size_t i = 0; i < m_systems.size(); ++i) {
    for (std::size_t j = i + 1; j < m_systems.size(); ++j) {
      if (m_systems[i].phase != m_systems[j].phase) {
        continue;
      }
      const auto overlap = (m_systems[i].writeMask & m_systems[j].writeMask) |
                          (m_systems[i].writeMask & m_systems[j].readMask) |
                          (m_systems[i].readMask & m_systems[j].writeMask);
      if (overlap == 0) {
        continue;
      }
      for (std::size_t b = 0; b < static_cast<std::size_t>(ComponentKind::Count); ++b) {
        if (!(overlap & (1u << b))) {
          continue;
        }
        list.push_back({m_systems[i].name, m_systems[j].name, static_cast<ComponentKind>(b), m_systems[i].phase});
      }
    }
  }
  return list;
}

vkpt::core::StableId SceneWorld::create_entity(std::string_view name, vkpt::core::StableId stable_hint) {
  auto id = stable_hint == 0 ? m_nextStableId++ : stable_hint;
  if (m_entities.contains(id)) {
    if (stable_hint != 0) {
      return 0;
    }
    while (m_entities.contains(id)) {
      ++id;
    }
    m_nextStableId = id + 1;
  }
  EntityRecord record;
  record.stable_id = id;
  record.runtime_id = m_nextHandle++;
  record.alive = true;
  record.identity.stable_id = id;
  if (!name.empty()) {
    record.identity.name = std::string(name);
  }
  m_entities[id] = std::move(record);
  m_entities_order.push_back(id);
  if (id >= m_nextStableId) {
    m_nextStableId = id + 1;
  }
  return id;
}

bool SceneWorld::destroy_entity(vkpt::core::StableId id) {
  const auto it = m_entities.find(id);
  if (it == m_entities.end()) {
    return false;
  }
  if (!it->second.alive) {
    return false;
  }
  it->second.alive = false;
  m_entities_order.erase(std::remove(m_entities_order.begin(), m_entities_order.end(), id), m_entities_order.end());
  if (const auto childIt = m_children.find(id); childIt != m_children.end()) {
    for (const auto child : childIt->second) {
      if (auto* childRecord = get_entity(child)) {
        childRecord->hierarchy.reset();
        mark_dirty_recursive(child);
      }
    }
  }
  m_children.erase(id);
  for (auto& parentChildren : m_children) {
    parentChildren.second.erase(std::remove(parentChildren.second.begin(), parentChildren.second.end(), id),
                               parentChildren.second.end());
  }
  m_transformAuthority.erase(id);
  m_worldTransforms.erase(id);
  return true;
}

bool SceneWorld::entity_exists(vkpt::core::StableId id) const {
  const auto it = m_entities.find(id);
  return it != m_entities.end() && it->second.alive;
}

bool SceneWorld::set_identity(vkpt::core::StableId id, const IdentityComponent& component) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  record->identity = component;
  return true;
}

bool SceneWorld::set_transform(vkpt::core::StableId id,
                              const TransformComponent& transform,
                              TransformAuthority authority,
                              std::string_view writer,
                              vkpt::core::FrameIndex frame) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  const auto existing = m_transformAuthority.find(id);
  if (existing != m_transformAuthority.end() && existing->second.frame == frame) {
    const auto previous_rank = authority_rank(existing->second.authority);
    const auto next_rank = authority_rank(authority);
    const bool writer_conflict = existing->second.writer != writer;
    const bool lower_authority = next_rank < previous_rank;
    const bool tie_loses = next_rank == previous_rank && writer_conflict && std::string(writer) > existing->second.writer;
    if (writer_conflict || lower_authority) {
      const auto selected = (lower_authority || tie_loses) ? existing->second.authority : authority;
      m_authority_conflicts.push_back(
          {id, existing->second.writer, std::string(writer), selected, frame});
      vkpt::log::Logger::instance().log(vkpt::log::Severity::Warning, "scene", "transform authority conflict",
                                         {{"entity", std::to_string(id)},
                                          {"writer_a", existing->second.writer},
                                          {"writer_b", std::string(writer)},
                                          {"selected", std::string(to_string(selected))},
                                          {"frame", std::to_string(frame)}});
    }
    if (lower_authority || tie_loses) {
      return false;
    }
  }
  record->transform = transform;
  m_transformAuthority[id] = {authority, frame, std::string(writer)};
  mark_dirty_recursive(id);
  return true;
}

bool SceneWorld::assign_material(vkpt::core::StableId id, vkpt::core::StableId material_id) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  record->material_override = MaterialOverrideComponent{material_id};
  return true;
}

bool SceneWorld::assign_light(vkpt::core::StableId id, const LightComponent& light) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  record->light = light;
  return true;
}

bool SceneWorld::assign_camera(vkpt::core::StableId id, const CameraComponent& camera) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  record->camera = camera;
  return true;
}

bool SceneWorld::set_hierarchy_parent(vkpt::core::StableId child, vkpt::core::StableId parent) {
  auto* childRecord = get_entity(child);
  if (!childRecord) {
    return false;
  }
  if (parent != 0 && !entity_exists(parent)) {
    return false;
  }
  if (child == parent || is_ancestor(child, parent)) {
    return false;
  }
  const vkpt::core::StableId previous = childRecord->hierarchy ? childRecord->hierarchy->parent : 0;
  if (previous != 0) {
    auto prev = m_children.find(previous);
    if (prev != m_children.end()) {
      prev->second.erase(std::remove(prev->second.begin(), prev->second.end(), child), prev->second.end());
    }
  }
  if (parent == 0) {
    childRecord->hierarchy.reset();
  } else {
    childRecord->hierarchy = HierarchyComponent{parent};
    auto& list = m_children[parent];
    if (std::find(list.begin(), list.end(), child) == list.end()) {
      list.push_back(child);
      std::sort(list.begin(), list.end());
    }
  }
  mark_dirty_recursive(child);
  return true;
}

bool SceneWorld::set_component(vkpt::core::StableId id, ComponentKind kind, const ComponentVariant& component) {
  return add_component(id, kind, component);
}

bool SceneWorld::add_component(vkpt::core::StableId id, ComponentKind kind, const ComponentVariant& component) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  switch (kind) {
    case ComponentKind::Identity:
      if (const auto* value = std::get_if<IdentityComponent>(&component)) {
        return set_identity(id, *value);
      }
      return false;
    case ComponentKind::Transform:
      if (const auto* value = std::get_if<TransformComponent>(&component)) {
        record->transform = *value;
        mark_dirty_recursive(id);
        return true;
      }
      return false;
    case ComponentKind::Hierarchy:
      if (const auto* value = std::get_if<HierarchyComponent>(&component)) {
        return set_hierarchy_parent(id, value->parent);
      }
      return false;
    case ComponentKind::Camera:
      if (const auto* value = std::get_if<CameraComponent>(&component)) {
        record->camera = *value;
        return true;
      }
      return false;
    case ComponentKind::Light:
      if (const auto* value = std::get_if<LightComponent>(&component)) {
        record->light = *value;
        return true;
      }
      return false;
    case ComponentKind::MeshRenderer:
      if (const auto* value = std::get_if<MeshRendererComponent>(&component)) {
        record->mesh_renderer = *value;
        return true;
      }
      return false;
    case ComponentKind::SdfPrimitive:
      if (const auto* value = std::get_if<SdfPrimitiveComponent>(&component)) {
        record->sdf_primitive = *value;
        return true;
      }
      return false;
    case ComponentKind::MaterialOverride:
      if (const auto* value = std::get_if<MaterialOverrideComponent>(&component)) {
        record->material_override = *value;
        return true;
      }
      return false;
    case ComponentKind::PhysicsBody:
      if (const auto* value = std::get_if<PhysicsBodyComponent>(&component)) {
        record->physics_body = *value;
        return true;
      }
      return false;
    case ComponentKind::Script:
      if (const auto* value = std::get_if<ScriptComponent>(&component)) {
        record->script = *value;
        return true;
      }
      return false;
    case ComponentKind::Animation:
      if (const auto* value = std::get_if<AnimationComponent>(&component)) {
        record->animation = *value;
        return true;
      }
      return false;
    case ComponentKind::BenchmarkTag:
      if (const auto* value = std::get_if<BenchmarkTagComponent>(&component)) {
        record->benchmark_tag = *value;
        return true;
      }
      return false;
    default:
      return false;
  }
}

bool SceneWorld::remove_component(vkpt::core::StableId id, ComponentKind kind) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  switch (kind) {
    case ComponentKind::Identity:
      record->identity = IdentityComponent{};
      return true;
    case ComponentKind::Transform:
      record->transform.reset();
      m_worldTransforms.erase(id);
      mark_dirty_recursive(id);
      return true;
    case ComponentKind::Hierarchy:
      if (record->hierarchy && record->hierarchy->parent != 0) {
        auto parentIt = m_children.find(record->hierarchy->parent);
        if (parentIt != m_children.end()) {
          parentIt->second.erase(std::remove(parentIt->second.begin(), parentIt->second.end(), id),
                                 parentIt->second.end());
        }
      }
      record->hierarchy.reset();
      mark_dirty_recursive(id);
      return true;
    case ComponentKind::Camera:
      record->camera.reset();
      return true;
    case ComponentKind::Light:
      record->light.reset();
      return true;
    case ComponentKind::MeshRenderer:
      record->mesh_renderer.reset();
      return true;
    case ComponentKind::SdfPrimitive:
      record->sdf_primitive.reset();
      return true;
    case ComponentKind::MaterialOverride:
      record->material_override.reset();
      return true;
    case ComponentKind::PhysicsBody:
      record->physics_body.reset();
      return true;
    case ComponentKind::Script:
      record->script.reset();
      return true;
    case ComponentKind::Animation:
      record->animation.reset();
      return true;
    case ComponentKind::BenchmarkTag:
      record->benchmark_tag.reset();
      return true;
    default:
      return false;
  }
}

const SceneWorld::EntityRecord* SceneWorld::get_entity(vkpt::core::StableId id) const {
  const auto it = m_entities.find(id);
  if (it == m_entities.end() || !it->second.alive) {
    return nullptr;
  }
  return &it->second;
}

SceneWorld::EntityRecord* SceneWorld::get_entity(vkpt::core::StableId id) {
  auto it = m_entities.find(id);
  if (it == m_entities.end() || !it->second.alive) {
    return nullptr;
  }
  return &it->second;
}

const std::vector<vkpt::core::StableId>& SceneWorld::all_entities() const {
  return m_entities_order;
}

std::vector<vkpt::core::StableId> SceneWorld::query(ComponentKind kind) const {
  std::vector<vkpt::core::StableId> out;
  for (const auto id : m_entities_order) {
    const auto* entity = get_entity(id);
    if (!entity) {
      continue;
    }
    switch (kind) {
      case ComponentKind::Identity:
        out.push_back(id);
        break;
      case ComponentKind::Transform:
        if (entity->transform.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::Hierarchy:
        if (entity->hierarchy.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::Camera:
        if (entity->camera.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::Light:
        if (entity->light.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::MeshRenderer:
        if (entity->mesh_renderer.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::SdfPrimitive:
        if (entity->sdf_primitive.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::MaterialOverride:
        if (entity->material_override.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::PhysicsBody:
        if (entity->physics_body.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::Script:
        if (entity->script.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::Animation:
        if (entity->animation.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::BenchmarkTag:
        if (entity->benchmark_tag.has_value()) {
          out.push_back(id);
        }
        break;
      default:
        break;
    }
  }
  return out;
}

WorldTransform SceneWorld::compute_world_transform_unchecked(const EntityRecord* entity) const {
  WorldTransform out{};
  if (!entity || !entity->transform.has_value()) {
    out.world_matrix = identity_matrix();
    return out;
  }
  out = {};
  out.translation = entity->transform->translation;
  out.rotation = normalize_quat(entity->transform->rotation);
  out.scale = entity->transform->scale;
  out.world_matrix = make_transform_matrix(*entity->transform);
  out.dirty = false;
  auto get_parent_transform = [&](vkpt::core::StableId parent_id) -> WorldTransform {
    WorldTransform parent{};
    parent.world_matrix = identity_matrix();
    const auto it = m_worldTransforms.find(parent_id);
    if (it != m_worldTransforms.end()) {
      return it->second;
    }
    const auto parentIt = m_entities.find(parent_id);
    if (parentIt == m_entities.end()) {
      return parent;
    }
    return compute_world_transform_unchecked(&parentIt->second);
  };
  if (entity->hierarchy && entity->hierarchy->parent != 0) {
    const auto parentId = entity->hierarchy->parent;
    const auto parentTransform = get_parent_transform(parentId);
    const auto localMatrix = out.world_matrix;
    out.world_matrix = multiply_matrix(parentTransform.world_matrix, localMatrix);
    out.translation = matrix_translation(out.world_matrix);
    out.rotation = multiply_quat(parentTransform.rotation, out.rotation);
    out.scale = {parentTransform.scale.x * out.scale.x,
                 parentTransform.scale.y * out.scale.y,
                 parentTransform.scale.z * out.scale.z};
  }
  return out;
}

void SceneWorld::mark_dirty_recursive(vkpt::core::StableId id) {
  auto* entity = get_entity(id);
  if (!entity) {
    return;
  }
  if (entity->transform.has_value()) {
    entity->transform->dirty = true;
  }
  m_worldTransforms.erase(id);
  const auto it = m_children.find(id);
  if (it == m_children.end()) {
    return;
  }
  for (const auto child : it->second) {
    mark_dirty_recursive(child);
  }
}

bool SceneWorld::is_ancestor(vkpt::core::StableId ancestor, vkpt::core::StableId candidate) const {
  if (ancestor == 0 || candidate == 0) {
    return false;
  }
  auto it = m_entities.find(candidate);
  while (it != m_entities.end() && it->second.hierarchy.has_value()) {
    const auto parent = it->second.hierarchy->parent;
    if (parent == ancestor) {
      return true;
    }
    if (parent == 0) {
      break;
    }
    it = m_entities.find(parent);
  }
  return false;
}

void SceneWorld::recompute_world_transforms() {
  m_worldTransforms.clear();
  for (auto id : m_entities_order) {
    auto* entity = get_entity(id);
    if (!entity || !entity->transform.has_value()) {
      continue;
    }
    if (entity->transform->dirty || !m_worldTransforms.contains(id)) {
      m_worldTransforms[id] = compute_world_transform_unchecked(entity);
      m_entities[id].transform->dirty = false;
    }
  }
}

const WorldTransform* SceneWorld::world_transform(vkpt::core::StableId id) const {
  const auto it = m_worldTransforms.find(id);
  if (it == m_worldTransforms.end()) {
    return nullptr;
  }
  return &it->second;
}

const std::vector<WorldAuthorityConflict>& SceneWorld::authority_conflicts() const {
  return m_authority_conflicts;
}

bool SceneWorld::has_authority(vkpt::core::StableId id,
                               TransformAuthority authority,
                               std::string_view writer,
                               vkpt::core::FrameIndex frame) const {
  const auto existing = m_transformAuthority.find(id);
  if (existing == m_transformAuthority.end() || existing->second.frame != frame) {
    return true;
  }
  const auto previous_rank = authority_rank(existing->second.authority);
  const auto next_rank = authority_rank(authority);
  if (next_rank != previous_rank) {
    return next_rank > previous_rank;
  }
  return existing->second.writer == writer || std::string(writer) < existing->second.writer;
}

uint32_t SceneWorld::kind_mask(ComponentKind kind) const {
  return component_kind_mask(kind);
}

SceneSnapshot SceneWorld::build_snapshot() const {
  SceneSnapshot out;
  out.benchmark = {};
  out.asset_refs.reserve(m_entities_order.size());
  std::string blob = "scene:";
  for (const auto id : m_entities_order) {
    const auto* entity = get_entity(id);
    if (!entity) {
      continue;
    }
    out.entity_ids.push_back(id);
    blob += "e" + std::to_string(id) + ":" + entity->identity.name + ";";
    auto resolve_world = [&]() {
      WorldTransform world{};
      world.world_matrix = identity_matrix();
      if (const auto* wt = world_transform(id)) {
        world = *wt;
      } else {
        world = compute_world_transform_unchecked(entity);
      }
      return world;
    };
    if (entity->mesh_renderer.has_value()) {
      const auto world = resolve_world();
      out.renderables.push_back(SceneSnapshot::RenderableObject{
          id, entity->mesh_renderer->mesh_id, entity->mesh_renderer->material_id, world});
      blob += "r" + std::to_string(entity->mesh_renderer->mesh_id) + ":" +
              std::to_string(entity->mesh_renderer->material_id) + ";";
      blob += "t" + std::to_string(world.translation.x) + "," + std::to_string(world.translation.y) + "," +
              std::to_string(world.translation.z) + ";";
    }
    if (entity->light.has_value()) {
      const auto world = resolve_world();
      out.lights.push_back(
          SceneSnapshot::LightObject{id, *entity->light, world});
      blob += "l" + entity->light->type + ":" + std::to_string(entity->light->intensity) + ";";
    }
    if (entity->material_override.has_value() && std::none_of(out.materials.begin(), out.materials.end(),
        [&](const SceneSnapshot::MaterialObject& material) {
          return material.id == entity->material_override->material_id;
        })) {
      SceneSnapshot::MaterialObject material;
      material.id = entity->material_override->material_id;
      out.materials.push_back(material);
      blob += "mat" + std::to_string(material.id) + ";";
    }
  }
  if (!query(ComponentKind::Camera).empty()) {
    const auto first = query(ComponentKind::Camera).front();
    const auto* cameraEnt = get_entity(first);
    if (cameraEnt && cameraEnt->camera.has_value()) {
      out.camera = SceneCameraDefinition{first, *cameraEnt->camera};
      blob += "c" + std::to_string(first) + ":" + std::to_string(cameraEnt->camera->fov) + ";";
    }
  }
  out.scene_hash = hash_scene_blob(blob);
  return out;
}

RenderSceneProxy SceneWorld::extract_render_scene(vkpt::core::FrameIndex frame) const {
  RenderSceneProxy proxy;
  const auto snapshot = build_snapshot();
  proxy.scene_hash = snapshot.scene_hash;
  proxy.frame = frame;
  proxy.benchmark = snapshot.benchmark;
  proxy.renderables.reserve(snapshot.renderables.size());
  proxy.lights.reserve(snapshot.lights.size());
  proxy.materials.reserve(snapshot.materials.size());

  for (const auto& renderable : snapshot.renderables) {
    RenderSceneProxy::Renderable out;
    out.entity_id = renderable.entity_id;
    out.geometry_id = renderable.mesh_id;
    out.material_id = renderable.material_id;
    if (const auto* wt = world_transform(renderable.entity_id)) {
      out.world_matrix = wt->world_matrix;
      out.translation = wt->translation;
      out.scale = wt->scale;
    } else {
      out.world_matrix = make_transform_matrix(renderable.transform);
      out.translation = renderable.transform.translation;
      out.scale = renderable.transform.scale;
    }
    proxy.renderables.push_back(out);
  }

  for (const auto& light : snapshot.lights) {
    RenderSceneProxy::Light out;
    out.entity_id = light.entity_id;
    out.type = light.light.type;
    out.color = light.light.color;
    out.intensity = light.light.intensity;
    out.radius = light.light.radius;
    if (const auto* wt = world_transform(light.entity_id)) {
      out.world_matrix = wt->world_matrix;
      out.position = wt->translation;
    } else {
      out.world_matrix = make_transform_matrix(light.transform);
      out.position = light.transform.translation;
    }
    proxy.lights.push_back(out);
  }

  for (const auto& material : snapshot.materials) {
    proxy.materials.push_back(RenderSceneProxy::Material{
        material.id,
        material.material.albedo,
        material.material.roughness,
        material.material.emission,
        material.material.emission_intensity});
  }

  if (snapshot.camera) {
    RenderSceneProxy::Camera camera;
    camera.entity_id = snapshot.camera->id;
    camera.fov = snapshot.camera->camera.fov;
    camera.near_plane = snapshot.camera->camera.near_plane;
    camera.far_plane = snapshot.camera->camera.far_plane;
    if (const auto* wt = world_transform(camera.entity_id)) {
      camera.world_matrix = wt->world_matrix;
      camera.position = wt->translation;
    } else if (const auto* entity = get_entity(camera.entity_id); entity && entity->transform) {
      camera.world_matrix = make_transform_matrix(*entity->transform);
      camera.position = entity->transform->translation;
    } else {
      camera.world_matrix = identity_matrix();
    }
    proxy.camera = camera;
  }

  return proxy;
}

void SceneWorld::clear() {
  m_entities.clear();
  m_entities_order.clear();
  m_children.clear();
  m_transformAuthority.clear();
  m_worldTransforms.clear();
  m_authority_conflicts.clear();
  m_nextStableId = 1;
  m_nextHandle = 1;
}

void WorldCommandBuffer::add_create_entity(std::string_view name, vkpt::core::StableId stable_hint) {
  CreateEntityCommand cmd;
  cmd.name = std::string(name);
  cmd.requested_id = stable_hint;
  m_commands.push_back({CommandType::CreateEntity, cmd});
}

void WorldCommandBuffer::add_destroy_entity(vkpt::core::StableId id) {
  m_commands.push_back({CommandType::DestroyEntity, DestroyEntityCommand{id}});
}

void WorldCommandBuffer::add_set_component(vkpt::core::StableId id, ComponentKind kind, ComponentVariant component) {
  m_commands.push_back({CommandType::SetComponent, SetComponentCommand{id, kind, std::move(component)}});
}

void WorldCommandBuffer::add_add_component(vkpt::core::StableId id, ComponentKind kind, ComponentVariant component) {
  m_commands.push_back({CommandType::AddComponent, AddComponentCommand{id, kind, std::move(component)}});
}

void WorldCommandBuffer::add_remove_component(vkpt::core::StableId id, ComponentKind kind) {
  m_commands.push_back({CommandType::RemoveComponent, RemoveComponentCommand{id, kind}});
}

void WorldCommandBuffer::add_set_transform(vkpt::core::StableId id,
                                          TransformComponent transform,
                                          TransformAuthority authority,
                                          std::string_view writer,
                                          vkpt::core::FrameIndex frame) {
  SetTransformCommand cmd;
  cmd.id = id;
  cmd.transform = transform;
  cmd.authority = authority;
  cmd.writer = std::string(writer);
  cmd.frame = frame;
  m_commands.push_back({CommandType::SetTransform, cmd});
}

void WorldCommandBuffer::add_assign_material(vkpt::core::StableId id, vkpt::core::StableId material_id) {
  m_commands.push_back({CommandType::AssignMaterial, AssignMaterialCommand{id, material_id}});
}

void WorldCommandBuffer::add_assign_light(vkpt::core::StableId id, const LightComponent& light) {
  m_commands.push_back({CommandType::AssignLight, AssignLightCommand{id, light}});
}

void WorldCommandBuffer::add_assign_camera(vkpt::core::StableId id, const CameraComponent& camera) {
  m_commands.push_back({CommandType::AssignCamera, AssignCameraCommand{id, camera}});
}

vkpt::core::Result<void> WorldCommandBuffer::replay(SceneWorld& world) const {
  for (const auto& command : m_commands) {
    bool ok = std::visit([&](const auto& payload) -> bool {
      using T = std::decay_t<decltype(payload)>;
      if constexpr (std::is_same_v<T, CreateEntityCommand>) {
        return world.create_entity(payload.name, payload.requested_id) != 0;
      } else if constexpr (std::is_same_v<T, DestroyEntityCommand>) {
        return world.destroy_entity(payload.id);
      } else if constexpr (std::is_same_v<T, SetComponentCommand>) {
        return world.set_component(payload.id, payload.kind, payload.component);
      } else if constexpr (std::is_same_v<T, AddComponentCommand>) {
        return world.add_component(payload.id, payload.kind, payload.component);
      } else if constexpr (std::is_same_v<T, RemoveComponentCommand>) {
        return world.remove_component(payload.id, payload.kind);
      } else if constexpr (std::is_same_v<T, SetTransformCommand>) {
        return world.set_transform(payload.id, payload.transform, payload.authority, payload.writer, payload.frame);
      } else if constexpr (std::is_same_v<T, AssignMaterialCommand>) {
        return world.assign_material(payload.id, payload.material_id);
      } else if constexpr (std::is_same_v<T, AssignLightCommand>) {
        return world.assign_light(payload.id, payload.light);
      } else if constexpr (std::is_same_v<T, AssignCameraCommand>) {
        return world.assign_camera(payload.id, payload.camera);
      } else {
        return false;
      }
    }, command.payload);
    if (!ok) {
      return vkpt::core::Result<void>::error(vkpt::core::ErrorCode::Internal);
    }
  }
  return vkpt::core::Result<void>::ok();
}

void WorldCommandBuffer::clear() {
  m_commands.clear();
}

const std::vector<WorldCommandBuffer::Command>& WorldCommandBuffer::commands() const {
  return m_commands;
}

vkpt::core::Result<SceneDocument> SceneDocument::load_from_text(std::string_view text) {
  const auto root = JsonParser::parse(text);
  if (!root || root->kind != JsonValue::Kind::Object) {
    return vkpt::core::Result<SceneDocument>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  SceneDocument doc;
  std::unordered_set<std::uint64_t> usedIds;
  const auto& rootObj = *root;
  auto require_kind_if_present = [&](std::string_view key, JsonValue::Kind kind) {
    const auto it = rootObj.object.find(std::string(key));
    return it == rootObj.object.end() || it->second.kind == kind;
  };
  if (!require_kind_if_present("schema", JsonValue::Kind::String) ||
      !require_kind_if_present("metadata", JsonValue::Kind::Object) ||
      !require_kind_if_present("assets", JsonValue::Kind::Array) ||
      !require_kind_if_present("materials", JsonValue::Kind::Array) ||
      !require_kind_if_present("geometry", JsonValue::Kind::Array) ||
      !require_kind_if_present("sdf_primitives", JsonValue::Kind::Array) ||
      !require_kind_if_present("entities", JsonValue::Kind::Array) ||
      !require_kind_if_present("transforms", JsonValue::Kind::Array) ||
      !require_kind_if_present("cameras", JsonValue::Kind::Array) ||
      !require_kind_if_present("lights", JsonValue::Kind::Array) ||
      !require_kind_if_present("benchmark", JsonValue::Kind::Object)) {
    return vkpt::core::Result<SceneDocument>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  read_string(rootObj, "schema", doc.metadata.schema);

  if (const auto metadataNode = rootObj.object.find("metadata"); metadataNode != rootObj.object.end()) {
    read_string(metadataNode->second, "schema", doc.metadata.schema);
    read_string(metadataNode->second, "scene_name", doc.metadata.scene_name);
    read_string(metadataNode->second, "author", doc.metadata.author);
    read_string(metadataNode->second, "created", doc.metadata.created);
  }

  if (const auto assetsNode = rootObj.object.find("assets"); assetsNode != rootObj.object.end() &&
      assetsNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : assetsNode->second.array) {
      SceneAssetDefinition asset;
      read_u64(item, "id", asset.id);
      read_string(item, "type", asset.type);
      read_string(item, "uri", asset.uri);
      if (asset.id == 0) {
        asset.id = allocate_id(usedIds);
      }
      usedIds.insert(asset.id);
      doc.assets.push_back(std::move(asset));
    }
  }

  usedIds.clear();
  if (const auto materialsNode = rootObj.object.find("materials"); materialsNode != rootObj.object.end() &&
      materialsNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : materialsNode->second.array) {
      SceneMaterialDefinition material;
      read_u64(item, "id", material.id);
      read_string(item, "name", material.name);
      if (material.id == 0) {
        material.id = allocate_id(usedIds);
      }
      usedIds.insert(material.id);
      if (item.object.contains("albedo")) {
        read_vec3(item, "albedo", material.albedo);
      }
      if (item.object.contains("emission")) {
        read_vec3(item, "emission", material.emission);
      }
      if (item.object.contains("emission_intensity")) {
        read_float(item, "emission_intensity", material.emission_intensity);
      }
      read_float(item, "roughness", material.roughness);
      doc.materials.push_back(std::move(material));
    }
  }

  if (const auto geometryNode = rootObj.object.find("geometry"); geometryNode != rootObj.object.end() &&
      geometryNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : geometryNode->second.array) {
      SceneGeometryDefinition geometry;
      read_u64(item, "id", geometry.id);
      read_string(item, "primitive", geometry.primitive);
      read_u64(item, "material_id", geometry.material_id);
      read_vec3_list(item, "vertices", geometry.vertices);
      read_u32_list(item, "indices", geometry.indices);
      const auto tags = item.object.find("tags");
      if (tags != item.object.end() && tags->second.kind == JsonValue::Kind::Array) {
        for (const auto& t : tags->second.array) {
          if (t.kind == JsonValue::Kind::String) {
            geometry.tags.push_back(t.string);
          }
        }
      }
      doc.geometry.push_back(std::move(geometry));
    }
  }

  usedIds.clear();
  if (const auto sdfNode = rootObj.object.find("sdf_primitives"); sdfNode != rootObj.object.end() &&
      sdfNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : sdfNode->second.array) {
      SceneSdfPrimitiveDefinition primitive;
      read_u64(item, "id", primitive.id);
      read_string(item, "shape", primitive.shape);
      if (const auto transformNode = item.object.find("transform"); transformNode != item.object.end()) {
        primitive.transform = read_transform(transformNode->second);
      }
      if (const auto primitiveNode = item.object.find("primitive"); primitiveNode != item.object.end()) {
        read_string(primitiveNode->second, "shape", primitive.primitive.shape);
        read_float(primitiveNode->second, "radius", primitive.primitive.radius);
        read_float(primitiveNode->second, "param_a", primitive.primitive.param_a);
        read_float(primitiveNode->second, "param_b", primitive.primitive.param_b);
      }
      if (primitive.id == 0) {
        primitive.id = allocate_id(usedIds);
      }
      usedIds.insert(primitive.id);
      doc.sdf_primitives.push_back(std::move(primitive));
    }
  }

  usedIds.clear();
  if (const auto entitiesNode = rootObj.object.find("entities"); entitiesNode != rootObj.object.end() &&
      entitiesNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : entitiesNode->second.array) {
      SceneEntityDefinition entity;
      read_u64(item, "id", entity.id);
      if (entity.id == 0) {
        entity.id = allocate_id(usedIds);
      }
      usedIds.insert(entity.id);
      read_string(item, "name", entity.name);

      if (const auto transformNode = item.object.find("transform"); transformNode != item.object.end()) {
        entity.has_transform = true;
        entity.transform = read_transform(transformNode->second);
      }
      if (const auto cameraNode = item.object.find("camera"); cameraNode != item.object.end()) {
        entity.has_camera = true;
        read_float(cameraNode->second, "fov", entity.camera.fov);
        read_float(cameraNode->second, "near_plane", entity.camera.near_plane);
        read_float(cameraNode->second, "far_plane", entity.camera.far_plane);
      }
      if (const auto lightNode = item.object.find("light"); lightNode != item.object.end()) {
        entity.has_light = true;
        read_string(lightNode->second, "type", entity.light.type);
        read_vec3(lightNode->second, "color", entity.light.color);
        read_float(lightNode->second, "intensity", entity.light.intensity);
        read_float(lightNode->second, "radius", entity.light.radius);
      }
      if (const auto meshNode = item.object.find("mesh"); meshNode != item.object.end()) {
        entity.has_mesh = true;
        read_u64(meshNode->second, "mesh_id", entity.mesh.mesh_id);
        read_u64(meshNode->second, "material_id", entity.mesh.material_id);
      }
      if (const auto hierarchyNode = item.object.find("hierarchy"); hierarchyNode != item.object.end()) {
        read_u64(hierarchyNode->second, "parent", entity.hierarchy.parent);
      }
      if (const auto materialNode = item.object.find("material"); materialNode != item.object.end()) {
        read_u64(materialNode->second, "id", entity.material.material_id);
      }
      if (const auto animNode = item.object.find("animation"); animNode != item.object.end()) {
        read_string(animNode->second, "clip", entity.animation.clip);
        read_bool(animNode->second, "looping", entity.animation.looping);
      }
      if (const auto scriptNode = item.object.find("script"); scriptNode != item.object.end()) {
        read_string(scriptNode->second, "source", entity.script.script);
      }
      if (const auto benchmarkNode = item.object.find("benchmark"); benchmarkNode != item.object.end()) {
        read_bool(benchmarkNode->second, "enabled", entity.benchmark_tag.enabled);
      }
      doc.entities.push_back(std::move(entity));
    }
  }

  if (const auto transformsNode = rootObj.object.find("transforms"); transformsNode != rootObj.object.end() &&
      transformsNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : transformsNode->second.array) {
      SceneTransformEntry entry;
      read_u64(item, "id", entry.id);
      read_u64(item, "parent", entry.parent);
      if (const auto transform = item.object.find("transform"); transform != item.object.end()) {
        entry.transform = read_transform(transform->second);
      }
      doc.transforms.push_back(std::move(entry));
    }
  }

  if (const auto camerasNode = rootObj.object.find("cameras"); camerasNode != rootObj.object.end() &&
      camerasNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : camerasNode->second.array) {
      SceneCameraDefinition cam;
      read_u64(item, "id", cam.id);
      read_float(item, "fov", cam.camera.fov);
      read_float(item, "near_plane", cam.camera.near_plane);
      read_float(item, "far_plane", cam.camera.far_plane);
      doc.cameras.push_back(std::move(cam));
    }
  }

  if (const auto lightsNode = rootObj.object.find("lights"); lightsNode != rootObj.object.end() &&
      lightsNode->second.kind == JsonValue::Kind::Array) {
    for (const auto& item : lightsNode->second.array) {
      SceneLightDefinition light;
      read_u64(item, "id", light.id);
      read_string(item, "type", light.light.type);
      read_vec3(item, "color", light.light.color);
      read_float(item, "intensity", light.light.intensity);
      read_float(item, "radius", light.light.radius);
      doc.lights.push_back(std::move(light));
    }
  }

  if (const auto benchmarkNode = rootObj.object.find("benchmark"); benchmarkNode != rootObj.object.end()) {
    read_bool(benchmarkNode->second, "enabled", doc.benchmark.enabled);
    read_u32(benchmarkNode->second, "frame_target", doc.benchmark.frame_target);
    read_u32(benchmarkNode->second, "warmup_frames", doc.benchmark.warmup_frames);
  }

  if (!doc.validate(nullptr)) {
    doc.parse_result = SceneSchemaError::ValidationFailure;
    return vkpt::core::Result<SceneDocument>::error(vkpt::core::ErrorCode::InvalidArgument);
  }
  doc.parse_result = SceneSchemaError::Ok;
  return vkpt::core::Result<SceneDocument>::ok(std::move(doc));
}

vkpt::core::Result<SceneDocument> SceneDocument::load_from_file(std::string_view path) {
  std::ifstream file{std::string(path)};
  if (!file) {
    return vkpt::core::Result<SceneDocument>::error(vkpt::core::ErrorCode::IOError);
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return load_from_text(buffer.str());
}

bool SceneDocument::validate(std::vector<std::string>* issues) const {
  bool ok = true;
  auto report = [&](const std::string& message) {
    ok = false;
    if (issues) {
      issues->push_back(message);
    }
  };
  std::unordered_set<vkpt::core::StableId> entityIds;
  std::unordered_set<vkpt::core::StableId> materialIds;
  std::unordered_set<vkpt::core::StableId> geometryIds;
  std::unordered_set<vkpt::core::StableId> assetIds;
  std::unordered_set<vkpt::core::StableId> sdfIds;
  std::unordered_map<vkpt::core::StableId, vkpt::core::StableId> parentByEntity;

  if (metadata.schema.empty()) {
    report("metadata schema is empty");
  } else if (metadata.schema != "1.0") {
    report("unsupported schema " + metadata.schema);
  }

  for (const auto& asset : assets) {
    if (asset.id == 0) {
      report("asset id is zero");
    }
    if (!assetIds.insert(asset.id).second) {
      report("duplicate asset id " + std::to_string(asset.id));
    }
    if (asset.type.empty()) {
      report("asset type is empty for " + std::to_string(asset.id));
    }
    if (asset.uri.empty()) {
      report("asset uri is empty for " + std::to_string(asset.id));
    }
  }

  for (const auto& material : materials) {
    if (material.id == 0) {
      report("material id is zero");
    }
    if (!materialIds.insert(material.id).second) {
      report("duplicate material id " + std::to_string(material.id));
    }
    if (!finite_vec3(material.albedo) || !finite_vec3(material.emission)) {
      report("material contains non-finite color " + std::to_string(material.id));
    }
    if (!std::isfinite(material.roughness) || material.roughness < 0.0f || material.roughness > 1.0f) {
      report("material roughness out of range " + std::to_string(material.id));
    }
    if (!std::isfinite(material.emission_intensity) || material.emission_intensity < 0.0f) {
      report("material emission intensity out of range " + std::to_string(material.id));
    }
  }

  for (const auto& geometry_entry : geometry) {
    if (geometry_entry.id == 0) {
      report("geometry id is zero");
    }
    if (!geometryIds.insert(geometry_entry.id).second) {
      report("duplicate geometry id " + std::to_string(geometry_entry.id));
    }
    if (geometry_entry.primitive.empty()) {
      report("geometry primitive is empty " + std::to_string(geometry_entry.id));
    }
    if (geometry_entry.material_id != 0 && !materialIds.empty() && !materialIds.contains(geometry_entry.material_id)) {
      report("geometry references missing material " + std::to_string(geometry_entry.material_id));
    }
    if (geometry_entry.primitive == "triangle") {
      if (geometry_entry.vertices.empty()) {
        report("triangle geometry has no vertices " + std::to_string(geometry_entry.id));
      }
      if (geometry_entry.indices.empty() || geometry_entry.indices.size() % 3u != 0u) {
        report("triangle geometry indices are not triangles " + std::to_string(geometry_entry.id));
      }
      for (const auto index : geometry_entry.indices) {
        if (index >= geometry_entry.vertices.size()) {
          report("geometry index out of range " + std::to_string(geometry_entry.id));
          break;
        }
      }
    }
    for (const auto& vertex : geometry_entry.vertices) {
      if (!finite_vec3(vertex)) {
        report("geometry contains non-finite vertex " + std::to_string(geometry_entry.id));
        break;
      }
    }
  }

  for (const auto& sdf : sdf_primitives) {
    if (sdf.id == 0) {
      report("sdf primitive id is zero");
    }
    if (!sdfIds.insert(sdf.id).second) {
      report("duplicate sdf primitive id " + std::to_string(sdf.id));
    }
    if (sdf.shape.empty()) {
      report("sdf primitive shape is empty " + std::to_string(sdf.id));
    }
    if (!valid_transform_values(sdf.transform)) {
      report("sdf primitive has invalid transform " + std::to_string(sdf.id));
    }
    if (!std::isfinite(sdf.primitive.radius) || sdf.primitive.radius < 0.0f) {
      report("sdf primitive radius is invalid " + std::to_string(sdf.id));
    }
  }

  for (const auto& entity : entities) {
    if (entity.id == 0) {
      report("entity id is zero");
    }
    if (!entityIds.insert(entity.id).second) {
      report("duplicate entity id " + std::to_string(entity.id));
    }
    if (entity.hierarchy.parent != 0 && entity.id == entity.hierarchy.parent) {
      report("entity has self parent " + std::to_string(entity.id));
    }
    if (entity.hierarchy.parent != 0) {
      parentByEntity[entity.id] = entity.hierarchy.parent;
    }
    if (entity.has_transform && !valid_transform_values(entity.transform)) {
      report("entity has invalid transform " + std::to_string(entity.id));
    }
    if (entity.has_mesh) {
      if (!geometryIds.empty() && !geometryIds.contains(entity.mesh.mesh_id)) {
        report("entity references missing geometry " + std::to_string(entity.mesh.mesh_id));
      }
      if (entity.mesh.material_id != 0 && !materialIds.empty() && !materialIds.contains(entity.mesh.material_id)) {
        report("entity references missing material " + std::to_string(entity.mesh.material_id));
      }
    }
    if (entity.material.material_id != 0 && !materialIds.empty() && !materialIds.contains(entity.material.material_id)) {
      report("entity material override references missing material " + std::to_string(entity.material.material_id));
    }
    if (entity.has_camera &&
        (!std::isfinite(entity.camera.fov) || entity.camera.fov <= 0.0f ||
         entity.camera.near_plane <= 0.0f || entity.camera.far_plane <= entity.camera.near_plane)) {
      report("entity camera has invalid clip/fov " + std::to_string(entity.id));
    }
    if (entity.has_light &&
        (!finite_vec3(entity.light.color) || !std::isfinite(entity.light.intensity) || entity.light.intensity < 0.0f ||
         !std::isfinite(entity.light.radius) || entity.light.radius < 0.0f)) {
      report("entity light has invalid values " + std::to_string(entity.id));
    }
  }
  for (const auto& transform : transforms) {
    if (!entityIds.contains(transform.id)) {
      report("transform references missing entity " + std::to_string(transform.id));
    }
    if (transform.parent != 0) {
      if (!entityIds.contains(transform.parent)) {
        report("transform references missing parent " + std::to_string(transform.parent));
      }
      parentByEntity[transform.id] = transform.parent;
    }
    if (!valid_transform_values(transform.transform)) {
      report("transform has invalid values " + std::to_string(transform.id));
    }
  }
  for (const auto& light : lights) {
    if (!entityIds.contains(light.id)) {
      report("light references missing entity " + std::to_string(light.id));
    }
    if (!finite_vec3(light.light.color) || !std::isfinite(light.light.intensity) || light.light.intensity < 0.0f ||
        !std::isfinite(light.light.radius) || light.light.radius < 0.0f) {
      report("light has invalid values " + std::to_string(light.id));
    }
  }
  for (const auto& cam : cameras) {
    if (!entityIds.contains(cam.id)) {
      report("camera references missing entity " + std::to_string(cam.id));
    }
    if (!std::isfinite(cam.camera.fov) || cam.camera.fov <= 0.0f ||
        cam.camera.near_plane <= 0.0f || cam.camera.far_plane <= cam.camera.near_plane) {
      report("camera has invalid clip/fov " + std::to_string(cam.id));
    }
  }

  for (const auto& [child, parent] : parentByEntity) {
    if (parent != 0 && !entityIds.contains(parent)) {
      report("hierarchy references missing parent " + std::to_string(parent));
    }
    std::unordered_set<vkpt::core::StableId> visited;
    auto current = child;
    while (parentByEntity.contains(current)) {
      if (!visited.insert(current).second) {
        report("hierarchy cycle includes entity " + std::to_string(child));
        break;
      }
      current = parentByEntity[current];
      if (current == 0) {
        break;
      }
    }
  }

  if (benchmark.enabled && benchmark.frame_target != 0 && benchmark.warmup_frames > benchmark.frame_target) {
    report("benchmark warmup_frames exceeds frame_target");
  }
  return ok;
}

bool SceneDocument::has_section(std::string_view name) const {
  return name == "schema" || name == "metadata" || name == "assets" || name == "materials" ||
      name == "geometry" || name == "sdf_primitives" || name == "entities" ||
      name == "transforms" || name == "cameras" || name == "lights" || name == "benchmark";
}

vkpt::core::Result<SceneWorld> SceneDocument::to_world() const {
  SceneWorld world;
  for (const auto& entity : entities) {
    const auto id = world.create_entity(entity.name, entity.id);
    if (!id) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.has_transform && !world.set_component(id, ComponentKind::Transform, entity.transform)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.has_camera && !world.set_component(id, ComponentKind::Camera, entity.camera)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.has_light && !world.set_component(id, ComponentKind::Light, entity.light)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.has_mesh && !world.set_component(id, ComponentKind::MeshRenderer, entity.mesh)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.material.material_id != 0 && !world.set_component(id, ComponentKind::MaterialOverride, entity.material)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (entity.hierarchy.parent != 0 && !world.set_hierarchy_parent(id, entity.hierarchy.parent)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
    if (!entity.animation.clip.empty() && !world.set_component(id, ComponentKind::Animation, entity.animation)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
    if (!entity.script.script.empty() &&
        (!world.set_component(id, ComponentKind::Script, entity.script) ||
         !world.set_component(id, ComponentKind::BenchmarkTag, entity.benchmark_tag))) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::Internal);
    }
  }
  for (const auto& cam : cameras) {
    if (!world.set_component(cam.id, ComponentKind::Camera, cam.camera)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
  }
  for (const auto& light : lights) {
    if (!world.set_component(light.id, ComponentKind::Light, light.light)) {
      return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
  }
  for (const auto& transform : transforms) {
    if (transform.id != 0) {
      if (transform.parent != 0) {
        if (!world.set_hierarchy_parent(transform.id, transform.parent)) {
          return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::InvalidArgument);
        }
      }
      if (!world.set_transform(transform.id, transform.transform, TransformAuthority::Authored, "document", 0)) {
        return vkpt::core::Result<SceneWorld>::error(vkpt::core::ErrorCode::InvalidArgument);
      }
    }
  }
  world.recompute_world_transforms();
  return vkpt::core::Result<SceneWorld>::ok(std::move(world));
}

vkpt::core::Result<void> SceneDocument::apply_to_world(SceneWorld& world) const {
  auto loaded = to_world();
  if (!loaded) {
    return vkpt::core::Result<void>::error(loaded.error());
  }
  world = std::move(loaded.value());
  return vkpt::core::Result<void>::ok();
}

std::string SceneDocument::to_json(bool pretty) const {
  JsonValue root;
  root.kind = JsonValue::Kind::Object;
  root.object["schema"] = JsonValue{JsonValue::Kind::String, false, 0.0, "1.0", {}, {}};

  JsonValue metadataNode;
  metadataNode.kind = JsonValue::Kind::Object;
  metadataNode.object["schema"] = JsonValue{JsonValue::Kind::String, false, 0.0, metadata.schema, {}, {}};
  metadataNode.object["scene_name"] = JsonValue{JsonValue::Kind::String, false, 0.0, metadata.scene_name, {}, {}};
  metadataNode.object["author"] = JsonValue{JsonValue::Kind::String, false, 0.0, metadata.author, {}, {}};
  metadataNode.object["created"] = JsonValue{JsonValue::Kind::String, false, 0.0, metadata.created, {}, {}};
  root.object["metadata"] = metadataNode;

  root.object["assets"] = JsonValue{JsonValue::Kind::Array, false, 0.0, "", {}, {}};
  root.object["materials"] = JsonValue{JsonValue::Kind::Array, false, 0.0, "", {}, {}};
  root.object["geometry"] = JsonValue{JsonValue::Kind::Array, false, 0.0, "", {}, {}};
  root.object["sdf_primitives"] = JsonValue{JsonValue::Kind::Array, false, 0.0, "", {}, {}};
  root.object["entities"] = JsonValue{JsonValue::Kind::Array, false, 0.0, "", {}, {}};
  root.object["transforms"] = JsonValue{JsonValue::Kind::Array, false, 0.0, "", {}, {}};
  root.object["cameras"] = JsonValue{JsonValue::Kind::Array, false, 0.0, "", {}, {}};
  root.object["lights"] = JsonValue{JsonValue::Kind::Array, false, 0.0, "", {}, {}};

  JsonValue benchmarkNode;
  benchmarkNode.kind = JsonValue::Kind::Object;
  benchmarkNode.object["enabled"] = JsonValue{JsonValue::Kind::Boolean, benchmark.enabled, 0.0, "", {}, {}};
  benchmarkNode.object["frame_target"] = JsonValue{JsonValue::Kind::Number, false, static_cast<double>(benchmark.frame_target), "", {}, {}};
  benchmarkNode.object["warmup_frames"] = JsonValue{JsonValue::Kind::Number, false, static_cast<double>(benchmark.warmup_frames), "", {}, {}};
  root.object["benchmark"] = benchmarkNode;

  return stringify(root, pretty);
}

std::string SceneDocument::export_hash_hex() const {
  const auto hash = snapshot().scene_hash;
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(hash.size() * 2);
  for (const auto byte : hash) {
    out.push_back(kHex[(byte >> 4) & 0x0f]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

SceneSnapshot SceneDocument::snapshot() const {
  SceneSnapshot out;
  if (auto world = to_world()) {
    out = world.value().build_snapshot();
  }

  std::string blob = "scene:" + metadata.schema + ":" + metadata.scene_name + ";";
  out.entity_ids.clear();
  out.renderables.clear();
  out.lights.clear();
  for (const auto& entity : entities) {
    out.entity_ids.push_back(entity.id);
    blob += "e" + std::to_string(entity.id) + ":" + entity.name + ";";
    if (entity.has_mesh) {
      out.renderables.push_back({entity.id, entity.mesh.mesh_id, entity.mesh.material_id, entity.transform});
      blob += "m" + std::to_string(entity.mesh.mesh_id) + ":" + std::to_string(entity.mesh.material_id) + ";";
    }
    if (entity.has_light) {
      out.lights.push_back({entity.id, entity.light, entity.transform});
      blob += "l" + entity.light.type + ":" + std::to_string(entity.light.intensity) + ";";
    }
    if (entity.has_camera && !out.camera) {
      out.camera = SceneCameraDefinition{entity.id, entity.camera};
    }
  }
  out.materials.clear();
  for (const auto& material : materials) {
    out.materials.push_back({material.id, material});
    blob += "mat" + std::to_string(material.id) + ":" + material.name + ":" +
            std::to_string(material.roughness) + ";";
  }
  out.asset_refs.clear();
  for (const auto& asset : assets) {
    out.asset_refs.push_back(asset.uri);
    blob += "a" + std::to_string(asset.id) + ":" + asset.uri + ";";
  }
  for (const auto& camera : cameras) {
    if (!out.camera) {
      out.camera = camera;
    }
    blob += "c" + std::to_string(camera.id) + ":" + std::to_string(camera.camera.fov) + ";";
  }
  for (const auto& geometry_entry : geometry) {
    blob += "g" + std::to_string(geometry_entry.id) + ":" + geometry_entry.primitive + ":" +
            std::to_string(geometry_entry.material_id) + ":" +
            std::to_string(geometry_entry.vertices.size()) + ":" +
            std::to_string(geometry_entry.indices.size()) + ";";
  }
  for (const auto& transform : transforms) {
    blob += "x" + std::to_string(transform.id) + ":" + std::to_string(transform.parent) + ":" +
            std::to_string(transform.transform.translation.x) + "," +
            std::to_string(transform.transform.translation.y) + "," +
            std::to_string(transform.transform.translation.z) + ";";
  }
  for (const auto& light : lights) {
    blob += "L" + std::to_string(light.id) + ":" + light.light.type + ":" +
            std::to_string(light.light.intensity) + ";";
  }
  for (const auto& sdf : sdf_primitives) {
    blob += "sdf" + std::to_string(sdf.id) + ":" + sdf.shape + ":" +
            std::to_string(sdf.primitive.radius) + ";";
  }
  out.benchmark = benchmark;
  if (out.benchmark.enabled) {
    blob += "b";
    blob += std::to_string(out.benchmark.frame_target);
    blob += std::to_string(out.benchmark.warmup_frames);
  }
  out.scene_hash = hash_scene_blob(blob);
  return out;
}

RenderSceneProxy SceneDocument::extract_render_scene(vkpt::core::FrameIndex frame) const {
  RenderSceneProxy proxy;
  const auto snap = snapshot();
  proxy.scene_hash = snap.scene_hash;
  proxy.frame = frame;
  proxy.benchmark = benchmark;

  if (auto loaded = to_world()) {
    proxy = loaded.value().extract_render_scene(frame);
    proxy.scene_hash = snap.scene_hash;
    proxy.benchmark = benchmark;
  }

  proxy.materials.clear();
  proxy.materials.reserve(materials.size());
  for (const auto& material : materials) {
    proxy.materials.push_back(RenderSceneProxy::Material{
        material.id,
        material.albedo,
        material.roughness,
        material.emission,
        material.emission_intensity});
  }

  if (!proxy.camera && snap.camera) {
    RenderSceneProxy::Camera camera;
    camera.entity_id = snap.camera->id;
    camera.fov = snap.camera->camera.fov;
    camera.near_plane = snap.camera->camera.near_plane;
    camera.far_plane = snap.camera->camera.far_plane;
    camera.world_matrix = identity_matrix();
    proxy.camera = camera;
  }

  return proxy;
}

vkpt::core::Result<SceneDocument> JsonSceneLoader::load_document_from_text(std::string_view text) {
  return SceneDocument::load_from_text(text);
}

vkpt::core::Result<SceneDocument> JsonSceneLoader::load_document_from_file(std::string_view path) {
  return SceneDocument::load_from_file(path);
}

SceneRuntime::SceneRuntime(SceneWorld world) : m_world(std::move(world)) {}

IEcsWorld& SceneRuntime::world() {
  return m_world;
}

const IEcsWorld& SceneRuntime::world() const {
  return m_world;
}

SceneWorld& SceneRuntime::scene_world() {
  return m_world;
}

const SceneWorld& SceneRuntime::scene_world() const {
  return m_world;
}

vkpt::core::Result<void> SceneRuntime::load_document(const SceneDocument& document) {
  return document.apply_to_world(m_world);
}

SceneSnapshot SceneRuntime::snapshot() const {
  return m_world.build_snapshot();
}

RenderSceneProxy SceneRuntime::extract_render_scene(vkpt::core::FrameIndex frame) const {
  return m_world.extract_render_scene(frame);
}

FrameLifecycleController& SceneRuntime::frame_lifecycle() {
  return m_frameLifecycle;
}

const FrameLifecycleController& SceneRuntime::frame_lifecycle() const {
  return m_frameLifecycle;
}

}  // namespace vkpt::scene
