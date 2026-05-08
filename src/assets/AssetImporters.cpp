#include "assets/AssetImporters.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "core/Logging.h"
#include "core/health/Health.h"
#include "core/metrics/Metrics.h"

namespace vkpt::assets {

namespace {

struct AssetTelemetryState {
  std::atomic<std::uint64_t> load_started_total{0};
  std::atomic<std::uint64_t> load_completed_total{0};
  std::atomic<std::uint64_t> load_failed_total{0};
  std::atomic<std::uint64_t> cache_hit_total{0};
  std::atomic<std::uint64_t> cache_miss_total{0};
  std::atomic<std::uint64_t> load_us_count{0};
  std::atomic<std::uint64_t> load_us_sum{0};
  std::atomic<std::uint64_t> last_load_us{0};
  std::atomic<std::uint64_t> last_load_bytes{0};
  std::atomic<std::uint64_t> total_bytes_loaded{0};
  std::atomic<std::uint64_t> in_flight{0};
  std::atomic<std::uint64_t> last_tick_ns{0};
  std::atomic<std::uint64_t> current_flow_id{0};
  std::atomic<bool> health_probe_registered{false};
  std::mutex mutex;
  std::unordered_set<std::string> seen_sources;
  std::string last_asset_id;
  std::string last_kind;
  std::string last_error;
};

AssetTelemetryState& Telemetry() {
  static AssetTelemetryState state;
  return state;
}

double CacheHitRate(const AssetTelemetryState& telemetry) {
  const auto hits = telemetry.cache_hit_total.load(std::memory_order_relaxed);
  const auto misses = telemetry.cache_miss_total.load(std::memory_order_relaxed);
  const auto total = hits + misses;
  return total == 0u ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
}

void UpdateAssetTelemetryGauges(const AssetTelemetryState& telemetry) {
  auto& registry = vkpt::core::metrics::MetricsRegistry::instance();
  registry.gauge("vkp.assets.in_flight")
      .set(static_cast<double>(telemetry.in_flight.load(std::memory_order_relaxed)));
  registry.gauge("vkp.assets.cache_hit_rate").set(CacheHitRate(telemetry));
  registry.gauge("vkp.assets.total_bytes_loaded")
      .set(static_cast<double>(telemetry.total_bytes_loaded.load(std::memory_order_relaxed)));
}

std::uint64_t EstimateSourceBytes(const AssetImportSource& source) {
  if (!source.bytes.empty()) {
    return static_cast<std::uint64_t>(source.bytes.size());
  }
  if (!source.uri.empty()) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(source.uri, ec);
    if (!ec) {
      return static_cast<std::uint64_t>(size);
    }
  }
  return 0;
}

std::uint64_t SteadyNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string AssetKindFromSource(const AssetImportSource& source) {
  auto ext = detail::ExtensionOf(source.uri);
  if (!ext.empty() && ext.front() == '.') {
    ext.erase(ext.begin());
  }
  return ext.empty() ? "unknown" : ext;
}

bool RecordAssetLoadStarted(const AssetImportSource& source) {
  RegisterAssetHealthProbeForProcess();
  auto& telemetry = Telemetry();
  telemetry.load_started_total.fetch_add(1u, std::memory_order_relaxed);
  telemetry.in_flight.fetch_add(1u, std::memory_order_relaxed);
  telemetry.last_tick_ns.store(SteadyNowNs(), std::memory_order_relaxed);
  telemetry.current_flow_id.store(source.flow_id, std::memory_order_relaxed);
  bool cacheHit = false;
  {
    std::scoped_lock lock(telemetry.mutex);
    cacheHit = !source.uri.empty() && telemetry.seen_sources.contains(source.uri);
    if (!source.uri.empty()) {
      telemetry.seen_sources.insert(source.uri);
    }
    telemetry.last_kind = AssetKindFromSource(source);
    telemetry.last_error.clear();
  }
  if (cacheHit) {
    telemetry.cache_hit_total.fetch_add(1u, std::memory_order_relaxed);
    VKP_METRIC_INC("vkp.assets.cache_hit_total");
  } else {
    telemetry.cache_miss_total.fetch_add(1u, std::memory_order_relaxed);
    VKP_METRIC_INC("vkp.assets.cache_miss_total");
  }
  UpdateAssetTelemetryGauges(telemetry);
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Debug,
      "assets",
      "assets.load_started",
      {{"asset_id", source.uri.empty() ? "inline" : source.uri},
       {"kind", AssetKindFromSource(source)},
       {"cache_hit", cacheHit ? "true" : "false"},
       {"flow_id", std::to_string(source.flow_id)}});
  return cacheHit;
}

