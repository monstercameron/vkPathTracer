#include "scene/Json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>

namespace vkpt::scene {

namespace {

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

}  // namespace

std::optional<JsonValue> JsonParser::parse(std::string_view text) {
  JsonParserImpl parser(text);
  return parser.parse();
}

std::string JsonParser::stringify(const JsonValue& value) {
  return vkpt::scene::stringify(value, false);
}

std::string stringify(const JsonValue& value, bool pretty) {
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

bool read_vec2_list(const JsonValue& object, std::string_view key, std::vector<Vec2>& out) {
  out.clear();
  auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != JsonValue::Kind::Array) {
    return false;
  }
  out.reserve(it->second.array.size());
  for (const auto& element : it->second.array) {
    if (element.kind != JsonValue::Kind::Array || element.array.size() != 2) {
      return false;
    }
    if (std::any_of(element.array.begin(), element.array.end(), [](const JsonValue& value) {
          return value.kind != JsonValue::Kind::Number || !std::isfinite(value.number);
        })) {
      return false;
    }
    Vec2 value{};
    value.u = static_cast<float>(element.array[0].number);
    value.v = static_cast<float>(element.array[1].number);
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

}  // namespace vkpt::scene
