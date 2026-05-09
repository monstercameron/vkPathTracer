#ifdef PT_ENABLE_QT
#include "app/QtDockPanelsInternal.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace vkpt::app {

QtDockRayDeviceMetric QtRayMetricAccumulator::update(std::string device_key,
                                                     std::string device_name,
                                                     std::uint64_t total_rays,
                                                     std::uint32_t sample_count,
                                                     std::chrono::steady_clock::time_point now) {
  const bool reset =
      !initialized ||
      total_rays < observed_total_rays ||
      sample_count < observed_sample_count;

  if (reset) {
    initialized = true;
    baseline_total_rays = total_rays;
    last_total_rays = total_rays;
    observed_total_rays = total_rays;
    last_sample_count = sample_count;
    observed_sample_count = sample_count;
    start_time = now;
    last_time = now;
    instant_rays_per_second = 0.0;
    rolling_rays_per_second = 0.0;
    accumulated_rays_per_second = 0.0;
  } else {
    const double elapsed = std::chrono::duration<double>(now - start_time).count();
    if (elapsed > 0.0 && total_rays >= baseline_total_rays) {
      accumulated_rays_per_second =
          static_cast<double>(total_rays - baseline_total_rays) / elapsed;
    }

    if (last_time != std::chrono::steady_clock::time_point{} && now > last_time) {
      const double dt = std::chrono::duration<double>(now - last_time).count();
      if (dt >= 0.05) {
        instant_rays_per_second =
            static_cast<double>(total_rays - last_total_rays) / dt;
        const double alpha = std::clamp(dt / 2.0, 0.05, 0.35);
        rolling_rays_per_second = rolling_rays_per_second <= 0.0
            ? instant_rays_per_second
            : (rolling_rays_per_second * (1.0 - alpha) +
               instant_rays_per_second * alpha);
        last_total_rays = total_rays;
        last_sample_count = sample_count;
        last_time = now;
      }
    }
    observed_total_rays = total_rays;
    observed_sample_count = sample_count;
  }

  QtDockRayDeviceMetric metric;
  metric.device_key = std::move(device_key);
  metric.device_name = std::move(device_name);
  metric.sample_count = sample_count;
  metric.total_rays = total_rays;
  metric.instant_rays_per_second = instant_rays_per_second;
  metric.rolling_rays_per_second = rolling_rays_per_second;
  metric.accumulated_rays_per_second = accumulated_rays_per_second;
  metric.measured = initialized && (sample_count > 0u || total_rays > baseline_total_rays);
  return metric;
}
std::string QtDockBool(bool value) {
  return value ? "true" : "false";
}

std::string QtDockNumber(double value, int precision) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

std::string QtDockBytes(std::uint64_t bytes) {
  constexpr double kKiB = 1024.0;
  constexpr double kMiB = kKiB * 1024.0;
  constexpr double kGiB = kMiB * 1024.0;
  const double value = static_cast<double>(bytes);
  if (bytes == 0u) {
    return "0 B";
  }
  if (value >= kGiB) {
    return QtDockNumber(value / kGiB, 2) + " GiB";
  }
  if (value >= kMiB) {
    return QtDockNumber(value / kMiB, 1) + " MiB";
  }
  if (value >= kKiB) {
    return QtDockNumber(value / kKiB, 1) + " KiB";
  }
  return std::to_string(bytes) + " B";
}

std::string QtDockRate(double raysPerSecond) {
  const double value = std::max(0.0, raysPerSecond);
  if (value >= 1.0e9) {
    return QtDockNumber(value / 1.0e9, 2) + " GRays/s";
  }
  if (value >= 1.0e6) {
    return QtDockNumber(value / 1.0e6, 2) + " MRays/s";
  }
  if (value >= 1.0e3) {
    return QtDockNumber(value / 1.0e3, 2) + " kRays/s";
  }
  return QtDockNumber(value, 1) + " Rays/s";
}