void RecordAssetLoadCompleted(const AssetImportSource& source,
                              const AssetImportResult& result,
                              std::uint64_t loadUs,
                              bool cacheHit) {
  auto& telemetry = Telemetry();
  telemetry.load_completed_total.fetch_add(1u, std::memory_order_relaxed);
  telemetry.load_us_count.fetch_add(1u, std::memory_order_relaxed);
  telemetry.load_us_sum.fetch_add(loadUs, std::memory_order_relaxed);
  telemetry.last_load_us.store(loadUs, std::memory_order_relaxed);
  telemetry.last_tick_ns.store(SteadyNowNs(), std::memory_order_relaxed);
  telemetry.current_flow_id.store(source.flow_id, std::memory_order_relaxed);
  VKP_METRIC_OBSERVE("vkp.assets.load_us", loadUs);
  const auto bytes = EstimateSourceBytes(source);
  telemetry.last_load_bytes.store(bytes, std::memory_order_relaxed);
  telemetry.total_bytes_loaded.fetch_add(bytes, std::memory_order_relaxed);
  telemetry.in_flight.fetch_sub(1u, std::memory_order_relaxed);
  UpdateAssetTelemetryGauges(telemetry);
  const std::string assetId =
      !result.assets.empty() ? result.assets.front().id.urn
                             : (source.uri.empty() ? "inline" : source.uri);
  {
    std::scoped_lock lock(telemetry.mutex);
    telemetry.last_asset_id = assetId;
    telemetry.last_kind = AssetKindFromSource(source);
    telemetry.last_error.clear();
  }
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Debug,
      "assets",
      "assets.load_completed",
      {{"asset_id", assetId},
       {"kind", AssetKindFromSource(source)},
       {"bytes", std::to_string(bytes)},
       {"load_us", std::to_string(loadUs)},
       {"cache_hit", cacheHit ? "true" : "false"},
       {"flow_id", std::to_string(source.flow_id)}});
}

void RecordAssetLoadFailed(const AssetImportSource& source,
                           std::string_view reason,
                           std::uint64_t loadUs) {
  auto& telemetry = Telemetry();
  telemetry.load_failed_total.fetch_add(1u, std::memory_order_relaxed);
  telemetry.last_load_us.store(loadUs, std::memory_order_relaxed);
  telemetry.last_tick_ns.store(SteadyNowNs(), std::memory_order_relaxed);
  telemetry.current_flow_id.store(source.flow_id, std::memory_order_relaxed);
  VKP_METRIC_OBSERVE("vkp.assets.load_us", loadUs);
  telemetry.in_flight.fetch_sub(1u, std::memory_order_relaxed);
  UpdateAssetTelemetryGauges(telemetry);
  {
    std::scoped_lock lock(telemetry.mutex);
    telemetry.last_asset_id = source.uri.empty() ? "inline" : source.uri;
    telemetry.last_kind = AssetKindFromSource(source);
    telemetry.last_error = std::string(reason);
  }
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Error,
      "assets",
      "assets.load_failed",
      {{"asset_id", source.uri.empty() ? "inline" : source.uri},
       {"path_or_urn", source.uri},
       {"reason", std::string(reason)},
       {"requesting_asset_id", source.binding_context.empty() ? "direct" : source.binding_context},
       {"flow_id", std::to_string(source.flow_id)}});
}

}  // namespace

namespace detail {

std::string ToLower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string Trim(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::string ExtensionOf(std::string_view uri) {
  const auto slash = uri.find_last_of("/\\");
  const auto dot = uri.find_last_of('.');
  const auto segment_start = slash == std::string_view::npos ? 0u : slash + 1u;
  if (dot == std::string_view::npos ||
      (slash != std::string_view::npos && dot < slash) ||
      dot == segment_start ||
      dot + 1u >= uri.size()) {
    return {};
  }
  return ToLower(uri.substr(dot));
}

bool HasExtension(std::string_view uri, const std::vector<std::string_view>& extensions) {
  const auto ext = ExtensionOf(uri);
  for (const auto candidate : extensions) {
    if (ext == candidate) {
      return true;
    }
  }
  return false;
}

std::uint8_t Byte(const std::vector<std::byte>& bytes, std::size_t index) {
  return std::to_integer<std::uint8_t>(bytes[index]);
}

std::uint32_t ReadU32Le(const std::vector<std::byte>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(Byte(bytes, offset)) |
         (static_cast<std::uint32_t>(Byte(bytes, offset + 1)) << 8u) |
         (static_cast<std::uint32_t>(Byte(bytes, offset + 2)) << 16u) |
         (static_cast<std::uint32_t>(Byte(bytes, offset + 3)) << 24u);
}

std::uint32_t ReadU32Be(const std::vector<std::byte>& bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(Byte(bytes, offset)) << 24u) |
         (static_cast<std::uint32_t>(Byte(bytes, offset + 1)) << 16u) |
         (static_cast<std::uint32_t>(Byte(bytes, offset + 2)) << 8u) |
         static_cast<std::uint32_t>(Byte(bytes, offset + 3));
}

std::uint16_t ReadU16Be(const std::vector<std::byte>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(Byte(bytes, offset)) << 8u) |
                                    static_cast<std::uint16_t>(Byte(bytes, offset + 1)));
}

std::vector<std::byte> ReadFileBytes(std::string_view uri) {
  std::ifstream file(std::filesystem::path(std::string(uri)), std::ios::binary);
  if (!file) {
    return {};
  }
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  if (size <= 0 || size > std::streampos{std::numeric_limits<std::streamsize>::max()}) {
    return {};
  }
  file.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
    return {};
  }
  return bytes;
}

std::vector<std::byte> ResolveSourceBytes(const AssetImportSource& source) {
  // Keep import hashes deterministic for callers that already staged bytes.
  if (!source.bytes.empty()) {
    return source.bytes;
  }
  if (!source.uri.empty()) {
    return ReadFileBytes(source.uri);
  }
  return {};
}

std::string BytesToString(const std::vector<std::byte>& bytes) {
  std::string text;
  text.resize(bytes.size());
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    text[i] = static_cast<char>(Byte(bytes, i));
  }
  return text;
}

std::string HashBytesHex(const std::vector<std::byte>& bytes) {
  if (bytes.empty()) {
    return HashTextHex("");
  }
  return Hex64(Fnv1a64Bytes(bytes.data(), bytes.size()));
}

