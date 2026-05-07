#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vkpt::assets {

enum class AssetClass : std::uint8_t {
  Mesh,
  Texture,
  Material,
  Scene,
  Shader,
  BenchmarkScene,
  Unknown,
};

struct AssetId {
  std::string urn;
  std::uint64_t hash = 0;

  [[nodiscard]] bool empty() const {
    return urn.empty() || hash == 0;
  }
};

inline bool operator==(const AssetId& lhs, const AssetId& rhs) {
  return lhs.hash == rhs.hash && lhs.urn == rhs.urn;
}

struct AssetIdHash {
  std::size_t operator()(const AssetId& id) const noexcept {
    return static_cast<std::size_t>(id.hash);
  }
};

struct AssetRecord {
  AssetId id;
  AssetClass asset_class = AssetClass::Unknown;
  std::string name;
  std::string source_uri;
  std::string source_hash;
  std::vector<AssetId> dependencies;
  std::vector<std::string> tags;
  std::unordered_map<std::string, std::string> metadata;
};

inline const char* ToString(AssetClass asset_class) {
  switch (asset_class) {
    case AssetClass::Mesh:
      return "mesh";
    case AssetClass::Texture:
      return "texture";
    case AssetClass::Material:
      return "material";
    case AssetClass::Scene:
      return "scene";
    case AssetClass::Shader:
      return "shader";
    case AssetClass::BenchmarkScene:
      return "benchmark_scene";
    case AssetClass::Unknown:
    default:
      return "unknown";
  }
}

inline AssetClass ParseAssetClass(std::string_view text) {
  if (text == "mesh") return AssetClass::Mesh;
  if (text == "texture") return AssetClass::Texture;
  if (text == "material") return AssetClass::Material;
  if (text == "scene") return AssetClass::Scene;
  if (text == "shader") return AssetClass::Shader;
  if (text == "benchmark_scene") return AssetClass::BenchmarkScene;
  return AssetClass::Unknown;
}

inline std::string EscapeJson(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
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
        out.push_back(c);
        break;
    }
  }
  return out;
}

inline std::uint64_t Fnv1a64Bytes(const void* data, std::size_t byte_count) {
  constexpr std::uint64_t kOffset = 1469598103934665603ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffset;
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < byte_count; ++i) {
    hash ^= static_cast<std::uint64_t>(bytes[i]);
    hash *= kPrime;
  }
  return hash;
}

inline std::uint64_t Fnv1a64(std::string_view text) {
  return Fnv1a64Bytes(text.data(), text.size());
}

inline std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << value;
  return out.str();
}

inline std::string HashTextHex(std::string_view text) {
  return Hex64(Fnv1a64(text));
}

inline AssetId MakeAssetId(AssetClass asset_class, std::string_view stable_key) {
  const std::string seed = std::string(ToString(asset_class)) + ":" + std::string(stable_key);
  const auto hash = Fnv1a64(seed);
  return AssetId{"urn:vkpt:asset:" + std::string(ToString(asset_class)) + ":" + Hex64(hash), hash};
}

inline std::string SerializeAssetRecord(const AssetRecord& record) {
  std::ostringstream out;
  out << "{";
  out << "\"id\":\"" << EscapeJson(record.id.urn) << "\",";
  out << "\"hash\":\"" << Hex64(record.id.hash) << "\",";
  out << "\"class\":\"" << ToString(record.asset_class) << "\",";
  out << "\"name\":\"" << EscapeJson(record.name) << "\",";
  out << "\"source_uri\":\"" << EscapeJson(record.source_uri) << "\",";
  out << "\"source_hash\":\"" << EscapeJson(record.source_hash) << "\",";
  out << "\"tags\":[";
  for (std::size_t i = 0; i < record.tags.size(); ++i) {
    if (i > 0) out << ",";
    out << "\"" << EscapeJson(record.tags[i]) << "\"";
  }
  out << "],\"dependencies\":[";
  for (std::size_t i = 0; i < record.dependencies.size(); ++i) {
    if (i > 0) out << ",";
    out << "\"" << EscapeJson(record.dependencies[i].urn) << "\"";
  }
  out << "],\"metadata\":{";
  std::vector<std::pair<std::string, std::string>> metadata(record.metadata.begin(), record.metadata.end());
  std::sort(metadata.begin(), metadata.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  for (std::size_t i = 0; i < metadata.size(); ++i) {
    if (i > 0) out << ",";
    out << "\"" << EscapeJson(metadata[i].first) << "\":\"" << EscapeJson(metadata[i].second) << "\"";
  }
  out << "}}";
  return out.str();
}

class AssetRegistry {
 public:
  [[nodiscard]] bool register_asset(AssetRecord record) {
    if (record.id.empty()) {
      record.id = MakeAssetId(record.asset_class, record.source_uri.empty() ? record.name : record.source_uri);
    }
    if (record.source_hash.empty() && !record.source_uri.empty()) {
      record.source_hash = HashTextHex(record.source_uri);
    }
    if (record.id.empty() || record.asset_class == AssetClass::Unknown) {
      return false;
    }
    const auto urn = record.id.urn;
    const auto existing = m_indexByUrn.find(urn);
    if (existing != m_indexByUrn.end()) {
      m_records[existing->second] = std::move(record);
      return true;
    }
    m_indexByUrn.emplace(urn, m_records.size());
    m_records.push_back(std::move(record));
    return true;
  }

  [[nodiscard]] const AssetRecord* find(const AssetId& id) const {
    return find_by_urn(id.urn);
  }

  [[nodiscard]] const AssetRecord* find_by_urn(std::string_view urn) const {
    const auto it = m_indexByUrn.find(std::string(urn));
    if (it == m_indexByUrn.end()) {
      return nullptr;
    }
    return &m_records[it->second];
  }

  [[nodiscard]] std::vector<const AssetRecord*> query(AssetClass asset_class) const {
    std::vector<const AssetRecord*> out;
    for (const auto& record : m_records) {
      if (record.asset_class == asset_class) {
        out.push_back(&record);
      }
    }
    return out;
  }

  [[nodiscard]] const std::vector<AssetRecord>& records() const {
    return m_records;
  }

  [[nodiscard]] bool empty() const {
    return m_records.empty();
  }

  void clear() {
    m_records.clear();
    m_indexByUrn.clear();
  }

  [[nodiscard]] std::string dump_manifest() const {
    std::vector<const AssetRecord*> ordered;
    ordered.reserve(m_records.size());
    for (const auto& record : m_records) {
      ordered.push_back(&record);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const AssetRecord* lhs, const AssetRecord* rhs) { return lhs->id.urn < rhs->id.urn; });

    std::ostringstream out;
    out << "{\"schema\":\"vkpt.asset_manifest.v1\",\"assets\":[";
    for (std::size_t i = 0; i < ordered.size(); ++i) {
      if (i > 0) out << ",";
      out << SerializeAssetRecord(*ordered[i]);
    }
    out << "]}";
    return out.str();
  }

 private:
  std::vector<AssetRecord> m_records;
  std::unordered_map<std::string, std::size_t> m_indexByUrn;
};

}  // namespace vkpt::assets