int QtDockPreferredPixels(float value) {
  if (!std::isfinite(value) || value <= 0.0f) {
    return 0;
  }
  return static_cast<int>(std::round(std::clamp(value, 1.0f, 4096.0f)));
}

std::uint64_t EstimateQtSceneMemoryBytes(const vkpt::pathtracer::PathTracerSceneSnapshot& scene) {
  return static_cast<std::uint64_t>(scene.vertices.size() * sizeof(vkpt::pathtracer::Vec3)) +
         static_cast<std::uint64_t>(scene.indices.size() * sizeof(std::uint32_t)) +
         static_cast<std::uint64_t>(scene.materials.size() * sizeof(vkpt::pathtracer::RTMaterial)) +
         static_cast<std::uint64_t>(scene.instances.size() * sizeof(vkpt::pathtracer::RTInstance)) +
         static_cast<std::uint64_t>(scene.tessellation_requests.size() * sizeof(vkpt::pathtracer::RTTessellationRequest)) +
         static_cast<std::uint64_t>(scene.lights.size() * sizeof(vkpt::pathtracer::RTHitLight)) +
         static_cast<std::uint64_t>(scene.sdf_primitives.size() * sizeof(vkpt::pathtracer::RTSdfPrimitive));
}

std::string QtDockFeatureSummary(const vkpt::render::RenderBackendCapabilities& caps) {
  std::vector<std::string> features;
  if (caps.compute) {
    features.push_back("compute");
  }
  if (caps.ray_tracing) {
    features.push_back("ray tracing");
  }
  if (caps.ray_query_supported || caps.ray_query) {
    features.push_back("ray query");
  }
  if (caps.timestamp_queries) {
    features.push_back("timing");
  }
  if (features.empty()) {
    return "basic";
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < features.size(); ++i) {
    if (i > 0u) {
      out << ", ";
    }
    out << features[i];
  }
  return out.str();
}

std::string QtDockAcceleratorKind(const vkpt::render::AcceleratorCapabilities& accel) {
  using vkpt::render::AcceleratorKind;
  switch (accel.accelerator_kind) {
    case AcceleratorKind::DiscreteGpu:
      return "Discrete GPU";
    case AcceleratorKind::IntegratedGpu:
      return "Integrated GPU";
    case AcceleratorKind::Warp:
      return "Software adapter";
    case AcceleratorKind::Cpu:
      return "CPU";
    case AcceleratorKind::VirtualGpu:
      return "Virtual GPU";
    case AcceleratorKind::Unknown:
    default:
      return "Unknown";
  }
}

std::string QtDockMemoryUsage(std::uint64_t usage,
                              std::uint64_t budget,
                              std::string_view unavailable_reason) {
  if (usage == 0u && budget == 0u) {
    return unavailable_reason.empty() ? std::string("not reported") : std::string(unavailable_reason);
  }
  if (budget == 0u) {
    return QtDockBytes(usage);
  }
  std::ostringstream out;
  out << QtDockBytes(usage) << " / " << QtDockBytes(budget);
  if (usage > 0u) {
    const double percent = static_cast<double>(usage) * 100.0 / static_cast<double>(budget);
    out << " (" << QtDockNumber(percent, 1) << "%)";
  }
  return out.str();
}

std::string QtDockJoin(const std::vector<std::string>& parts, std::string_view separator) {
  std::ostringstream out;
  bool first = true;
  for (const auto& part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!first) {
      out << separator;
    }
    out << part;
    first = false;
  }
  return out.str();
}

std::string QtDockToLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

std::string QtDockPathString(const std::filesystem::path& path) {
  return path.lexically_normal().generic_string();
}