AssetImportDiagnostic Diagnostic(ImportDiagnosticSeverity severity,
                                 std::string code,
                                 std::string message,
                                 bool lossy) {
  return {severity, std::move(code), std::move(message), lossy};
}

std::vector<std::string> SplitLines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto end = text.find_first_of("\r\n", start);
    if (end == std::string_view::npos) {
      lines.emplace_back(text.substr(start));
      break;
    }
    lines.emplace_back(text.substr(start, end - start));
    start = end + 1;
    if (end + 1 < text.size() && text[end] == '\r' && text[end + 1] == '\n') {
      start = end + 2;
    }
  }
  return lines;
}

std::vector<std::string> SplitWords(std::string_view text) {
  std::vector<std::string> words;
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
      ++cursor;
    }
    const auto start = cursor;
    while (cursor < text.size() && !std::isspace(static_cast<unsigned char>(text[cursor]))) {
      ++cursor;
    }
    if (cursor > start) {
      words.emplace_back(text.substr(start, cursor - start));
    }
  }
  return words;
}

std::optional<std::string_view> ExtractNamedArray(std::string_view json, std::string_view name) {
  const std::string key = "\"" + std::string(name) + "\"";
  const auto key_pos = json.find(key);
  if (key_pos == std::string_view::npos) {
    return {};
  }
  const auto colon = json.find(':', key_pos + key.size());
  if (colon == std::string_view::npos) {
    return {};
  }
  const auto begin = json.find('[', colon + 1);
  if (begin == std::string_view::npos) {
    return {};
  }
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = begin; i < json.size(); ++i) {
    const char c = json[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
      continue;
    }
    if (c == '[') {
      ++depth;
    } else if (c == ']') {
      --depth;
      if (depth == 0) {
        return json.substr(begin, i - begin + 1);
      }
    }
  }
  return {};
}

std::vector<std::string_view> TopLevelObjects(std::string_view array_text) {
  std::vector<std::string_view> out;
  std::size_t object_begin = std::string_view::npos;
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = 0; i < array_text.size(); ++i) {
    const char c = array_text[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
      continue;
    }
    if (c == '{') {
      if (depth == 0) {
        object_begin = i;
      }
      ++depth;
    } else if (c == '}') {
      if (depth > 0) {
        --depth;
      }
      if (depth == 0 && object_begin != std::string_view::npos) {
        out.push_back(array_text.substr(object_begin, i - object_begin + 1));
        object_begin = std::string_view::npos;
      }
    }
  }
  return out;
}

std::optional<std::string> ExtractStringValue(std::string_view object_text, std::string_view name) {
  const std::string key = "\"" + std::string(name) + "\"";
  const auto key_pos = object_text.find(key);
  if (key_pos == std::string_view::npos) {
    return {};
  }
  const auto colon = object_text.find(':', key_pos + key.size());
  if (colon == std::string_view::npos) {
    return {};
  }
  const auto quote = object_text.find('"', colon + 1);
  if (quote == std::string_view::npos) {
    return {};
  }
  std::string out;
  bool escaped = false;
  for (std::size_t i = quote + 1; i < object_text.size(); ++i) {
    const char c = object_text[i];
    if (escaped) {
      out.push_back(c);
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      return out;
    }
    out.push_back(c);
  }
  return {};
}

std::vector<std::string> ExtractObjectNames(std::string_view json, std::string_view array_name) {
  std::vector<std::string> names;
  const auto array = ExtractNamedArray(json, array_name);
  if (!array) {
    return names;
  }
  const auto objects = TopLevelObjects(*array);
  names.reserve(objects.size());
  for (const auto object : objects) {
    if (auto name = ExtractStringValue(object, "name")) {
      names.push_back(*name);
    } else {
      names.emplace_back();
    }
  }
  return names;
}

std::vector<std::string> ExtractImageUris(std::string_view json) {
  std::vector<std::string> uris;
  const auto array = ExtractNamedArray(json, "images");
  if (!array) {
    return uris;
  }
  const auto objects = TopLevelObjects(*array);
  uris.reserve(objects.size());
  for (const auto object : objects) {
    if (auto uri = ExtractStringValue(object, "uri")) {
      uris.push_back(*uri);
    } else {
      uris.emplace_back();
    }
  }
  return uris;
}

TextureChannelSemantic InferTextureSemantic(std::string_view uri, std::string_view context) {
  const auto key = ToLower(std::string(uri) + " " + std::string(context));
  if (key.find("normal") != std::string::npos || key.find("_nrm") != std::string::npos) {
    return TextureChannelSemantic::Normal;
  }
  if (key.find("rough") != std::string::npos) {
    return TextureChannelSemantic::Roughness;
  }
  if (key.find("metal") != std::string::npos) {
    return TextureChannelSemantic::Metallic;
  }
  if (key.find("occlusion") != std::string::npos || key.find("_ao") != std::string::npos) {
    return TextureChannelSemantic::Occlusion;
  }
  if (key.find("emissive") != std::string::npos || key.find("emission") != std::string::npos) {
    return TextureChannelSemantic::Emissive;
  }
  if (key.find("alpha") != std::string::npos || key.find("opacity") != std::string::npos) {
    return TextureChannelSemantic::Alpha;
  }
  if (key.find("height") != std::string::npos || key.find("displace") != std::string::npos) {
    return TextureChannelSemantic::Height;
  }
  return TextureChannelSemantic::BaseColor;
}

std::string FileNameOrUri(std::string_view uri) {
  const auto path = std::filesystem::path(std::string(uri));
  const auto name = path.filename().string();
  return name.empty() ? std::string(uri) : name;
}

}  // namespace detail

bool ImporterRegistry::register_importer(std::shared_ptr<IAssetImporter> importer) {
  if (!importer || importer->importer_id().empty()) {
    return false;
  }
  for (const auto& existing : m_importers) {
    if (existing->importer_id() == importer->importer_id()) {
      return false;
    }
  }
  m_importers.push_back(std::move(importer));
  return true;
}

const IAssetImporter* ImporterRegistry::importer_for_extension(std::string_view extension) const {
  const auto normalized = detail::ToLower(extension);
  for (const auto& importer : m_importers) {
    for (const auto ext : importer->supported_extensions()) {
      if (normalized == ext) {
        return importer.get();
      }
    }
  }
  return nullptr;
}

const IAssetImporter* ImporterRegistry::importer_for_source(const AssetImportSource& source) const {
  return importer_for_extension(detail::ExtensionOf(source.uri));
}

AssetValidationResult ImporterRegistry::validate_source(const AssetImportSource& source,
                                                        const AssetImportOptions& options) const {
  const auto* importer = importer_for_source(source);
  if (!importer) {
    return {false, "no importer registered for extension " + detail::ExtensionOf(source.uri), {}};
  }
  return importer->validate_source(source, options);
}

AssetImportResult ImporterRegistry::import_source(const AssetImportSource& source,
                                                  const AssetImportOptions& options) const {
  const auto start = std::chrono::steady_clock::now();
  const bool cacheHit = RecordAssetLoadStarted(source);
  auto elapsedUs = [&]() -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
  };
  const auto* importer = importer_for_source(source);
  if (!importer) {
    AssetImportResult result;
    result.success = false;
    result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Error,
                                                    "asset.no_importer",
                                                    "no importer registered for extension " +
                                                        detail::ExtensionOf(source.uri)));
    result.status = vkpt::core::Status::error(vkpt::core::StatusCode::Unsupported,
                                              result.diagnostics.back().message,
                                              "register an importer for this extension or convert the asset");
    RecordAssetLoadFailed(source,
                          result.diagnostics.back().message,
                          elapsedUs());
    return result;
  }
  auto result = importer->import_source(source, options);
  if (result.success) {
    if (result.status.is_error()) {
      result.status = vkpt::core::Status::ok("asset import completed");
    }
    RecordAssetLoadCompleted(source, result, elapsedUs(), cacheHit);
  } else {
    const std::string reason = result.diagnostics.empty()
        ? "importer returned failure"
        : result.diagnostics.back().message;
    if (result.status.is_ok()) {
      result.status = vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument, reason);
    }
    RecordAssetLoadFailed(source, reason, elapsedUs());
  }
  return result;
}