std::filesystem::path QtDockAbsoluteNormalizedPath(std::string_view path) {
  // Hot path for QtDockSamePath, called once per scene-library entry inside
  // BuildQtAssetBrowserDock. weakly_canonical does multiple stat() syscalls;
  // cache by input string.
  static std::mutex mtx;
  static std::unordered_map<std::string, std::filesystem::path> cache;

  std::string key(path);
  {
    std::lock_guard<std::mutex> lock(mtx);
    if (const auto it = cache.find(key); it != cache.end()) {
      return it->second;
    }
  }

  std::filesystem::path candidate{key};
  std::error_code ec;
  if (candidate.is_relative()) {
    candidate = std::filesystem::current_path(ec) / candidate;
  }
  auto normalized = std::filesystem::weakly_canonical(candidate, ec);
  if (ec) {
    normalized = std::filesystem::absolute(candidate, ec);
  }
  auto result = ec ? candidate.lexically_normal() : normalized.lexically_normal();

  std::lock_guard<std::mutex> lock(mtx);
  cache.emplace(std::move(key), result);
  return result;
}

std::string QtDockDisplayPath(const std::filesystem::path& path) {
  // Called once per asset-library entry inside BuildQtAssetBrowserDock.
  // current_path() + relative() each issue syscalls, so cache by the input
  // path's normalized string. Working directory is stable across the
  // process lifetime in practice — we rebuild the cache whenever cwd
  // changes by capturing it as part of the key.
  static std::mutex mtx;
  static std::unordered_map<std::string, std::string> cache;
  static std::string cached_cwd;

  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  std::string cwdString = ec ? std::string{} : cwd.generic_string();
  const std::string pathKey = path.generic_string();
  {
    std::lock_guard<std::mutex> lock(mtx);
    if (cwdString != cached_cwd) {
      cache.clear();
      cached_cwd = cwdString;
    } else if (const auto it = cache.find(pathKey); it != cache.end()) {
      return it->second;
    }
  }

  std::string out;
  if (!ec) {
    const auto relative = std::filesystem::relative(path, cwd, ec);
    if (!ec && !relative.empty()) {
      const auto text = QtDockPathString(relative);
      if (!text.starts_with("..")) {
        out = text;
      }
    }
  }
  if (out.empty()) {
    out = QtDockPathString(path);
  }

  std::lock_guard<std::mutex> lock(mtx);
  cache.emplace(pathKey, out);
  return out;
}

std::filesystem::path QtDockFindRepoRelativePath(const std::filesystem::path& relativePath) {
  // Repo layout doesn't change at runtime; resolve once per (cwd, relative)
  // pair. Without this cache every dock rebuild re-walks 8 parent dirs and
  // does an `exists()` syscall per level, just to point at assets/ or game/.
  struct CacheKey {
    std::string cwd;
    std::string rel;
    bool operator==(const CacheKey& other) const noexcept {
      return cwd == other.cwd && rel == other.rel;
    }
  };
  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const noexcept {
      return std::hash<std::string>{}(key.cwd) ^
             (std::hash<std::string>{}(key.rel) << 1u);
    }
  };
  static std::mutex mtx;
  static std::unordered_map<CacheKey, std::filesystem::path, CacheKeyHash> cache;

  std::error_code ec;
  auto cwd = std::filesystem::current_path(ec);
  if (ec) {
    return relativePath;
  }

  CacheKey key{cwd.generic_string(), relativePath.generic_string()};
  {
    std::lock_guard<std::mutex> lock(mtx);
    if (const auto it = cache.find(key); it != cache.end()) {
      return it->second;
    }
  }

  auto current = cwd;
  std::filesystem::path resolved = relativePath;
  for (int i = 0; i < 8; ++i) {
    const auto candidate = (current / relativePath).lexically_normal();
    if (std::filesystem::exists(candidate, ec) && !ec) {
      resolved = candidate;
      break;
    }
    ec.clear();
    if (!current.has_parent_path() || current.parent_path() == current) {
      break;
    }
    current = current.parent_path();
  }

  std::lock_guard<std::mutex> lock(mtx);
  cache.emplace(std::move(key), resolved);
  return resolved;
}