std::vector<std::string_view> ImporterRegistry::supported_extensions() const {
  std::vector<std::string_view> out;
  for (const auto& importer : m_importers) {
    const auto extensions = importer->supported_extensions();
    out.insert(out.end(), extensions.begin(), extensions.end());
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

const std::vector<std::shared_ptr<IAssetImporter>>& ImporterRegistry::importers() const {
  return m_importers;
}

AssetTelemetryStatus GetAssetTelemetryStatus() {
  auto& telemetry = Telemetry();
  AssetTelemetryStatus out;
  out.load_started_total = telemetry.load_started_total.load(std::memory_order_relaxed);
  out.load_completed_total = telemetry.load_completed_total.load(std::memory_order_relaxed);
  out.load_failed_total = telemetry.load_failed_total.load(std::memory_order_relaxed);
  out.cache_hit_total = telemetry.cache_hit_total.load(std::memory_order_relaxed);
  out.cache_miss_total = telemetry.cache_miss_total.load(std::memory_order_relaxed);
  out.load_us_count = telemetry.load_us_count.load(std::memory_order_relaxed);
  out.load_us_sum = telemetry.load_us_sum.load(std::memory_order_relaxed);
  out.last_load_us = telemetry.last_load_us.load(std::memory_order_relaxed);
  out.last_load_bytes = telemetry.last_load_bytes.load(std::memory_order_relaxed);
  out.total_bytes_loaded = telemetry.total_bytes_loaded.load(std::memory_order_relaxed);
  out.in_flight = telemetry.in_flight.load(std::memory_order_relaxed);
  out.last_tick_ns = telemetry.last_tick_ns.load(std::memory_order_relaxed);
  out.ticks_total = out.load_started_total;
  out.errors_total = out.load_failed_total;
  out.current_flow_id = telemetry.current_flow_id.load(std::memory_order_relaxed);
  if (out.in_flight > 0u) {
    out.lifecycle = vkpt::core::contracts::ComponentLifecycle::Busy;
  } else if (out.load_started_total == 0u) {
    out.lifecycle = vkpt::core::contracts::ComponentLifecycle::Uninitialized;
  } else if (out.load_failed_total > 0u) {
    out.lifecycle = vkpt::core::contracts::ComponentLifecycle::Degraded;
  } else {
    out.lifecycle = vkpt::core::contracts::ComponentLifecycle::Ready;
  }
  const auto hits = out.cache_hit_total;
  const auto misses = out.cache_miss_total;
  const auto totalCacheLookups = hits + misses;
  out.cache_hit_rate = totalCacheLookups == 0u
      ? 0.0
      : static_cast<double>(hits) / static_cast<double>(totalCacheLookups);
  {
    std::scoped_lock lock(telemetry.mutex);
    out.last_asset_id = telemetry.last_asset_id;
    out.last_kind = telemetry.last_kind;
    out.last_error = telemetry.last_error;
    out.last_failure = telemetry.last_error;
  }
  return out;
}

AssetsStatus GetAssetsStatus() {
  AssetsStatus out;
  static_cast<AssetTelemetryStatus&>(out) = GetAssetTelemetryStatus();
  return out;
}

void RegisterAssetHealthProbeForProcess() {
  auto& telemetry = Telemetry();
  bool expected = false;
  if (!telemetry.health_probe_registered.compare_exchange_strong(expected,
                                                                  true,
                                                                  std::memory_order_acq_rel)) {
    return;
  }
  VKP_LIFECYCLE_STARTED("assets");
  vkpt::core::health::HealthRegistry::instance().register_probe(
      std::make_shared<vkpt::core::health::FunctionProbe>(
          "assets",
          [] {
            const auto status = GetAssetsStatus();
            if (status.in_flight > 0u) {
              return vkpt::core::health::Report{
                  vkpt::core::health::Status::Degraded,
                  "asset imports in flight: " + std::to_string(status.in_flight)};
            }
            if (status.errors_total > 0u) {
              return vkpt::core::health::Report{
                  vkpt::core::health::Status::Degraded,
                  status.last_failure.empty() ? "asset import failures observed" : status.last_failure};
            }
            return vkpt::core::health::Report{vkpt::core::health::Status::Ok, "assets ready"};
          }));
}

void ResetAssetTelemetryForTest() {
  auto& telemetry = Telemetry();
  telemetry.load_started_total.store(0u, std::memory_order_relaxed);
  telemetry.load_completed_total.store(0u, std::memory_order_relaxed);
  telemetry.load_failed_total.store(0u, std::memory_order_relaxed);
  telemetry.cache_hit_total.store(0u, std::memory_order_relaxed);
  telemetry.cache_miss_total.store(0u, std::memory_order_relaxed);
  telemetry.load_us_count.store(0u, std::memory_order_relaxed);
  telemetry.load_us_sum.store(0u, std::memory_order_relaxed);
  telemetry.last_load_us.store(0u, std::memory_order_relaxed);
  telemetry.last_load_bytes.store(0u, std::memory_order_relaxed);
  telemetry.total_bytes_loaded.store(0u, std::memory_order_relaxed);
  telemetry.in_flight.store(0u, std::memory_order_relaxed);
  telemetry.last_tick_ns.store(0u, std::memory_order_relaxed);
  telemetry.current_flow_id.store(0u, std::memory_order_relaxed);
  vkpt::core::metrics::MetricsRegistry::instance().reset("vkp.assets.");
  std::scoped_lock lock(telemetry.mutex);
  telemetry.seen_sources.clear();
  telemetry.last_asset_id.clear();
  telemetry.last_kind.clear();
  telemetry.last_error.clear();
}

std::string_view FakeAssetImporter::importer_id() const {
  return "fake";
}

std::vector<std::string_view> FakeAssetImporter::supported_extensions() const {
  return {".fake"};
}

std::vector<std::string_view> FakeAssetImporter::supported_features() const {
  return {"unit_test", "metadata_only"};
}

AssetValidationResult FakeAssetImporter::validate_source(const AssetImportSource& source,
                                                         const AssetImportOptions&) const {
  if (!detail::HasExtension(source.uri, supported_extensions())) {
    return {false, "unsupported extension", {}};
  }
  if (source.uri.empty() && source.bytes.empty()) {
    return {false, "fake importer needs a uri or bytes", {}};
  }
  return {true, "", {}};
}

AssetImportResult FakeAssetImporter::import_source(const AssetImportSource& source,
                                                   const AssetImportOptions& options) const {
  AssetImportResult result;
  auto validation = validate_source(source, options);
  if (!validation.valid) {
    result.diagnostics = std::move(validation.diagnostics);
    result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Error,
                                                    "fake.invalid",
                                                    validation.reason));
    result.status = vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument,
                                              validation.reason);
    return result;
  }
  const auto bytes = detail::ResolveSourceBytes(source);
  const auto source_hash = bytes.empty() ? HashTextHex(source.uri) : detail::HashBytesHex(bytes);
  AssetRecord record;
  record.asset_class = AssetClass::Scene;
  record.name = detail::FileNameOrUri(source.uri);
  record.source_uri = source.uri;
  record.source_hash = source_hash;
  record.id = MakeAssetId(AssetClass::Scene, source.uri + ":" + source_hash);
  record.tags = {"fake", "test"};
  record.metadata = {{"importer", "fake"}};
  result.assets.push_back(std::move(record));
  result.deterministic_import_hash = HashTextHex(std::string("fake:") + source.uri + ":" + source_hash);
  result.success = true;
  result.status = vkpt::core::Status::ok("fake asset import completed");
  return result;
}

std::vector<AssetImportDiagnostic> FakeAssetImporter::emit_diagnostics() const {
  return {detail::Diagnostic(ImportDiagnosticSeverity::Info,
                             "fake.ready",
                             "fake metadata importer is available for importer interface tests")};
}

std::string_view GltfGlbImporter::importer_id() const {
  return "gltf_glb_mvp";
}

std::vector<std::string_view> GltfGlbImporter::supported_extensions() const {
  return {".gltf", ".glb"};
}

std::vector<std::string_view> GltfGlbImporter::supported_features() const {
  return {"gltf2", "glb", "static_mesh_metadata", "material_metadata", "texture_references"};
}

AssetValidationResult GltfGlbImporter::validate_source(const AssetImportSource& source,
                                                       const AssetImportOptions&) const {
  if (!detail::HasExtension(source.uri, supported_extensions())) {
    return {false, "unsupported extension", {}};
  }
  const auto bytes = detail::ResolveSourceBytes(source);
  if (bytes.empty()) {
    return {false, "source bytes unavailable", {}};
  }
  const auto ext = detail::ExtensionOf(source.uri);
  if (ext == ".glb") {
    if (bytes.size() < 20) {
      return {false, "GLB header is too small", {}};
    }
    if (detail::ReadU32Le(bytes, 0) != 0x46546c67u) {
      return {false, "missing GLB magic", {}};
    }
    if (detail::ReadU32Le(bytes, 4) != 2u) {
      return {false, "only GLB version 2 is supported", {}};
    }
    if (detail::ReadU32Le(bytes, 8) > bytes.size()) {
      return {false, "GLB declared length exceeds source size", {}};
    }
    if (detail::ReadU32Le(bytes, 16) != 0x4e4f534au) {
      return {false, "first GLB chunk is not JSON", {}};
    }
    return {true, "", {}};
  }
  const auto text = detail::BytesToString(bytes);
  if (text.find("\"asset\"") == std::string::npos || text.find("\"version\"") == std::string::npos) {
    return {false, "gltf JSON must contain asset.version", {}};
  }
  return {true, "", {}};
}

AssetImportResult GltfGlbImporter::import_source(const AssetImportSource& source,
                                                 const AssetImportOptions& options) const {
  AssetImportResult result;
  auto validation = validate_source(source, options);
  if (!validation.valid) {
    result.diagnostics = std::move(validation.diagnostics);
    result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Error,
                                                    "gltf.invalid",
                                                    validation.reason));
    result.status = vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument,
                                              validation.reason);
    return result;
  }

  const auto bytes = detail::ResolveSourceBytes(source);
  const auto source_hash = detail::HashBytesHex(bytes);
  const auto json = extract_json(bytes, detail::ExtensionOf(source.uri) == ".glb");
  const auto mesh_names = detail::ExtractObjectNames(json, "meshes");
  const auto material_names = detail::ExtractObjectNames(json, "materials");
  const auto image_uris = detail::ExtractImageUris(json);
  const auto texture_array = detail::ExtractNamedArray(json, "textures");
  const auto node_array = detail::ExtractNamedArray(json, "nodes");
  const auto texture_count = texture_array ? detail::TopLevelObjects(*texture_array).size() : image_uris.size();
  const auto node_count = node_array ? detail::TopLevelObjects(*node_array).size() : 0u;

  // MVP glTF import is metadata-first: records are stable even before buffer decode.
  AssetRecord scene;
  scene.asset_class = AssetClass::Scene;
  scene.name = detail::FileNameOrUri(source.uri);
  scene.source_uri = source.uri;
  scene.source_hash = source_hash;
  scene.id = MakeAssetId(AssetClass::Scene, source.uri + ":" + source_hash + "#scene");
  scene.tags = {"gltf", "scene"};
  scene.metadata = {
      {"importer", "gltf_glb_mvp"},
      {"mesh_count", std::to_string(mesh_names.size())},
      {"material_count", std::to_string(material_names.size())},
      {"texture_count", std::to_string(texture_count)},
      {"node_count", std::to_string(node_count)},
      {"metadata_only", options.metadata_only ? "true" : "false"},
  };
  result.assets.push_back(std::move(scene));

  for (std::size_t i = 0; i < mesh_names.size(); ++i) {
    AssetRecord mesh;
    mesh.asset_class = AssetClass::Mesh;
    mesh.name = mesh_names[i].empty() ? "mesh_" + std::to_string(i) : mesh_names[i];
    mesh.source_uri = source.uri + "#mesh/" + std::to_string(i);
    mesh.source_hash = HashTextHex(std::string(json.substr(0, std::min<std::size_t>(json.size(), 4096))) +
                                    mesh.source_uri);
    mesh.id = MakeAssetId(AssetClass::Mesh, mesh.source_uri + ":" + mesh.source_hash);
    mesh.tags = {"gltf", "static_mesh"};
    mesh.metadata = {{"mesh_index", std::to_string(i)}, {"importer", "gltf_glb_mvp"}};
    result.assets.push_back(std::move(mesh));
  }

  for (std::size_t i = 0; i < material_names.size(); ++i) {
    AssetRecord material;
    material.asset_class = AssetClass::Material;
    material.name = material_names[i].empty() ? "material_" + std::to_string(i) : material_names[i];
    material.source_uri = source.uri + "#material/" + std::to_string(i);
    material.source_hash = HashTextHex(source_hash + material.source_uri);
    material.id = MakeAssetId(AssetClass::Material, material.source_uri + ":" + material.source_hash);
    material.tags = {"gltf", "pbr", "material"};
    material.metadata = {
        {"material_index", std::to_string(i)},
        {"workflow", "metallic_roughness"},
        {"importer", "gltf_glb_mvp"},
    };
    result.assets.push_back(std::move(material));
  }

  const std::size_t texture_assets = std::max<std::size_t>(texture_count, image_uris.size());
  for (std::size_t i = 0; i < texture_assets; ++i) {
    const std::string image_uri = i < image_uris.size() && !image_uris[i].empty()
                                      ? image_uris[i]
                                      : source.uri + "#image/" + std::to_string(i);
    TextureDesc desc;
    desc.channel_semantic = TextureChannelSemantic::BaseColor;
    desc.color_space = TextureColorSpace::Srgb;
    desc.source_hash = HashTextHex(source_hash + image_uri);
    auto texture = MakeTextureAssetRecord(detail::FileNameOrUri(image_uri), image_uri, desc);
    texture.tags.push_back("gltf");
    texture.metadata["texture_index"] = std::to_string(i);
    texture.metadata["importer"] = "gltf_glb_mvp";
    result.assets.push_back(std::move(texture));
  }

  result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                                                  "gltf.metadata_only",
                                                  "glTF/GLB MVP currently imports deterministic mesh/material/texture metadata; buffer geometry decode is deferred",
                                                  true));
  if (material_names.empty()) {
    result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                                                    "gltf.no_materials",
                                                    "glTF has no material entries; renderer should resolve benchmark-safe fallback material"));
  }
  result.deterministic_import_hash = HashTextHex(std::string("gltf:") + source.uri + ":" + source_hash + ":" +
                                                 std::to_string(result.assets.size()));
  result.success = true;
  result.status = vkpt::core::Status::ok("glTF asset import completed");
  return result;
}