bool QtDockSamePath(std::string_view lhs, std::string_view rhs) {
  if (lhs.empty() || rhs.empty()) {
    return false;
  }
  return QtDockPathString(QtDockAbsoluteNormalizedPath(lhs)) ==
         QtDockPathString(QtDockAbsoluteNormalizedPath(rhs));
}

bool QtDockHasExtension(const std::filesystem::path& path,
                        std::initializer_list<std::string_view> extensions) {
  const auto ext = QtDockToLower(path.extension().string());
  for (const auto allowed : extensions) {
    if (ext == allowed) {
      return true;
    }
  }
  return false;
}

std::vector<std::filesystem::path> QtDockFindAssetFiles(
    const std::filesystem::path& root,
    std::initializer_list<std::string_view> extensions,
    bool recursive) {
  // Asset directory listings are the dominant cost of the dock-panel rebuild
  // when game/models/lods (88+ files) and assets/scenes (28+ files) get
  // walked every refresh: each file becomes a stat() and (in debug builds)
  // a heap allocation for the lexically_normal path. The asset/script
  // directories are stable across the 500ms-throttled rebuild cadence, so
  // cache results with a short TTL keyed on (root, ext-set, recursive).
  // Refreshing every 3 seconds keeps the panel reactive to user-added
  // scenes/scripts without paying the recursive scan on every refresh.
  struct CacheKey {
    std::string root;
    std::string ext_signature;
    bool recursive;
    bool operator==(const CacheKey& other) const noexcept {
      return recursive == other.recursive && root == other.root &&
             ext_signature == other.ext_signature;
    }
  };
  struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const noexcept {
      return std::hash<std::string>{}(key.root) ^
             (std::hash<std::string>{}(key.ext_signature) << 1u) ^
             (key.recursive ? 0x9E3779B9u : 0u);
    }
  };
  struct CacheEntry {
    std::vector<std::filesystem::path> files;
    std::chrono::steady_clock::time_point captured_at{};
  };
  static std::mutex mtx;
  static std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> cache;
  constexpr auto kTtl = std::chrono::seconds(10);

  std::string ext_signature;
  ext_signature.reserve(64u);
  for (const auto ext : extensions) {
    ext_signature.append(ext);
    ext_signature.push_back('|');
  }
  CacheKey key{root.generic_string(), std::move(ext_signature), recursive};
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(mtx);
    if (const auto it = cache.find(key); it != cache.end()) {
      if (now - it->second.captured_at < kTtl) {
        return it->second.files;
      }
    }
  }

  std::vector<std::filesystem::path> files;
  std::error_code ec;
  if (!std::filesystem::exists(root, ec) || ec || !std::filesystem::is_directory(root, ec) || ec) {
    std::lock_guard<std::mutex> lock(mtx);
    cache[key] = CacheEntry{files, now};
    return files;
  }

  const auto visit = [&](const std::filesystem::directory_entry& entry) {
    std::error_code entryEc;
    if (entry.is_regular_file(entryEc) &&
        !entryEc &&
        QtDockHasExtension(entry.path(), extensions)) {
      files.push_back(entry.path().lexically_normal());
    }
  };

  if (recursive) {
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator{};
         it.increment(ec)) {
      visit(*it);
    }
  } else {
    for (std::filesystem::directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator{};
         it.increment(ec)) {
      visit(*it);
    }
  }

  std::sort(files.begin(), files.end(), [](const auto& lhs, const auto& rhs) {
    return QtDockPathString(lhs) < QtDockPathString(rhs);
  });

  std::lock_guard<std::mutex> lock(mtx);
  cache[key] = CacheEntry{files, now};
  return files;
}

std::string QtDockPrettyStem(const std::filesystem::path& path) {
  std::string label = path.stem().string();
  for (char& c : label) {
    if (c == '_' || c == '-') {
      c = ' ';
    }
  }
  bool capitalize = true;
  for (char& c : label) {
    const auto ch = static_cast<unsigned char>(c);
    if (std::isspace(ch) != 0) {
      capitalize = true;
      continue;
    }
    if (capitalize) {
      c = static_cast<char>(std::toupper(ch));
      capitalize = false;
    }
  }
  return label.empty() ? path.filename().string() : label;
}

std::string QtDockReadSceneDisplayName(const std::filesystem::path& path) {
  // Reading + JSON-parsing every scene file on every dock refresh dominates
  // BuildQtAssetBrowserDock when the scene library is large (28+ scenes in
  // the representative_acceptance_scene gate). Cache the parsed metadata
  // name keyed on (path, last_write_time) so unchanged files are free.
  struct CacheEntry {
    std::filesystem::file_time_type mtime{};
    std::string display_name;
  };
  static std::mutex mtx;
  static std::unordered_map<std::string, CacheEntry> cache;

  const auto pathKey = path.generic_string();
  std::error_code ec;
  const auto mtime = std::filesystem::last_write_time(path, ec);
  if (!ec) {
    std::lock_guard<std::mutex> lock(mtx);
    if (const auto it = cache.find(pathKey);
        it != cache.end() && it->second.mtime == mtime) {
      return it->second.display_name;
    }
  }

  std::string result;
  std::ifstream file(path);
  if (!file) {
    result = QtDockPrettyStem(path);
  } else {
    // Read at most a few KiB and look for "scene_name": "<value>" with a
    // narrow string scan instead of a full JsonParser pass. Authored scenes
    // place metadata at the top of the file, so this avoids parsing the
    // entire (potentially megabyte-sized) document just to harvest a label.
    // Falls back to QtDockPrettyStem when the substring search fails or
    // the file lacks the metadata.
    constexpr std::size_t kPrefixBytes = 8u * 1024u;
    std::string prefix(kPrefixBytes, '\0');
    file.read(prefix.data(), static_cast<std::streamsize>(kPrefixBytes));
    prefix.resize(static_cast<std::size_t>(file.gcount()));
    const auto sceneNameKey = std::string_view{"\"scene_name\""};
    auto pos = prefix.find(sceneNameKey);
    if (pos != std::string::npos) {
      pos += sceneNameKey.size();
      while (pos < prefix.size() &&
             (prefix[pos] == ' ' || prefix[pos] == ':' || prefix[pos] == '\t')) {
        ++pos;
      }
      if (pos < prefix.size() && prefix[pos] == '"') {
        ++pos;
        const auto end = prefix.find('"', pos);
        if (end != std::string::npos && end > pos) {
          result.assign(prefix, pos, end - pos);
        }
      }
    }
    if (result.empty()) {
      result = QtDockPrettyStem(path);
    }
  }

  if (!ec) {
    std::lock_guard<std::mutex> lock(mtx);
    cache[pathKey] = CacheEntry{mtime, result};
  }
  return result;
}

QtDockTreeRow QtDockAssetFileRow(std::string idPrefix,
                                 const std::filesystem::path& path,
                                 std::string label,
                                 std::string icon,
                                 bool selected) {
  const auto displayPath = QtDockDisplayPath(path);
  QtDockTreeRow row;
  row.id = std::move(idPrefix) + displayPath;
  row.label = std::move(label);
  row.value = displayPath;
  row.icon = std::move(icon);
  row.selected = selected;
  row.activatable = true;
  return row;
}

std::string QtDockVec3(float x, float y, float z) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(3)
      << x << ", " << y << ", " << z;
  return out.str();
}

std::string QtDockVec3(const vkpt::scene::Vec3& v) {
  return QtDockVec3(v.x, v.y, v.z);
}

std::string QtDockVec3(const vkpt::pathtracer::Vec3& v) {
  return QtDockVec3(v.x, v.y, v.z);
}

std::string QtDockVec3(const vkpt::editor::Vec3& v) {
  return QtDockVec3(v.x, v.y, v.z);
}

std::string QtDockBounds(const vkpt::editor::Bounds& bounds) {
  if (!bounds.valid) {
    return "invalid";
  }
  return "min(" + QtDockVec3(bounds.min) + ") max(" + QtDockVec3(bounds.max) + ")";
}