std::vector<AssetImportDiagnostic> GltfGlbImporter::emit_diagnostics() const {
  return {detail::Diagnostic(ImportDiagnosticSeverity::Info,
                             "gltf.ready",
                             "glTF/GLB metadata importer is registered for static mesh/material/texture MVP imports")};
}

std::string GltfGlbImporter::extract_json(const std::vector<std::byte>& bytes, bool glb) {
  if (!glb) {
    return detail::BytesToString(bytes);
  }
  const auto json_length = detail::ReadU32Le(bytes, 12);
  const auto json_begin = 20u;
  if (bytes.size() < json_begin || json_length > bytes.size() - json_begin) {
    return {};
  }
  std::string text;
  text.resize(json_length);
  for (std::size_t i = 0; i < json_length; ++i) {
    text[i] = static_cast<char>(detail::Byte(bytes, json_begin + i));
  }
  return text;
}

std::string_view ObjMtlImporter::importer_id() const {
  return "obj_mtl_mvp";
}

std::vector<std::string_view> ObjMtlImporter::supported_extensions() const {
  return {".obj"};
}

std::vector<std::string_view> ObjMtlImporter::supported_features() const {
  return {"obj", "mtl", "static_mesh_metadata", "triangle_geometry_decode", "scene_asset_expansion",
          "legacy_material_to_pbr_fallback"};
}

AssetValidationResult ObjMtlImporter::validate_source(const AssetImportSource& source,
                                                      const AssetImportOptions&) const {
  if (!detail::HasExtension(source.uri, supported_extensions())) {
    return {false, "unsupported extension", {}};
  }
  const auto bytes = detail::ResolveSourceBytes(source);
  if (bytes.empty()) {
    return {false, "source bytes unavailable", {}};
  }
  const auto text = detail::BytesToString(bytes);
  if (text.find("\nv ") == std::string::npos && !text.starts_with("v ")) {
    return {false, "OBJ has no vertex positions", {}};
  }
  return {true, "", {}};
}

AssetImportResult ObjMtlImporter::import_source(const AssetImportSource& source,
                                                const AssetImportOptions& options) const {
  AssetImportResult result;
  auto validation = validate_source(source, options);
  if (!validation.valid) {
    result.diagnostics = std::move(validation.diagnostics);
    result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Error,
                                                    "obj.invalid",
                                                    validation.reason));
    result.status = vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument,
                                              validation.reason);
    return result;
  }

  const auto bytes = detail::ResolveSourceBytes(source);
  const auto text = detail::BytesToString(bytes);
  const auto source_hash = detail::HashBytesHex(bytes);
  std::size_t positions = 0;
  std::size_t normals = 0;
  std::size_t texcoords = 0;
  std::size_t faces = 0;
  std::size_t non_tri_faces = 0;
  std::set<std::string> mtllibs;
  std::set<std::string> used_materials;

  // OBJ is scanned once for deterministic inventory and compatibility diagnostics.
  for (const auto& raw_line : detail::SplitLines(text)) {
    const auto line = detail::Trim(raw_line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const auto words = detail::SplitWords(line);
    if (words.empty()) {
      continue;
    }
    if (words[0] == "v") {
      ++positions;
    } else if (words[0] == "vn") {
      ++normals;
    } else if (words[0] == "vt") {
      ++texcoords;
    } else if (words[0] == "f") {
      ++faces;
      if (words.size() != 4) {
        ++non_tri_faces;
      }
    } else if (words[0] == "mtllib" && words.size() >= 2) {
      mtllibs.insert(words[1]);
    } else if (words[0] == "usemtl" && words.size() >= 2) {
      used_materials.insert(words[1]);
    }
  }

  AssetRecord scene;
  scene.asset_class = AssetClass::Scene;
  scene.name = detail::FileNameOrUri(source.uri);
  scene.source_uri = source.uri;
  scene.source_hash = source_hash;
  scene.id = MakeAssetId(AssetClass::Scene, source.uri + ":" + source_hash + "#obj_scene");
  scene.tags = {"obj", "scene"};
  scene.metadata = {
      {"importer", "obj_mtl_mvp"},
      {"mtllib_count", std::to_string(mtllibs.size())},
      {"used_material_count", std::to_string(used_materials.size())},
  };
  result.assets.push_back(std::move(scene));

  AssetRecord mesh;
  mesh.asset_class = AssetClass::Mesh;
  mesh.name = detail::FileNameOrUri(source.uri);
  mesh.source_uri = source.uri + "#mesh/0";
  mesh.source_hash = HashTextHex(source_hash + mesh.source_uri);
  mesh.id = MakeAssetId(AssetClass::Mesh, mesh.source_uri + ":" + mesh.source_hash);
  mesh.tags = {"obj", "static_mesh"};
  mesh.metadata = {
      {"importer", "obj_mtl_mvp"},
      {"position_count", std::to_string(positions)},
      {"normal_count", std::to_string(normals)},
      {"texcoord_count", std::to_string(texcoords)},
      {"face_count", std::to_string(faces)},
      {"non_tri_face_count", std::to_string(non_tri_faces)},
  };
  result.assets.push_back(std::move(mesh));

  const auto parsed_materials = parse_mtl_material_names(source, options, mtllibs);
  std::set<std::string> material_names = parsed_materials;
  material_names.insert(used_materials.begin(), used_materials.end());
  if (material_names.empty()) {
    material_names.insert("obj_fallback_material");
  }
  std::size_t material_index = 0;
  for (const auto& material_name : material_names) {
    AssetRecord material;
    material.asset_class = AssetClass::Material;
    material.name = material_name;
    material.source_uri = source.uri + "#material/" + material_name;
    material.source_hash = HashTextHex(source_hash + material.source_uri);
    material.id = MakeAssetId(AssetClass::Material, material.source_uri + ":" + material.source_hash);
    material.tags = {"obj", "mtl", "pbr_fallback"};
    material.metadata = {
        {"importer", "obj_mtl_mvp"},
        {"material_index", std::to_string(material_index++)},
        {"compatibility", "legacy_mtl_to_pbr_fallback"},
    };
    result.assets.push_back(std::move(material));
  }

  if (non_tri_faces > 0) {
    result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                                                    "obj.non_tri_faces",
                                                    "OBJ contains non-triangle faces; triangulation must be applied before renderer upload",
                                                    true));
  }
  result.diagnostics.push_back(detail::Diagnostic(ImportDiagnosticSeverity::Warning,
                                                  "obj.legacy_materials",
                                                  "OBJ/MTL material fields are mapped to PBR fallback descriptors with compatibility notes",
                                                  true));
  result.deterministic_import_hash = HashTextHex(std::string("obj:") + source.uri + ":" + source_hash + ":" +
                                                 std::to_string(result.assets.size()));
  result.success = true;
  result.status = vkpt::core::Status::ok("OBJ asset import completed");
  return result;
}

std::vector<AssetImportDiagnostic> ObjMtlImporter::emit_diagnostics() const {
  return {detail::Diagnostic(ImportDiagnosticSeverity::Info,
                             "obj.ready",
                             "OBJ/MTL importer is registered with scene expansion support and legacy material compatibility diagnostics")};
}

std::set<std::string> ObjMtlImporter::parse_mtl_material_names(const AssetImportSource& source,
                                                               const AssetImportOptions& options,
                                                               const std::set<std::string>& mtllibs) {
  std::set<std::string> out;
  for (const auto& mtl : mtllibs) {
    std::string text;
    if (const auto it = options.sidecar_text_by_uri.find(mtl); it != options.sidecar_text_by_uri.end()) {
      text = it->second;
    } else {
      const auto obj_path = std::filesystem::path(source.uri);
      const auto mtl_path = obj_path.parent_path() / mtl;
      std::ifstream file(mtl_path);
      if (file) {
        std::ostringstream buffer;
        buffer << file.rdbuf();
        text = buffer.str();
      }
    }
    for (const auto& raw_line : detail::SplitLines(text)) {
      const auto line = detail::Trim(raw_line);
      const auto words = detail::SplitWords(line);
      if (words.size() >= 2 && words[0] == "newmtl") {
        out.insert(words[1]);
      }
    }
  }
  return out;
}

ImporterRegistry CreateDefaultImporterRegistry(bool include_fake_importer) {
  ImporterRegistry registry;
  registry.register_importer(std::make_shared<GltfGlbImporter>());
  registry.register_importer(std::make_shared<ObjMtlImporter>());
  registry.register_importer(std::make_shared<TextureMetadataImporter>());
  registry.register_importer(std::make_shared<ExrPolicyImporter>());
  if (include_fake_importer) {
    registry.register_importer(std::make_shared<FakeAssetImporter>());
  }
  return registry;
}

std::string SerializeAssetCapabilityMatrix(const ImporterRegistry& registry,
                                           const ExrSupportPolicy& exr_policy) {
  std::ostringstream out;
  out << "{\"schema\":\"vkpt.asset_capability_matrix.v1\",";
  out << "\"importers\":[";
  const auto& importers = registry.importers();
  for (std::size_t i = 0; i < importers.size(); ++i) {
    if (i > 0) out << ",";
    out << "{";
    out << "\"id\":\"" << EscapeJson(importers[i]->importer_id()) << "\",";
    out << "\"extensions\":[";
    const auto extensions = importers[i]->supported_extensions();
    for (std::size_t j = 0; j < extensions.size(); ++j) {
      if (j > 0) out << ",";
      out << "\"" << EscapeJson(extensions[j]) << "\"";
    }
    out << "],\"features\":[";
    const auto features = importers[i]->supported_features();
    for (std::size_t j = 0; j < features.size(); ++j) {
      if (j > 0) out << ",";
      out << "\"" << EscapeJson(features[j]) << "\"";
    }
    out << "]}";
  }
  out << "],\"exr\":" << SerializeExrSupportPolicy(exr_policy) << "}";
  return out.str();
}

}  // namespace vkpt::assets