void QtDockAddProperty(QtDockPanelContent& panel,
                       std::string_view label,
                       std::string value) {
  QtDockProperty property;
  property.label = std::string(label);
  property.value = std::move(value);
  panel.properties.push_back(std::move(property));
}

void QtDockAddEditableGroupedProperty(QtDockPanelContent& panel,
                                      std::string id,
                                      std::string_view group,
                                      std::string_view label,
                                      std::string value,
                                      std::string unit) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = std::move(value);
  property.unit = std::move(unit);
  property.editable = true;
  property.enabled = true;
  panel.properties.push_back(std::move(property));
}

void QtDockAddTextGroupedProperty(QtDockPanelContent& panel,
                                  std::string id,
                                  std::string_view group,
                                  std::string_view label,
                                  std::string value) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = std::move(value);
  property.editor = "text";
  property.editable = true;
  property.enabled = true;
  panel.properties.push_back(std::move(property));
}

void QtDockAddDropdownGroupedProperty(QtDockPanelContent& panel,
                                      std::string id,
                                      std::string_view group,
                                      std::string_view label,
                                      std::string value,
                                      std::vector<std::string> options) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = std::move(value);
  property.editor = "dropdown";
  property.options = std::move(options);
  property.editable = true;
  property.enabled = true;
  panel.properties.push_back(std::move(property));
}

void QtDockAddToggleGroupedProperty(QtDockPanelContent& panel,
                                    std::string id,
                                    std::string_view group,
                                    std::string_view label,
                                    bool value) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = QtDockBool(value);
  property.editor = "toggle";
  property.editable = true;
  property.enabled = true;
  panel.properties.push_back(std::move(property));
}

void QtDockAddButtonGroupedProperty(QtDockPanelContent& panel,
                                    std::string id,
                                    std::string_view group,
                                    std::string_view label,
                                    std::string value) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = std::move(value);
  property.editor = "button";
  property.editable = true;
  property.enabled = true;
  panel.properties.push_back(std::move(property));
}

void QtDockAddSliderGroupedProperty(QtDockPanelContent& panel,
                                    std::string id,
                                    std::string_view group,
                                    std::string_view label,
                                    double value,
                                    double minimum,
                                    double maximum,
                                    double step,
                                    double default_value,
                                    std::string unit) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = QtDockNumber(value, step >= 1.0 ? 0 : 3);
  property.unit = std::move(unit);
  property.editor = "slider";
  property.minimum = minimum;
  property.maximum = maximum;
  property.step = step;
  property.default_value = default_value;
  property.has_numeric_range = true;
  property.has_default = true;
  property.editable = true;
  property.enabled = true;
  panel.properties.push_back(std::move(property));
}

void QtDockAddSliderProperty(QtDockPanelContent& panel,
                             std::string id,
                             std::string_view label,
                             double value,
                             double minimum,
                             double maximum,
                             double step,
                             double default_value) {
  QtDockAddSliderGroupedProperty(panel,
                                 std::move(id),
                                 std::string_view{},
                                 label,
                                 value,
                                 minimum,
                                 maximum,
                                 step,
                                 default_value);
}

std::vector<std::string> QtMaterialFamilyOptions() {
  return {
      "diffuse",
      "mirror",
      "glossy",
      "metallic_pbr",
      "ggx_rough_conductor",
      "dielectric_glass",
      "frosted_glass",
      "clearcoat",
      "velvet",
      "fabric_cloth",
      "toon_surface",
      "emissive",
      "wood",
      "oak",
      "walnut",
      "sandalwood",
      "pine",
      "teak",
      "mahogany",
      "procedural_material",
      "marble_scattering",
      "rust_progression",
      "thin_film_iridescent",
      "alpha_mask",
      "blackbody_emission",
      "wet_surface",
      "retroreflector",
      "normal_mapped_pbr",
      "xray"};
}

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
