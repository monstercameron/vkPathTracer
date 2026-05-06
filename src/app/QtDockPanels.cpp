#ifdef PT_ENABLE_QT

#include "app/QtDockPanels.h"

#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
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

std::string QtDockNumber(double value, int precision = 2) {
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

std::uint64_t EstimateQtSceneMemoryBytes(const vkpt::pathtracer::RTSceneData& scene) {
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

std::string QtDockJoin(const std::vector<std::string>& parts, std::string_view separator = " | ") {
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

std::string QtDockRayDeviceKeyForAccelerator(const vkpt::render::AcceleratorCapabilities& accel) {
  if (!accel.id.empty()) {
    return accel.id;
  }
  if (!accel.adapter_luid.empty()) {
    return std::string("luid:") + accel.adapter_luid;
  }
  std::ostringstream out;
  out << "accelerator:"
      << vkpt::render::AcceleratorKindToString(accel.accelerator_kind)
      << ":" << accel.vendor_id
      << ":" << accel.device_id
      << ":" << accel.name;
  return out.str();
}

std::string QtDockFallbackRayDeviceKey(std::string_view selected_backend,
                                       std::string_view renderer_path) {
  std::string backend(selected_backend.empty() ? std::string_view("unknown") : selected_backend);
  std::string renderer(renderer_path.empty() ? backend : renderer_path);
  return "backend:" + backend + ":" + renderer;
}

std::string QtDockActiveRayDeviceKey(const QtDockDeviceStats& device_stats) {
  if (device_stats.has_selected_accelerator) {
    return QtDockRayDeviceKeyForAccelerator(device_stats.selected_accelerator);
  }
  return QtDockFallbackRayDeviceKey(device_stats.selected_backend, device_stats.active_renderer_path);
}

std::string QtDockActiveRayDeviceName(const QtDockDeviceStats& device_stats) {
  if (device_stats.has_selected_accelerator && !device_stats.selected_accelerator.name.empty()) {
    return device_stats.selected_accelerator.name;
  }
  if (!device_stats.active_renderer_path.empty()) {
    return device_stats.active_renderer_path;
  }
  if (!device_stats.selected_backend.empty()) {
    return device_stats.selected_backend;
  }
  return "active renderer";
}

const QtDockRayDeviceMetric* QtDockFindRayMetric(
    const std::vector<QtDockRayDeviceMetric>& metrics,
    std::string_view device_key) {
  if (device_key.empty()) {
    return nullptr;
  }
  const auto it = std::find_if(metrics.begin(), metrics.end(), [&](const auto& metric) {
    return metric.device_key == device_key;
  });
  return it == metrics.end() ? nullptr : &(*it);
}

void QtDockUpsertRayMetric(std::vector<QtDockRayDeviceMetric>& metrics,
                           QtDockRayDeviceMetric metric) {
  if (metric.device_key.empty()) {
    return;
  }
  const auto it = std::find_if(metrics.begin(), metrics.end(), [&](const auto& existing) {
    return existing.device_key == metric.device_key;
  });
  if (it == metrics.end()) {
    metrics.push_back(std::move(metric));
  } else {
    *it = std::move(metric);
  }
}

bool QtDockRayMetricHasRate(const QtDockRayDeviceMetric* metric) {
  return metric != nullptr &&
         metric->measured &&
         (metric->rolling_rays_per_second > 0.0 ||
          metric->accumulated_rays_per_second > 0.0);
}

void QtDockAppendRayMetricTags(std::vector<std::string>& tags,
                               const QtDockRayDeviceMetric* metric) {
  if (!QtDockRayMetricHasRate(metric)) {
    return;
  }
  if (metric->rolling_rays_per_second > 0.0) {
    tags.push_back("rolling " + QtDockRate(metric->rolling_rays_per_second));
  }
  if (metric->accumulated_rays_per_second > 0.0) {
    tags.push_back("avg " + QtDockRate(metric->accumulated_rays_per_second));
  }
}

std::string QtDockShortAcceleratorKind(const vkpt::render::AcceleratorCapabilities& accel) {
  using vkpt::render::AcceleratorKind;
  switch (accel.accelerator_kind) {
    case AcceleratorKind::DiscreteGpu:
      return "dGPU";
    case AcceleratorKind::IntegratedGpu:
      return "iGPU";
    case AcceleratorKind::Warp:
      return "WARP";
    case AcceleratorKind::Cpu:
      return "CPU";
    case AcceleratorKind::VirtualGpu:
      return "vGPU";
    case AcceleratorKind::Unknown:
    default:
      return "device";
  }
}

bool QtDockSameAccelerator(const vkpt::render::AcceleratorCapabilities& lhs,
                           const vkpt::render::AcceleratorCapabilities& rhs) {
  if (!lhs.id.empty() && !rhs.id.empty()) {
    return lhs.id == rhs.id;
  }
  if (!lhs.adapter_luid.empty() && !rhs.adapter_luid.empty()) {
    return lhs.adapter_luid == rhs.adapter_luid;
  }
  return lhs.name == rhs.name &&
         lhs.accelerator_kind == rhs.accelerator_kind &&
         lhs.device_id == rhs.device_id &&
         lhs.vendor_id == rhs.vendor_id;
}

bool QtDockRayDeviceEligible(const vkpt::render::AcceleratorCapabilities& accel) {
  return accel.available &&
         (accel.compute || accel.ray_tracing || accel.cpu || accel.d3d12);
}

std::uint64_t QtDockAcceleratorMemoryBytes(const vkpt::render::AcceleratorCapabilities& accel) {
  if (accel.dedicated_video_memory_bytes > 0u) {
    return accel.dedicated_video_memory_bytes;
  }
  if (accel.current_budget_bytes > 0u) {
    return accel.current_budget_bytes;
  }
  return accel.shared_system_memory_bytes;
}

std::string QtDockAcceleratorSummary(const vkpt::render::AcceleratorCapabilities& accel,
                                     bool active,
                                     const QtDockRayDeviceMetric* metric = nullptr) {
  std::vector<std::string> tags;
  tags.push_back(QtDockShortAcceleratorKind(accel));
  if (accel.ray_tracing || accel.backend_caps.ray_tracing) {
    tags.push_back(accel.d3d12 ? "DXR" : "ray tracing");
  } else if (accel.compute || accel.backend_caps.compute) {
    tags.push_back("compute");
  }
  QtDockAppendRayMetricTags(tags, metric);
  if (!QtDockRayMetricHasRate(metric) && accel.estimated_rays_per_ms > 0.0) {
    tags.push_back("planning est " + QtDockRate(accel.estimated_rays_per_ms * 1000.0));
  }
  const auto memoryBytes = QtDockAcceleratorMemoryBytes(accel);
  if (memoryBytes > 0u) {
    tags.push_back(QtDockBytes(memoryBytes));
  }
  if (accel.selected_by_default && !active) {
    tags.push_back("default");
  }

  std::ostringstream out;
  if (active) {
    out << "active - ";
  }
  out << (accel.name.empty() ? std::string("unnamed device") : accel.name);
  const auto tagText = QtDockJoin(tags);
  if (!tagText.empty()) {
    out << "\n" << tagText;
  }
  return out.str();
}

std::string QtDockAcceleratorGroupSummary(
    const std::vector<const vkpt::render::AcceleratorCapabilities*>& accelerators,
    const std::vector<QtDockRayDeviceMetric>& metrics,
    std::size_t maxRows = 3u) {
  if (accelerators.empty()) {
    return "none";
  }

  std::ostringstream out;
  const std::size_t count = std::min(maxRows, accelerators.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (i > 0u) {
      out << "\n";
    }
    const auto* metric = QtDockFindRayMetric(
        metrics,
        QtDockRayDeviceKeyForAccelerator(*accelerators[i]));
    out << QtDockAcceleratorSummary(*accelerators[i], false, metric);
  }
  if (accelerators.size() > count) {
    out << "\n+" << (accelerators.size() - count) << " more";
  }
  return out.str();
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
                                      std::string unit = {}) {
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
                                    std::string unit = {}) {
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

std::string QtTrim(std::string_view text);
const vkpt::scene::SceneMaterialDefinition* FindQtSceneMaterial(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id);
const vkpt::scene::SceneGeometryDefinition* FindQtSceneGeometry(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id);
std::string QtEntityDisplayName(const vkpt::scene::SceneEntityDefinition& entity);

std::string QtStableIdDisplayLabel(std::string label, vkpt::core::StableId id) {
  label = QtTrim(label);
  if (label.empty()) {
    label = "item";
  }
  return label + " (#" + std::to_string(id) + ")";
}

std::string QtMaterialDisplayLabel(const vkpt::scene::SceneDocument& document,
                                   vkpt::core::StableId id) {
  if (id == 0u) {
    return "none";
  }
  for (const auto& material : document.materials) {
    if (material.id != id) {
      continue;
    }
    if (!material.name.empty()) {
      return QtStableIdDisplayLabel(material.name, id);
    }
    if (!material.family.empty()) {
      return QtStableIdDisplayLabel(material.family, id);
    }
  }
  for (const auto& entity : document.entities) {
    if ((entity.has_mesh && entity.mesh.material_id == id) ||
        entity.material.material_id == id) {
      const std::string entityName =
          entity.name.empty() ? "Entity " + std::to_string(entity.id) : entity.name;
      return QtStableIdDisplayLabel(entityName + " material", id);
    }
  }
  return QtStableIdDisplayLabel("material", id);
}

std::string QtGeometryDisplayLabel(const vkpt::scene::SceneDocument& document,
                                   vkpt::core::StableId id) {
  if (id == 0u) {
    return "none";
  }
  for (const auto& entity : document.entities) {
    if (entity.has_mesh && entity.mesh.mesh_id == id) {
      const std::string entityName =
          entity.name.empty() ? "Entity " + std::to_string(entity.id) : entity.name;
      return QtStableIdDisplayLabel(entityName + " mesh", id);
    }
  }
  for (const auto& geometry : document.geometry) {
    if (geometry.id != id) {
      continue;
    }
    if (!geometry.primitive.empty()) {
      return QtStableIdDisplayLabel(geometry.primitive, id);
    }
    if (!geometry.tags.empty() && !geometry.tags.front().empty()) {
      return QtStableIdDisplayLabel(geometry.tags.front(), id);
    }
  }
  return QtStableIdDisplayLabel("mesh", id);
}

std::vector<std::string> QtMaterialIdOptions(const vkpt::scene::SceneDocument& document,
                                             vkpt::core::StableId current) {
  std::vector<std::string> options;
  options.push_back("none");
  bool hasCurrent = current == 0u;
  for (const auto& material : document.materials) {
    options.push_back(QtMaterialDisplayLabel(document, material.id));
    hasCurrent = hasCurrent || material.id == current;
  }
  if (!hasCurrent) {
    options.push_back(QtMaterialDisplayLabel(document, current));
  }
  return options;
}

std::vector<std::string> QtGeometryIdOptions(const vkpt::scene::SceneDocument& document,
                                             vkpt::core::StableId current) {
  std::vector<std::string> options;
  bool hasCurrent = false;
  if (current == 0u) {
    options.push_back("none");
    hasCurrent = true;
  }
  for (const auto& geometry : document.geometry) {
    options.push_back(QtGeometryDisplayLabel(document, geometry.id));
    hasCurrent = hasCurrent || geometry.id == current;
  }
  if (!hasCurrent) {
    options.push_back(QtGeometryDisplayLabel(document, current));
  }
  return options;
}

std::vector<std::string> QtLightTypeOptions() {
  return {"point", "spot", "sphere", "directional", "environment"};
}

std::vector<std::string> QtSdfShapeOptions() {
  return {"sphere", "box", "rounded_box", "torus", "capsule", "plane"};
}

std::vector<std::string> QtPhysicsBodyTypeOptions() {
  return {"static", "dynamic", "kinematic"};
}

std::vector<std::string> QtPhysicsShapeOptions() {
  return {"box", "sphere", "capsule", "cylinder", "mesh"};
}

std::vector<std::string> QtBoolOptions() {
  return {"false", "true"};
}

std::vector<std::string> QtToneMapOptions() {
  return {"linear", "reinhard", "filmic_approx", "aces_approx"};
}

std::vector<std::string> QtOutputTransformOptions() {
  return {"gamma", "linear"};
}

std::string QtToneMapName(vkpt::pathtracer::ToneMapMode mode) {
  switch (mode) {
    case vkpt::pathtracer::ToneMapMode::Reinhard:
      return "reinhard";
    case vkpt::pathtracer::ToneMapMode::FilmicApprox:
      return "filmic_approx";
    case vkpt::pathtracer::ToneMapMode::AcesApprox:
      return "aces_approx";
    case vkpt::pathtracer::ToneMapMode::Linear:
    default:
      return "linear";
  }
}

std::string QtOutputTransformName(vkpt::pathtracer::OutputTransformMode mode) {
  switch (mode) {
    case vkpt::pathtracer::OutputTransformMode::Linear:
      return "linear";
    case vkpt::pathtracer::OutputTransformMode::Gamma:
    default:
      return "gamma";
  }
}

bool QtParseToneMapMode(std::string_view text, vkpt::pathtracer::ToneMapMode& out) {
  const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
    return std::isspace(c) != 0;
  }).base();
  std::string value = begin < end ? std::string(begin, end) : std::string{};
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (value == "linear") {
    out = vkpt::pathtracer::ToneMapMode::Linear;
    return true;
  }
  if (value == "reinhard") {
    out = vkpt::pathtracer::ToneMapMode::Reinhard;
    return true;
  }
  if (value == "filmic" || value == "filmic_approx") {
    out = vkpt::pathtracer::ToneMapMode::FilmicApprox;
    return true;
  }
  if (value == "aces" || value == "aces_approx") {
    out = vkpt::pathtracer::ToneMapMode::AcesApprox;
    return true;
  }
  return false;
}

bool QtParseOutputTransformMode(std::string_view text, vkpt::pathtracer::OutputTransformMode& out) {
  const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
    return std::isspace(c) != 0;
  }).base();
  std::string value = begin < end ? std::string(begin, end) : std::string{};
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (value == "linear") {
    out = vkpt::pathtracer::OutputTransformMode::Linear;
    return true;
  }
  if (value == "gamma" || value == "srgb" || value == "display") {
    out = vkpt::pathtracer::OutputTransformMode::Gamma;
    return true;
  }
  return false;
}

vkpt::scene::PhysicsBodyComponent QtDefaultDynamicPhysicsBody() {
  vkpt::scene::PhysicsBodyComponent body;
  body.enabled = true;
  body.dynamic = true;
  body.body_type = "dynamic";
  body.shape = "box";
  body.mass = 1.0f;
  body.friction = 0.5f;
  body.restitution = 0.0f;
  body.gravity_scale = 1.0f;
  body.trigger = false;
  body.allow_sleeping = true;
  body.continuous_collision = false;
  return body;
}

void QtDockAddVec3Sliders(QtDockPanelContent& panel,
                          std::string_view prefix,
                          std::string_view group,
                          std::string_view label,
                          const vkpt::scene::Vec3& value,
                          const vkpt::scene::Vec3& defaults,
                          double minimum,
                          double maximum,
                          double step) {
  const std::string base(prefix);
  const std::string labelBase(label);
  QtDockAddSliderGroupedProperty(panel, base + ".x", group, labelBase + " x",
                                 value.x, minimum, maximum, step, defaults.x);
  QtDockAddSliderGroupedProperty(panel, base + ".y", group, labelBase + " y",
                                 value.y, minimum, maximum, step, defaults.y);
  QtDockAddSliderGroupedProperty(panel, base + ".z", group, labelBase + " z",
                                 value.z, minimum, maximum, step, defaults.z);
}

void QtDockAddInspectorTransformControls(QtDockPanelContent& panel,
                                         std::string_view prefix,
                                         const vkpt::scene::TransformComponent& transform) {
  const std::string base(prefix);
  QtDockAddSliderProperty(panel, base + "translation.x", "Position X",
                          transform.translation.x, -10.0, 10.0, 0.01, 0.0);
  QtDockAddSliderProperty(panel, base + "translation.y", "Position Y",
                          transform.translation.y, -10.0, 10.0, 0.01, 0.0);
  QtDockAddSliderProperty(panel, base + "translation.z", "Position Z",
                          transform.translation.z, -10.0, 10.0, 0.01, 0.0);
  QtDockAddSliderProperty(panel, base + "scale.x", "Scale X",
                          transform.scale.x, 0.01, 10.0, 0.01, 1.0);
  QtDockAddSliderProperty(panel, base + "scale.y", "Scale Y",
                          transform.scale.y, 0.01, 10.0, 0.01, 1.0);
  QtDockAddSliderProperty(panel, base + "scale.z", "Scale Z",
                          transform.scale.z, 0.01, 10.0, 0.01, 1.0);
}

void QtDockAddPrimaryCameraControls(QtDockPanelContent& panel,
                                    std::string_view prefix,
                                    const vkpt::scene::CameraComponent& camera) {
  const std::string base(prefix);
  QtDockAddSliderProperty(panel, base + "fov", "FOV (deg)",
                          camera.fov, 1.0, 179.0, 0.1, 60.0);
  QtDockAddSliderProperty(panel, base + "focal_length_mm", "Focal length (mm)",
                          camera.focal_length_mm, 8.0, 300.0, 0.1, 35.0);
  QtDockAddSliderProperty(panel, base + "aperture_radius", "Aperture radius",
                          camera.aperture_radius, 0.0, 1.0, 0.001, 0.0);
  QtDockAddSliderProperty(panel, base + "focus_distance", "Focus distance",
                          camera.focus_distance, 0.0, 100.0, 0.01, 0.0);
  QtDockAddSliderProperty(panel, base + "f_stop", "F-stop",
                          camera.f_stop, 0.0, 32.0, 0.1, 0.0);
  QtDockAddSliderProperty(panel, base + "exposure_compensation", "Exposure (EV)",
                          camera.exposure_compensation, -8.0, 8.0, 0.1, 0.0);
  QtDockAddSliderProperty(panel, base + "white_balance_kelvin", "White balance (K)",
                          camera.white_balance_kelvin, 1000.0, 40000.0, 50.0, 6500.0);
  QtDockAddSliderProperty(panel, base + "iris_blade_count", "Iris blades",
                          static_cast<double>(camera.iris_blade_count), 0.0, 16.0, 1.0, 0.0);
}

void QtDockAddRow(QtDockPanelContent& panel, std::string row) {
  panel.rows.push_back(std::move(row));
}

void QtDockAddTreeRow(QtDockPanelContent& panel, QtDockTreeRow row) {
  panel.tree_rows.push_back(std::move(row));
}

const vkpt::editor::UiPanelState* FindQtLayoutPanel(
    const vkpt::editor::UiLayoutDocument& layout,
    std::string_view panel_id) {
  const auto it = std::find_if(layout.panels.begin(),
                               layout.panels.end(),
                               [panel_id](const vkpt::editor::UiPanelState& panel) {
                                 return panel.id == panel_id;
                               });
  return it == layout.panels.end() ? nullptr : &*it;
}

QtDockPanelContent MakeQtDockPanel(const vkpt::editor::UiLayoutDocument& layout,
                                   std::string_view id,
                                   std::string_view title,
                                   bool default_visible,
                                   float default_width = 320.0f,
                                   float default_height = 240.0f) {
  QtDockPanelContent panel;
  panel.id = std::string(id);
  panel.title = std::string(title);
  panel.visible = default_visible;
  panel.width = default_width;
  panel.height = default_height;
  const vkpt::editor::UiPanelState* state = FindQtLayoutPanel(layout, id);
  if (state == nullptr && id == "scene_graph") {
    state = FindQtLayoutPanel(layout, "scene_tree");
  } else if (state == nullptr && id == "materials") {
    state = FindQtLayoutPanel(layout, "material_editor");
  } else if (state == nullptr && id == "diagnostics") {
    state = FindQtLayoutPanel(layout, "console");
  }
  if (state != nullptr) {
    panel.visible = state->visible;
    panel.docked = state->docked;
    panel.floating = state->floating;
    panel.collapsed = state->collapsed;
    panel.width = state->width;
    panel.height = state->height;
  }
  return panel;
}

const vkpt::scene::SceneEntityDefinition* FindQtSceneEntity(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.entities.begin(),
                               document.entities.end(),
                               [id](const vkpt::scene::SceneEntityDefinition& entity) {
                                 return entity.id == id;
                               });
  return it == document.entities.end() ? nullptr : &*it;
}

const vkpt::scene::SceneMaterialDefinition* FindQtSceneMaterial(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.materials.begin(),
                               document.materials.end(),
                               [id](const vkpt::scene::SceneMaterialDefinition& material) {
                                 return material.id == id;
                               });
  return it == document.materials.end() ? nullptr : &*it;
}

const vkpt::scene::SceneGeometryDefinition* FindQtSceneGeometry(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.geometry.begin(),
                               document.geometry.end(),
                               [id](const vkpt::scene::SceneGeometryDefinition& geometry) {
                                 return geometry.id == id;
                               });
  return it == document.geometry.end() ? nullptr : &*it;
}

const vkpt::scene::SceneSdfPrimitiveDefinition* FindQtSceneSdfPrimitive(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.sdf_primitives.begin(),
                               document.sdf_primitives.end(),
                               [id](const vkpt::scene::SceneSdfPrimitiveDefinition& primitive) {
                                 return primitive.id == id;
                               });
  return it == document.sdf_primitives.end() ? nullptr : &*it;
}

vkpt::scene::SceneMaterialDefinition* FindQtMutableSceneMaterial(
    vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.materials.begin(),
                               document.materials.end(),
                               [id](const vkpt::scene::SceneMaterialDefinition& material) {
                                 return material.id == id;
                               });
  return it == document.materials.end() ? nullptr : &*it;
}

std::string QtTrim(std::string_view text) {
  const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
    return std::isspace(c) != 0;
  }).base();
  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

std::vector<std::string> QtSplitPropertyPath(std::string_view id) {
  std::vector<std::string> parts;
  std::size_t start = 0u;
  while (start <= id.size()) {
    const std::size_t dot = id.find('.', start);
    const std::size_t end = dot == std::string_view::npos ? id.size() : dot;
    parts.push_back(std::string(id.substr(start, end - start)));
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1u;
  }
  return parts;
}

bool QtParseFloat(std::string_view text, float& out) {
  std::istringstream in(QtTrim(text));
  float value = 0.0f;
  in >> value;
  if (!in || !std::isfinite(value)) {
    return false;
  }
  out = value;
  return true;
}

bool QtParseStableId(std::string_view text, vkpt::core::StableId& out) {
  const std::string trimmed = QtTrim(text);
  if (trimmed.empty() || trimmed == "none") {
    out = 0u;
    return true;
  }
  const auto hash = trimmed.find('#');
  if (hash != std::string::npos) {
    std::size_t digitStart = hash + 1u;
    std::size_t digitEnd = digitStart;
    while (digitEnd < trimmed.size() &&
           std::isdigit(static_cast<unsigned char>(trimmed[digitEnd]))) {
      ++digitEnd;
    }
    if (digitEnd == digitStart) {
      return false;
    }
    try {
      std::size_t consumed = 0u;
      const std::string idText = trimmed.substr(digitStart, digitEnd - digitStart);
      const auto value = std::stoull(idText, &consumed, 10);
      if (consumed != idText.size()) {
        return false;
      }
      out = static_cast<vkpt::core::StableId>(value);
      return true;
    } catch (...) {
      return false;
    }
  }
  try {
    std::size_t consumed = 0u;
    const auto value = std::stoull(trimmed, &consumed, 10);
    if (consumed != trimmed.size()) {
      return false;
    }
    out = static_cast<vkpt::core::StableId>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool QtParseBool(std::string_view text, bool& out) {
  std::string value = QtTrim(text);
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (value == "true" || value == "1" || value == "yes" || value == "on") {
    out = true;
    return true;
  }
  if (value == "false" || value == "0" || value == "no" || value == "off") {
    out = false;
    return true;
  }
  return false;
}

bool QtParseVec3(std::string_view text, vkpt::scene::Vec3& out) {
  std::string normalized(text);
  for (auto& c : normalized) {
    if (c == ',' || c == '(' || c == ')' || c == '[' || c == ']') {
      c = ' ';
    }
  }
  std::istringstream in(normalized);
  vkpt::scene::Vec3 value{};
  in >> value.x >> value.y >> value.z;
  if (!in || !std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) {
    return false;
  }
  out = value;
  return true;
}

bool QtParseQuat(std::string_view text, vkpt::scene::Quat& out) {
  std::string normalized(text);
  for (auto& c : normalized) {
    if (c == ',' || c == '(' || c == ')' || c == '[' || c == ']') {
      c = ' ';
    }
  }
  std::istringstream in(normalized);
  vkpt::scene::Quat value{};
  in >> value.x >> value.y >> value.z >> value.w;
  if (!in || !std::isfinite(value.x) || !std::isfinite(value.y) ||
      !std::isfinite(value.z) || !std::isfinite(value.w)) {
    return false;
  }
  const float lenSq = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
  if (lenSq <= 1.0e-8f) {
    return false;
  }
  const float invLen = 1.0f / std::sqrt(lenSq);
  value.x *= invLen;
  value.y *= invLen;
  value.z *= invLen;
  value.w *= invLen;
  out = value;
  return true;
}

std::string QtEntityComponentSummary(const vkpt::scene::SceneEntityDefinition& entity) {
  std::vector<std::string_view> parts;
  if (entity.has_transform) {
    parts.push_back("Transform");
  }
  if (entity.has_mesh) {
    parts.push_back("Mesh");
  }
  if (entity.has_sdf_primitive) {
    parts.push_back("SDF");
  }
  if (entity.has_light) {
    parts.push_back("Light");
  }
  if (entity.has_camera) {
    parts.push_back("Camera");
  }
  if (!entity.script.script.empty()) {
    parts.push_back("Script");
  }
  if (!entity.animation.clip.empty()) {
    parts.push_back("Animation");
  }
  if (entity.has_physics_body) {
    parts.push_back(entity.physics_body.enabled ? "Physics" : "Physics Off");
  }
  if (entity.has_benchmark_tag && entity.benchmark_tag.enabled) {
    parts.push_back("Benchmark");
  }
  if (parts.empty()) {
    return "Identity";
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0u) {
      out << ", ";
    }
    out << parts[i];
  }
  return out.str();
}

std::string QtEntityDisplayName(const vkpt::scene::SceneEntityDefinition& entity) {
  if (!entity.name.empty()) {
    return entity.name;
  }
  return std::string("Entity ") + std::to_string(entity.id);
}

vkpt::core::StableId QtPrimarySelectionId(const vkpt::editor::SelectionState& selection) {
  if (selection.active_primary_entity != 0u) {
    return selection.active_primary_entity;
  }
  if (!selection.selected_entity_ids.empty()) {
    return selection.selected_entity_ids.front();
  }
  return 0u;
}

const vkpt::editor::Bounds* FindQtSelectionBounds(
    const vkpt::editor::SelectionState& selection,
    vkpt::core::StableId entity_id) {
  const auto it = std::find_if(selection.per_item_bounds.begin(),
                               selection.per_item_bounds.end(),
                               [entity_id](const vkpt::editor::SceneEntityBounds& item) {
                                 return item.entity_id == entity_id;
                               });
  return it == selection.per_item_bounds.end() ? nullptr : &it->bounds;
}

void QtDockLimitRows(QtDockPanelContent& panel, std::size_t max_rows) {
  if (panel.rows.size() <= max_rows) {
    return;
  }
  const std::size_t hidden = panel.rows.size() - max_rows;
  panel.rows.resize(max_rows);
  panel.rows.push_back("... " + std::to_string(hidden) + " more");
}

QtDockPanelContent BuildQtSceneTreeDock(const vkpt::scene::SceneDocument& document,
                                        const vkpt::editor::SelectionState& selection,
                                        const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "scene_graph", "Scene Graph", true, 280.0f, 600.0f);
  QtDockAddProperty(panel, "entities", std::to_string(document.entities.size()));
  QtDockAddProperty(panel, "geometry", std::to_string(document.geometry.size()));
  QtDockAddProperty(panel, "sdf primitives", std::to_string(document.sdf_primitives.size()));

  const auto is_selected = [&](vkpt::core::StableId id) {
    return std::find(selection.selected_entity_ids.begin(),
                     selection.selected_entity_ids.end(),
                     id) != selection.selected_entity_ids.end();
  };

  const auto entity_icon = [](const vkpt::scene::SceneEntityDefinition& entity) {
    if (entity.has_camera) {
      return std::string("camera");
    }
    if (entity.has_light) {
      return std::string("light");
    }
    if (entity.has_mesh) {
      return std::string("model");
    }
    if (entity.has_physics_body) {
      return std::string("physics");
    }
    if (!entity.script.script.empty()) {
      return std::string("script");
    }
    if (!entity.animation.clip.empty()) {
      return std::string("animation");
    }
    return std::string("entity");
  };

  std::unordered_map<vkpt::core::StableId, std::vector<const vkpt::scene::SceneEntityDefinition*>> children;
  std::unordered_set<vkpt::core::StableId> entityIds;
  for (const auto& entity : document.entities) {
    entityIds.insert(entity.id);
    children[entity.hierarchy.parent].push_back(&entity);
  }

  std::unordered_set<vkpt::core::StableId> visited;
  auto build_entity_row = [&](auto&& self,
                              const vkpt::scene::SceneEntityDefinition& entity) -> QtDockTreeRow {
    visited.insert(entity.id);

    QtDockTreeRow row;
    row.id = "entity." + std::to_string(entity.id);
    row.label = QtEntityDisplayName(entity);
    row.value = "#" + std::to_string(entity.id) + "  " + QtEntityComponentSummary(entity);
    if (entity.hierarchy.parent != 0u && !entityIds.contains(entity.hierarchy.parent)) {
      row.value += "  missing parent #" + std::to_string(entity.hierarchy.parent);
    }
    row.icon = entity_icon(entity);
    row.entity_id = entity.id;
    row.selected = is_selected(entity.id);

    if (const auto childIt = children.find(entity.id); childIt != children.end()) {
      for (const auto* child : childIt->second) {
        if (child == nullptr || visited.contains(child->id)) {
          continue;
        }
        row.children.push_back(self(self, *child));
      }
    }
    return row;
  };

  for (const auto& entity : document.entities) {
    if (visited.contains(entity.id)) {
      continue;
    }
    if (entity.hierarchy.parent == 0u || !entityIds.contains(entity.hierarchy.parent)) {
      QtDockAddTreeRow(panel, build_entity_row(build_entity_row, entity));
    }
  }
  for (const auto& entity : document.entities) {
    if (!visited.contains(entity.id)) {
      QtDockAddTreeRow(panel, build_entity_row(build_entity_row, entity));
    }
  }

  if (!document.sdf_primitives.empty()) {
    QtDockTreeRow sdfGroup;
    sdfGroup.id = "sdf_primitives";
    sdfGroup.label = "SDF Primitives";
    sdfGroup.value = std::to_string(document.sdf_primitives.size()) + " authored";
    sdfGroup.icon = "group";
    for (const auto& primitive : document.sdf_primitives) {
      const std::string shape = primitive.shape.empty()
          ? (primitive.primitive.shape.empty() ? std::string("sphere") : primitive.primitive.shape)
          : primitive.shape;
      QtDockTreeRow row;
      row.id = "sdf." + std::to_string(primitive.id);
      row.label = "SDF " + shape;
      row.value = "#" + std::to_string(primitive.id);
      row.icon = "sdf";
      row.entity_id = primitive.id;
      row.selected = is_selected(primitive.id);
      sdfGroup.children.push_back(std::move(row));
    }
    QtDockAddTreeRow(panel, std::move(sdfGroup));
  }

  if (panel.tree_rows.empty()) {
    QtDockAddRow(panel, "No authored entities in document");
  }
  return panel;
}

QtDockPanelContent BuildQtInspectorDock(const vkpt::scene::SceneDocument& document,
                                        const vkpt::editor::SelectionState& selection,
                                        const vkpt::editor::UiRuntimeState& runtime,
                                        const vkpt::editor::UiLayoutDocument& layout) {
  (void)runtime;
  auto panel = MakeQtDockPanel(layout, "inspector", "Inspector", true, 420.0f, 600.0f);
  const auto primaryId = QtPrimarySelectionId(selection);

  if (primaryId == 0u) {
    QtDockAddProperty(panel, "Selection", "No object selected");
    return panel;
  }

  const auto* entity = FindQtSceneEntity(document, primaryId);
  if (entity == nullptr) {
    const auto* primitive = FindQtSceneSdfPrimitive(document, primaryId);
    if (primitive == nullptr) {
      QtDockAddProperty(panel, "Selection", "Selected object is not in the loaded document");
      return panel;
    }

    QtDockAddProperty(panel, "Name", "sdf " + std::to_string(primitive->id));
    QtDockAddProperty(panel, "Type", "SDF Primitive");
    if (const auto* bounds = FindQtSelectionBounds(selection, primaryId)) {
      QtDockAddProperty(panel, "Bounds", QtDockBounds(*bounds));
    }

    const std::string sdfPrefix = "sdf." + std::to_string(primitive->id) + ".";
    QtDockAddInspectorTransformControls(panel,
                                        sdfPrefix + "transform.",
                                        primitive->transform);

    const std::string shape = primitive->shape.empty()
        ? (primitive->primitive.shape.empty() ? std::string("sphere") : primitive->primitive.shape)
        : primitive->shape;
    QtDockAddDropdownGroupedProperty(panel,
                                     sdfPrefix + "shape",
                                     "",
                                     "SDF shape",
                                     shape,
                                     QtSdfShapeOptions());
    QtDockAddSliderGroupedProperty(panel,
                                   sdfPrefix + "primitive.radius",
                                   "",
                                   "SDF radius",
                                   primitive->primitive.radius,
                                   0.01,
                                   10.0,
                                   0.01,
                                   1.0);
    QtDockAddSliderGroupedProperty(panel,
                                   sdfPrefix + "primitive.param_a",
                                   "",
                                   "SDF param A",
                                   primitive->primitive.param_a,
                                   -10.0,
                                   10.0,
                                   0.01,
                                   0.0);
    QtDockAddSliderGroupedProperty(panel,
                                   sdfPrefix + "primitive.param_b",
                                   "",
                                   "SDF param B",
                                   primitive->primitive.param_b,
                                   -10.0,
                                   10.0,
                                   0.01,
                                   0.0);
    return panel;
  }

  QtDockAddEditableGroupedProperty(panel,
                                   "entity." + std::to_string(entity->id) + ".name",
                                   "",
                                   "Name",
                                   QtEntityDisplayName(*entity));
  QtDockAddProperty(panel, "Type", QtEntityComponentSummary(*entity));
  if (const auto* bounds = FindQtSelectionBounds(selection, primaryId)) {
    QtDockAddProperty(panel, "Bounds", QtDockBounds(*bounds));
  }

  const std::string entityPrefix = "entity." + std::to_string(entity->id) + ".";

  if (entity->has_transform) {
    QtDockAddInspectorTransformControls(panel,
                                        entityPrefix + "transform.",
                                        entity->transform);
  }
  if (entity->has_mesh) {
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "mesh.mesh_id",
                                     "",
                                     "Mesh",
                                     QtGeometryDisplayLabel(document, entity->mesh.mesh_id),
                                     QtGeometryIdOptions(document, entity->mesh.mesh_id));
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "mesh.material_id",
                                     "",
                                     "Material",
                                     QtMaterialDisplayLabel(document, entity->mesh.material_id),
                                     QtMaterialIdOptions(document, entity->mesh.material_id));
    if (const auto* material = FindQtSceneMaterial(document, entity->mesh.material_id)) {
      const std::string materialPrefix = "material." + std::to_string(material->id) + ".";
      QtDockAddDropdownGroupedProperty(panel,
                                       materialPrefix + "family",
                                       "",
                                       "Material model",
                                       material->family.empty() ? std::string("diffuse") : material->family,
                                       QtMaterialFamilyOptions());
      QtDockAddVec3Sliders(panel,
                           materialPrefix + "albedo",
                           "",
                           "Base color",
                           material->albedo,
                           vkpt::scene::Vec3{0.8f, 0.8f, 0.8f},
                           0.0,
                           1.0,
                           0.01);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "roughness",
                                     "",
                                     "Roughness",
                                     material->roughness,
                                     0.0,
                                     1.0,
                                     0.01,
                                     0.6);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "metallic",
                                     "",
                                     "Metallic",
                                     material->metallic,
                                     0.0,
                                     1.0,
                                     0.01,
                                     0.0);
      if (material->emission_intensity > 0.0f ||
          material->emission.x > 0.0f ||
          material->emission.y > 0.0f ||
          material->emission.z > 0.0f) {
        QtDockAddSliderGroupedProperty(panel,
                                       materialPrefix + "emission_intensity",
                                       "",
                                       "Emission",
                                       material->emission_intensity,
                                       0.0,
                                       50.0,
                                       0.1,
                                       0.0);
      }
    }
  }
  if (entity->has_sdf_primitive) {
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "sdf_primitive.shape",
                                     "",
                                     "SDF shape",
                                     entity->sdf_primitive.shape.empty()
                                         ? std::string("sphere")
                                         : entity->sdf_primitive.shape,
                                     QtSdfShapeOptions());
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "sdf_primitive.radius",
                                   "",
                                   "SDF radius",
                                   entity->sdf_primitive.radius,
                                   0.01,
                                   10.0,
                                   0.01,
                                   1.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "sdf_primitive.param_a",
                                   "",
                                   "SDF param A",
                                   entity->sdf_primitive.param_a,
                                   -10.0,
                                   10.0,
                                   0.01,
                                   0.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "sdf_primitive.param_b",
                                   "",
                                   "SDF param B",
                                   entity->sdf_primitive.param_b,
                                   -10.0,
                                   10.0,
                                   0.01,
                                   0.0);
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "material.material_id",
                                     "",
                                     "Material",
                                     QtMaterialDisplayLabel(document, entity->material.material_id),
                                     QtMaterialIdOptions(document, entity->material.material_id));
  }
  if (entity->has_light) {
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "light.type",
                                     "",
                                     "Light type",
                                     entity->light.type.empty() ? std::string("point") : entity->light.type,
                                     QtLightTypeOptions());
    QtDockAddVec3Sliders(panel,
                         entityPrefix + "light.color",
                         "",
                         "Light color",
                         entity->light.color,
                         vkpt::scene::Vec3{1.0f, 1.0f, 1.0f},
                         0.0,
                         1.0,
                         0.01);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "light.intensity",
                                   "",
                                   "Light intensity",
                                   entity->light.intensity,
                                   0.0,
                                   100.0,
                                   0.1,
                                   1.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "light.radius",
                                   "",
                                   "Light radius",
                                   entity->light.radius,
                                   0.0,
                                   10.0,
                                   0.01,
                                   0.0);
    if (QtTrim(entity->light.type) == "spot") {
      QtDockAddVec3Sliders(panel,
                           entityPrefix + "light.direction",
                           "",
                           "Spot direction",
                           entity->light.direction,
                           vkpt::scene::Vec3{0.0f, -1.0f, 0.0f},
                           -1.0,
                           1.0,
                           0.01);
      QtDockAddSliderGroupedProperty(panel,
                                     entityPrefix + "light.beam_angle",
                                     "",
                                     "Spot beam",
                                     entity->light.beam_angle_degrees,
                                     1.0,
                                     120.0,
                                     0.5,
                                     35.0);
      QtDockAddSliderGroupedProperty(panel,
                                     entityPrefix + "light.blend",
                                     "",
                                     "Spot edge",
                                     entity->light.blend,
                                     0.0,
                                     1.0,
                                     0.01,
                                     0.35);
    }
  }
  if (entity->has_camera) {
    QtDockAddPrimaryCameraControls(panel, entityPrefix + "camera.", entity->camera);
  }
  QtDockAddDropdownGroupedProperty(panel,
                                   entityPrefix + "physics.enabled",
                                   "",
                                   "Physics",
                                   QtDockBool(entity->has_physics_body && entity->physics_body.enabled),
                                   QtBoolOptions());
  if (entity->has_physics_body) {
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "physics.body_type",
                                     "",
                                     "Body type",
                                     entity->physics_body.dynamic
                                         ? std::string("dynamic")
                                         : entity->physics_body.body_type,
                                     QtPhysicsBodyTypeOptions());
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "physics.shape",
                                     "",
                                     "Collision shape",
                                     entity->physics_body.shape.empty()
                                         ? std::string("box")
                                         : entity->physics_body.shape,
                                     QtPhysicsShapeOptions());
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "physics.mass",
                                   "",
                                   "Mass",
                                   entity->physics_body.mass,
                                   0.01,
                                   1000.0,
                                   0.01,
                                   1.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "physics.friction",
                                   "",
                                   "Friction",
                                   entity->physics_body.friction,
                                   0.0,
                                   2.0,
                                   0.01,
                                   0.5);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "physics.restitution",
                                   "",
                                   "Bounce",
                                   entity->physics_body.restitution,
                                   0.0,
                                   1.0,
                                   0.01,
                                   0.0);
  }
  if (!entity->script.script.empty()) {
    QtDockAddProperty(panel, "Script", entity->script.script);
  }
  return panel;
}

QtDockPanelContent BuildQtMaterialsDock(const vkpt::scene::SceneDocument& document,
                                        const vkpt::pathtracer::RTSceneData& scene,
                                        const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "materials", "Materials", true, 520.0f, 420.0f);
  QtDockAddProperty(panel, "authored materials", std::to_string(document.materials.size()));
  QtDockAddProperty(panel, "runtime materials", std::to_string(scene.materials.size()));
  for (const auto& material : document.materials) {
    std::ostringstream row;
    row << "#" << material.id << " "
        << (material.name.empty() ? "material" : material.name)
        << " albedo=(" << QtDockVec3(material.albedo) << ")"
        << " roughness=" << QtDockNumber(material.roughness, 2);
    if (material.emission_intensity > 0.0f) {
      row << " emissive=" << QtDockNumber(material.emission_intensity, 2);
    }
    QtDockAddRow(panel, row.str());
  }
  if (document.materials.empty()) {
    for (std::size_t i = 0; i < scene.materials.size(); ++i) {
      const auto& material = scene.materials[i];
      std::ostringstream row;
      row << "runtime[" << i << "] albedo=(" << QtDockVec3(material.albedo)
          << ") roughness=" << QtDockNumber(material.roughness, 2)
          << " emissive=" << QtDockBool(material.is_emissive());
      QtDockAddRow(panel, row.str());
    }
  }
  QtDockLimitRows(panel, 96u);
  return panel;
}

QtDockPanelContent BuildQtLightsDock(const vkpt::scene::SceneDocument& document,
                                     const vkpt::pathtracer::RTSceneData& scene,
                                     const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "lights", "Lights", true, 360.0f, 360.0f);
  const auto lightObjectCount = static_cast<std::size_t>(std::count_if(
      document.entities.begin(),
      document.entities.end(),
      [](const vkpt::scene::SceneEntityDefinition& entity) {
        return entity.has_light;
      })) + document.lights.size();
  QtDockAddProperty(panel, "light objects", std::to_string(lightObjectCount));
  if (!document.lights.empty()) {
    QtDockAddProperty(panel, "legacy lights", std::to_string(document.lights.size()));
  }
  QtDockAddProperty(panel, "runtime lights", std::to_string(scene.lights.size()));
  for (const auto& entity : document.entities) {
    if (!entity.has_light) {
      continue;
    }
    std::ostringstream row;
    row << QtEntityDisplayName(entity) << " #" << entity.id
        << " " << entity.light.type
        << " intensity=" << QtDockNumber(entity.light.intensity, 2)
        << " color=(" << QtDockVec3(entity.light.color) << ")";
    QtDockAddRow(panel, row.str());
  }
  for (std::size_t i = 0; i < scene.lights.size(); ++i) {
    const auto& light = scene.lights[i];
    std::ostringstream row;
    row << "runtime[" << i << "] pos=(" << QtDockVec3(light.position)
        << ") intensity=" << QtDockNumber(light.intensity, 2)
        << " radius=" << QtDockNumber(light.radius, 2);
    QtDockAddRow(panel, row.str());
  }
  if (panel.rows.empty()) {
    QtDockAddRow(panel, "No lights in the loaded document or render scene");
  }
  return panel;
}

QtDockPanelContent BuildQtCameraDock(const vkpt::scene::SceneDocument& document,
                                     const vkpt::pathtracer::RTSceneData& scene,
                                     const vkpt::editor::UiRuntimeState& runtime,
                                     const vkpt::editor::UiLayoutDocument& layout,
                                     const QtDockFrameStats& frame_stats,
                                     int active_shot_slot,
                                     const std::array<bool, 4>& saved_shot_slots) {
  auto panel = MakeQtDockPanel(layout, "camera", "Camera", true, 420.0f, 560.0f);
  QtDockAddProperty(panel, "Active", runtime.active_camera.empty() ? "runtime camera" : runtime.active_camera);
  QtDockAddProperty(panel, "Mode", frame_stats.camera_mode.empty() ? "authored" : frame_stats.camera_mode);
  QtDockAddProperty(panel, "Runtime focus", QtDockNumber(scene.camera_focus_distance, 2));
  QtDockAddButtonGroupedProperty(panel,
                                 "camera.mode.fps_toggle",
                                 "",
                                 frame_stats.camera_mode == "fps" ? "Exit FPS" : "Enter FPS",
                                 frame_stats.camera_mode == "fps" ? "Exit FPS" : "Enter FPS");
  if (frame_stats.camera_mode == "fps") {
    QtDockAddProperty(panel,
                      "FPS body",
                      frame_stats.fps_player_grounded ? "grounded" : "airborne");
    QtDockAddProperty(panel, "FPS speed", QtDockNumber(frame_stats.fps_player_speed, 2));
    QtDockAddProperty(panel, "FPS eye height", QtDockNumber(frame_stats.fps_player_eye_height, 2));
    QtDockAddProperty(panel, "Run", QtDockBool(frame_stats.fps_player_running));
    QtDockAddProperty(panel, "Crouch", QtDockBool(frame_stats.fps_player_crouching));
  }
  QtDockAddButtonGroupedProperty(panel,
                                 "camera.focus.pick",
                                 "",
                                 "Focus under cursor",
                                 "Focus Under Cursor");
  QtDockAddButtonGroupedProperty(panel,
                                 "camera.focus.selected",
                                 "",
                                 "Focus selected",
                                 "Focus Selected");
  const int clampedShotSlot = std::clamp(active_shot_slot, 0, 3);
  QtDockAddDropdownGroupedProperty(panel,
                                   "camera.shot.slot",
                                   "",
                                   "Shot slot",
                                   std::to_string(clampedShotSlot + 1),
                                   {"1", "2", "3", "4"});
  QtDockAddButtonGroupedProperty(panel,
                                 "camera.shot.save",
                                 "",
                                 "Save shot",
                                 "Save Shot");
  QtDockAddButtonGroupedProperty(panel,
                                 "camera.shot.recall",
                                 "",
                                 "Recall shot",
                                 "Recall Shot");
  std::ostringstream savedSlots;
  for (std::size_t i = 0; i < saved_shot_slots.size(); ++i) {
    if (i > 0u) {
      savedSlots << "  ";
    }
    savedSlots << (i + 1u) << ":" << (saved_shot_slots[i] ? "saved" : "empty");
  }
  QtDockAddProperty(panel, "Saved shots", savedSlots.str());
  QtDockAddProperty(panel, "Viewport tool", vkpt::editor::ToString(runtime.active_viewport_tool));
  bool addedCameraControls = false;
  for (const auto& entity : document.entities) {
    if (entity.has_camera) {
      QtDockAddProperty(panel, "Editing", QtEntityDisplayName(entity) + " #" + std::to_string(entity.id));
      QtDockAddPrimaryCameraControls(panel,
                                     "entity." + std::to_string(entity.id) + ".camera.",
                                     entity.camera);
      addedCameraControls = true;
      break;
    }
  }
  if (!addedCameraControls) {
    QtDockAddProperty(panel, "Lens", QtDockNumber(scene.camera_focal_length_mm, 1) + " mm");
    QtDockAddProperty(panel, "Aperture", QtDockNumber(scene.camera_aperture_radius, 3));
    QtDockAddProperty(panel, "Exposure", QtDockNumber(scene.camera_exposure_compensation, 2) + " EV");
    QtDockAddProperty(panel, "White balance", QtDockNumber(scene.camera_white_balance_kelvin, 0) + " K");
  }
  return panel;
}

QtDockPanelContent BuildQtRenderSettingsDock(const vkpt::pathtracer::RTSceneData& scene,
                                             const vkpt::pathtracer::RenderSettings& settings,
                                             const vkpt::editor::UiRuntimeState& runtime,
                                             const vkpt::editor::UiLayoutDocument& layout,
                                             const QtDockFrameStats& frame_stats) {
  (void)scene;
  auto panel = MakeQtDockPanel(layout, "render_settings", "Render Settings", true, 420.0f, 460.0f);
  const std::string sceneName = runtime.active_scene.empty() ? "builtin:preview" : runtime.active_scene;
  const std::string renderState = frame_stats.render_mode.empty()
      ? (frame_stats.tracer_ready ? "path tracing on" : "renderer not ready")
      : frame_stats.render_mode;
  QtDockAddProperty(panel, "Scene", sceneName);
  QtDockAddProperty(panel, "Backend", runtime.active_renderer_backend.empty() ? "unknown" : runtime.active_renderer_backend);
  QtDockAddProperty(panel, "Render resolution",
                    std::to_string(settings.width) + "x" + std::to_string(settings.height));
  QtDockAddSliderGroupedProperty(panel,
                                 "render.resolution.width",
                                 "Resolution",
                                 "Render width",
                                 settings.width,
                                 16.0,
                                 8192.0,
                                 1.0,
                                 320.0);
  QtDockAddSliderGroupedProperty(panel,
                                 "render.resolution.height",
                                 "Resolution",
                                 "Render height",
                                 settings.height,
                                 16.0,
                                 8192.0,
                                 1.0,
                                 240.0);
  QtDockAddProperty(panel, "Published framebuffer",
                    std::to_string(frame_stats.frame_width) + "x" +
                    std::to_string(frame_stats.frame_height));
  if (frame_stats.canvas_width > 0u && frame_stats.canvas_height > 0u) {
    QtDockAddProperty(panel, "Viewport canvas",
                      std::to_string(frame_stats.canvas_width) + "x" +
                      std::to_string(frame_stats.canvas_height));
  }
  if (frame_stats.displayed_image_width > 0u && frame_stats.displayed_image_height > 0u) {
    QtDockAddProperty(panel, "Displayed image",
                      std::to_string(frame_stats.displayed_image_width) + "x" +
                      std::to_string(frame_stats.displayed_image_height));
  }
  QtDockAddProperty(panel, "Accumulation",
                    std::to_string(frame_stats.sample_count) + " spp, " + renderState);
  QtDockAddToggleGroupedProperty(panel,
                                 "render.denoiser",
                                 "",
                                 "GPU denoiser",
                                 settings.enable_denoiser);
  QtDockAddToggleGroupedProperty(panel,
                                 "render.temporal_aa",
                                 "",
                                 "Temporal AA",
                                 settings.enable_temporal_aa);
  QtDockAddSliderProperty(panel,
                          "render.max_depth",
                          "Max depth",
                          settings.max_depth,
                          1.0,
                          64.0,
                          1.0,
                          6.0);
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.nee",
                                   "",
                                   "Direct lighting",
                                   QtDockBool(settings.enable_nee),
                                   QtBoolOptions());
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.mis",
                                   "",
                                   "MIS",
                                   QtDockBool(settings.enable_mis),
                                   QtBoolOptions());
  QtDockAddSliderProperty(panel,
                          "render.film.exposure",
                          "Exposure",
                          settings.film_resolve.exposure,
                          0.0,
                          8.0,
                          0.01,
                          1.0);
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.film.tone_map",
                                   "",
                                   "Tone mapper",
                                   QtToneMapName(settings.film_resolve.tone_map),
                                   QtToneMapOptions());
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.film.output_transform",
                                   "",
                                   "Display transform",
                                   QtOutputTransformName(settings.film_resolve.output_transform),
                                   QtOutputTransformOptions());
  QtDockAddSliderProperty(panel,
                          "render.film.gamma",
                          "Gamma",
                          settings.film_resolve.gamma,
                          0.1,
                          4.0,
                          0.01,
                          2.2);
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.film.clamp_output",
                                   "",
                                   "Clamp output",
                                   QtDockBool(settings.film_resolve.clamp_output),
                                   QtBoolOptions());
  return panel;
}

QtDockPanelContent BuildQtBenchmarkDock(const vkpt::editor::BenchmarkPanelModel& benchmark,
                                        const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "benchmark_panel", "Benchmark", false, 560.0f, 480.0f);
  QtDockAddProperty(panel, "scene", benchmark.run_desc.scene_path);
  QtDockAddProperty(panel, "backend", benchmark.run_desc.backend);
  QtDockAddProperty(panel, "renderer", benchmark.run_desc.renderer_path);
  QtDockAddProperty(panel, "resolution", std::to_string(benchmark.run_desc.resolution.width) +
      "x" + std::to_string(benchmark.run_desc.resolution.height));
  QtDockAddProperty(panel, "spp", std::to_string(benchmark.run_desc.samples_per_pixel));
  QtDockAddProperty(panel, "max depth", std::to_string(benchmark.run_desc.max_depth));
  QtDockAddProperty(panel, "can run", QtDockBool(benchmark.can_run));
  QtDockAddProperty(panel, "summary", benchmark.result_summary);
  QtDockAddProperty(panel, "score", QtDockNumber(benchmark.score.normalized_score, 3));
  QtDockAddProperty(panel, "confidence", benchmark.score.confidence);
  for (const auto& action : benchmark.calibration_actions) {
    QtDockAddRow(panel, action.label + " [" + (action.supported ? "available" : action.unavailable_reason) + "]");
  }
  return panel;
}

QtDockPanelContent BuildQtDiagnosticsDock(const vkpt::editor::UiRuntimeState& runtime,
                                          const vkpt::editor::SelectionState& selection,
                                          const vkpt::editor::UiLayoutDocument& layout,
                                          const QtDockFrameStats& frame_stats) {
  auto panel = MakeQtDockPanel(layout, "diagnostics", "Diagnostics", true, 720.0f, 260.0f);
  QtDockAddProperty(panel, "status", runtime.status_message);
  QtDockAddProperty(panel, "last warning/error", runtime.last_warning_or_error);
  QtDockAddProperty(panel, "last menu action", runtime.last_menu_action);
  QtDockAddProperty(panel, "last clicked entity", std::to_string(runtime.last_clicked_entity));
  QtDockAddProperty(panel, "focused panel", runtime.focused_panel);
  QtDockAddProperty(panel, "active modal", runtime.active_modal.empty() ? "none" : runtime.active_modal);
  QtDockAddProperty(panel, "tracer ready", QtDockBool(frame_stats.tracer_ready));
  QtDockAddProperty(panel, "preview status", frame_stats.preview_status);
  QtDockAddProperty(panel, "selection source", vkpt::editor::ToString(selection.selection_source));
  return panel;
}

QtDockPanelContent BuildQtPerformanceDock(const vkpt::editor::UiRuntimeState& runtime,
                                          const vkpt::editor::UiLayoutDocument& layout,
                                          const QtDockFrameStats& frame_stats) {
  auto panel = MakeQtDockPanel(layout, "performance", "Performance", true, 360.0f, 320.0f);
  QtDockAddProperty(panel, "ui fps", QtDockNumber(runtime.fps, 1));
  QtDockAddProperty(panel, "ui frame ms", QtDockNumber(runtime.frame_ms, 2));
  QtDockAddProperty(panel, "samples", std::to_string(frame_stats.sample_count));
  QtDockAddProperty(panel, "published", std::to_string(frame_stats.render_published));
  QtDockAddProperty(panel, "render dropped", std::to_string(frame_stats.render_dropped));
  QtDockAddProperty(panel, "window received", std::to_string(frame_stats.window_received));
  QtDockAddProperty(panel, "window presented", std::to_string(frame_stats.window_presented));
  QtDockAddProperty(panel, "window dropped", std::to_string(frame_stats.window_dropped));
  QtDockAddProperty(panel, "publish cap", frame_stats.publish_cap.empty()
      ? std::to_string(frame_stats.preview_publish_hz) + " fps"
      : frame_stats.publish_cap);
  QtDockAddProperty(panel, "gpu batches/tick", frame_stats.background_thread
      ? std::string("background")
      : std::to_string(frame_stats.gpu_batches_per_tick));
  QtDockAddProperty(panel, "gpu batch ms", QtDockNumber(frame_stats.gpu_batch_ms, 3));
  QtDockAddProperty(panel, "background jobs", std::to_string(runtime.background_job_count));
  return panel;
}

QtDockPanelContent BuildQtDeviceDock(const vkpt::pathtracer::RTSceneData& scene,
                                     const vkpt::editor::UiRuntimeState& runtime,
                                     const vkpt::editor::UiLayoutDocument& layout,
                                     const QtDockFrameStats& frame_stats,
                                     const QtDockDeviceStats& device_stats) {
  auto panel = MakeQtDockPanel(layout, "device", "Device", true, 460.0f, 360.0f);
  const auto& caps = device_stats.backend_caps;
  const auto selectedBackend = device_stats.selected_backend.empty()
      ? runtime.active_renderer_backend
      : device_stats.selected_backend;
  const auto rendererPath = device_stats.active_renderer_path.empty()
      ? runtime.active_renderer_path
      : device_stats.active_renderer_path;

  const vkpt::render::AcceleratorCapabilities* selectedAccel =
      device_stats.has_selected_accelerator ? &device_stats.selected_accelerator : nullptr;
  const auto& selectedBudget = selectedAccel != nullptr
      ? selectedAccel->backend_caps.memory_budget
      : caps.memory_budget;
  const std::uint64_t usage = selectedAccel != nullptr && selectedAccel->current_usage_bytes > 0u
      ? selectedAccel->current_usage_bytes
      : selectedBudget.current_usage_bytes;
  const std::uint64_t budget = selectedAccel != nullptr && selectedAccel->current_budget_bytes > 0u
      ? selectedAccel->current_budget_bytes
      : selectedBudget.current_budget_bytes;
  const std::uint64_t dedicatedMemory = selectedAccel != nullptr && selectedAccel->dedicated_video_memory_bytes > 0u
      ? selectedAccel->dedicated_video_memory_bytes
      : selectedBudget.dedicated_video_memory_bytes;
  const std::uint64_t sharedMemory = selectedAccel != nullptr && selectedAccel->shared_system_memory_bytes > 0u
      ? selectedAccel->shared_system_memory_bytes
      : selectedBudget.shared_system_memory_bytes;
  const std::string deviceName = selectedAccel != nullptr
      ? selectedAccel->name
      : (caps.platform.platform_name.empty() ? std::string("unknown") : caps.platform.platform_name);
  const std::string deviceKind = selectedAccel != nullptr
      ? QtDockAcceleratorKind(*selectedAccel)
      : (caps.is_simulated ? std::string("Simulated") : std::string("Unknown"));

  const std::string backendValue = !rendererPath.empty() && rendererPath != selectedBackend
      ? selectedBackend + " / " + rendererPath
      : selectedBackend;
  QtDockAddProperty(panel, "Backend", backendValue);
  QtDockAddProperty(panel, "Render mode",
                    frame_stats.background_thread ? "background render thread" : "event loop renderer");

  std::ostringstream throughput;
  const double computerAverage = frame_stats.accumulated_rays_per_second > 0.0
      ? frame_stats.accumulated_rays_per_second
      : frame_stats.rolling_rays_per_second;
  throughput << "computer avg " << QtDockRate(computerAverage);
  if (frame_stats.rolling_rays_per_second > 0.0) {
    throughput << " | rolling " << QtDockRate(frame_stats.rolling_rays_per_second);
  }
  if (frame_stats.sample_count > 0u) {
    throughput << " @ " << frame_stats.sample_count << " spp";
  }
  QtDockAddProperty(panel, "Ray throughput", throughput.str());

  const auto activeMetricKey = selectedAccel != nullptr
      ? QtDockRayDeviceKeyForAccelerator(*selectedAccel)
      : QtDockActiveRayDeviceKey(device_stats);
  const auto* activeMetric = QtDockFindRayMetric(device_stats.ray_metrics, activeMetricKey);
  if (selectedAccel != nullptr) {
    QtDockAddProperty(panel, "Active ray device",
                      QtDockAcceleratorSummary(*selectedAccel, true, activeMetric));
  } else {
    std::vector<std::string> tags;
    tags.push_back(deviceKind);
    QtDockAppendRayMetricTags(tags, activeMetric);
    const auto tagText = QtDockJoin(tags);
    QtDockAddProperty(panel,
                      "Active ray device",
                      tagText.empty() ? deviceName : deviceName + "\n" + tagText);
  }

  std::vector<const vkpt::render::AcceleratorCapabilities*> gpuCandidates;
  std::vector<const vkpt::render::AcceleratorCapabilities*> cpuFallbacks;
  std::vector<const vkpt::render::AcceleratorCapabilities*> softwareFallbacks;
  for (const auto& accelerator : device_stats.accelerators) {
    if (!QtDockRayDeviceEligible(accelerator)) {
      continue;
    }
    if (selectedAccel != nullptr && QtDockSameAccelerator(accelerator, *selectedAccel)) {
      continue;
    }
    if (accelerator.cpu) {
      cpuFallbacks.push_back(&accelerator);
    } else if (accelerator.warp) {
      softwareFallbacks.push_back(&accelerator);
    } else {
      gpuCandidates.push_back(&accelerator);
    }
  }

  if (!gpuCandidates.empty()) {
    QtDockAddProperty(panel, "Other GPUs",
                      QtDockAcceleratorGroupSummary(gpuCandidates, device_stats.ray_metrics));
  }
  if (!cpuFallbacks.empty()) {
    QtDockAddProperty(panel, "CPU fallback",
                      QtDockAcceleratorGroupSummary(cpuFallbacks, device_stats.ray_metrics, 2u));
  }
  if (!softwareFallbacks.empty()) {
    QtDockAddProperty(panel, "Software fallback",
                      QtDockAcceleratorGroupSummary(softwareFallbacks, device_stats.ray_metrics, 2u));
  }

  QtDockAddProperty(panel, "Device memory",
                    QtDockMemoryUsage(usage, budget, selectedBudget.budget_unavailable_reason));
  if (dedicatedMemory > 0u || sharedMemory > 0u) {
    std::vector<std::string> memoryParts;
    if (dedicatedMemory > 0u) {
      memoryParts.push_back("dedicated " + QtDockBytes(dedicatedMemory));
    }
    if (sharedMemory > 0u &&
        (selectedAccel == nullptr || selectedAccel->unified_memory || dedicatedMemory == 0u)) {
      memoryParts.push_back("shared " + QtDockBytes(sharedMemory));
    }
    const auto memoryText = QtDockJoin(memoryParts);
    if (!memoryText.empty()) {
      QtDockAddProperty(panel, "Memory pool", memoryText);
    }
  }
  QtDockAddProperty(panel, "Scene buffers", QtDockBytes(EstimateQtSceneMemoryBytes(scene)));
  const auto featureSummary = QtDockFeatureSummary(caps);
  if (!featureSummary.empty() && featureSummary != "basic") {
    QtDockAddProperty(panel, "Capabilities", featureSummary);
  }
  return panel;
}

QtDockPanelContent BuildQtDebugViewsDock(const vkpt::editor::UiRuntimeState& runtime,
                                         const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "debug_views", "Debug Views", false, 320.0f, 300.0f);
  QtDockAddProperty(panel, "selected view", runtime.selected_debug_view.empty() ? "beauty" : runtime.selected_debug_view);
  QtDockAddProperty(panel, "active channel", runtime.active_debug_channel.empty() ? "rgb" : runtime.active_debug_channel);
  QtDockAddRow(panel, "beauty");
  QtDockAddRow(panel, "albedo");
  QtDockAddRow(panel, "normal");
  QtDockAddRow(panel, "depth");
  QtDockAddRow(panel, "sample_count");
  QtDockAddRow(panel, "selection_id");
  return panel;
}

QtDockPanelContent BuildQtAssetBrowserDock(const vkpt::scene::SceneDocument& document,
                                           const vkpt::pathtracer::RTSceneData& scene,
                                           const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "asset_browser", "Asset Browser", true, 720.0f, 260.0f);
  QtDockAddProperty(panel, "assets", std::to_string(document.assets.size()));
  QtDockAddProperty(panel, "geometry", std::to_string(document.geometry.size()));
  QtDockAddProperty(panel, "textures", std::to_string(scene.textures.size()));
  for (const auto& asset : document.assets) {
    QtDockAddRow(panel, "#" + std::to_string(asset.id) + " " + asset.type + " " + asset.uri);
  }
  for (const auto& geometry : document.geometry) {
    QtDockAddRow(panel, "#" + std::to_string(geometry.id) + " geometry " +
        (geometry.primitive.empty() ? "mesh" : geometry.primitive) +
        " vertices=" + std::to_string(geometry.vertices.size()) +
        " indices=" + std::to_string(geometry.indices.size()));
  }
  for (const auto& texture : scene.textures) {
    QtDockAddRow(panel, "texture " + texture);
  }
  if (panel.rows.empty()) {
    QtDockAddRow(panel, "No external assets referenced by this scene");
  }
  QtDockLimitRows(panel, 128u);
  return panel;
}

QtDockPanelContent BuildQtTimelineDock(const vkpt::scene::SceneDocument& document,
                                       const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "timeline", "Timeline", false, 560.0f, 220.0f);
  std::size_t animated = 0u;
  for (const auto& entity : document.entities) {
    if (!entity.animation.clip.empty()) {
      ++animated;
      QtDockAddRow(panel, QtEntityDisplayName(entity) + " clip=" + entity.animation.clip +
          " duration=" + QtDockNumber(entity.animation.duration_seconds, 2) + "s" +
          " speed=" + QtDockNumber(entity.animation.playback_speed, 2) +
          (entity.animation.looping ? " loop" : " once"));
    }
  }
  QtDockAddProperty(panel, "animated entities", std::to_string(animated));
  if (animated == 0u) {
    QtDockAddRow(panel, "No animation clips in the loaded document");
  }
  return panel;
}

QtDockPanelContent BuildQtScriptDock(const vkpt::scene::SceneDocument& document,
                                     const vkpt::editor::UiLayoutDocument& layout,
                                     const QtDockScriptRuntimeState* runtime = nullptr) {
  auto panel = MakeQtDockPanel(layout, "script_panel", "Scripting", true, 560.0f, 460.0f);
  std::size_t scripted = 0u;
  for (const auto& entity : document.entities) {
    if (!entity.script.script.empty()) {
      ++scripted;
    }
  }
  QtDockAddToggleGroupedProperty(panel,
                                 "script.runtime.enabled",
                                 "Runtime",
                                 "Scripts enabled",
                                 runtime == nullptr || runtime->scripts_enabled);
  QtDockAddToggleGroupedProperty(panel,
                                 "script.runtime.playing",
                                 "Runtime",
                                 "Playing",
                                 runtime != nullptr && runtime->playing);
  QtDockAddButtonGroupedProperty(panel, "script.runtime.play", "Controls", "Play", "Play");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.pause", "Controls", "Pause", "Pause");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.step", "Controls", "Step update", "Step");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.reload", "Controls", "Reload bindings", "Reload");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_on_load", "Hooks", "on_load", "Fire");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_fixed_update", "Hooks", "on_fixed_update", "Fire");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_late_update", "Hooks", "on_late_update", "Fire");

  QtDockAddProperty(panel, "authored script entities", std::to_string(scripted));
  if (runtime != nullptr) {
    QtDockAddProperty(panel, "runtime status", runtime->status);
    QtDockAddProperty(panel, "lua compiled", QtDockBool(runtime->binding_summary.lua_compiled_in));
    QtDockAddProperty(panel, "execution available", QtDockBool(runtime->binding_summary.execution_available));
    QtDockAddProperty(panel, "bindings", std::to_string(runtime->binding_summary.binding_count));
    QtDockAddProperty(panel, "runnable", std::to_string(runtime->binding_summary.runnable_count));
    QtDockAddProperty(panel, "disabled", std::to_string(runtime->binding_summary.disabled_count));
    QtDockAddProperty(panel,
                      "unsupported language",
                      std::to_string(runtime->binding_summary.unsupported_language_count));
    QtDockAddProperty(panel, "last hook", runtime->last_hook);
    QtDockAddProperty(panel, "last frame", std::to_string(runtime->last_frame));
    QtDockAddProperty(panel, "dispatches", std::to_string(runtime->dispatch_count));
    QtDockAddProperty(panel, "last hook calls", std::to_string(runtime->last_dispatch.hook_call_count));
    QtDockAddProperty(panel, "last skipped", std::to_string(runtime->last_dispatch.skipped_count));
    QtDockAddProperty(panel,
                      "last commands",
                      std::to_string(runtime->last_dispatch.command_count_before) + " -> " +
                          std::to_string(runtime->last_dispatch.command_count_after));
    std::size_t index = 0u;
    for (const auto& binding : runtime->bindings) {
      QtDockAddProperty(panel,
                        "binding " + std::to_string(++index),
                        (binding.enabled ? std::string("on ") : std::string("off ")) +
                            binding.entity_name + " #" + std::to_string(binding.entity) +
                            " " + binding.language + ":" + binding.entry +
                            " " + binding.source);
    }
    const std::size_t diagnostic_count = std::min<std::size_t>(runtime->diagnostics.size(), 6u);
    for (std::size_t i = 0; i < diagnostic_count; ++i) {
      const auto& diagnostic = runtime->diagnostics[runtime->diagnostics.size() - diagnostic_count + i];
      QtDockAddProperty(panel,
                        std::string("diagnostic ") + std::to_string(i + 1u),
                        std::string(vkpt::scripting::to_string(diagnostic.severity)) + " " +
                            std::string(vkpt::scripting::to_string(diagnostic.hook)) +
                            " #" + std::to_string(diagnostic.entity) +
                            " " + diagnostic.message);
    }
  }
  if (scripted == 0u) {
    QtDockAddProperty(panel, "bindings", "No scripts attached");
  }
  return panel;
}

QtDockPanelContent BuildQtPhysicsDock(const vkpt::scene::SceneDocument& document,
                                      const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "physics", "Physics", false, 420.0f, 320.0f);
  const auto engine = vkpt::physics::GetCompiledPhysicsEngineInfo();
  std::size_t authored = 0u;
  std::size_t enabled = 0u;
  std::size_t dynamic = 0u;
  QtDockAddProperty(panel, "entities", std::to_string(document.entities.size()));
  QtDockAddProperty(panel, "engine", engine.available ? engine.engine_name : std::string("disabled"));
  for (const auto& entity : document.entities) {
    if (entity.has_physics_body) {
      ++authored;
      if (entity.physics_body.enabled) {
        ++enabled;
        if (entity.physics_body.dynamic) {
          ++dynamic;
        }
      }
    }
    std::ostringstream row;
    row << QtEntityDisplayName(entity) << " #" << entity.id
        << " physics=" << QtDockBool(entity.has_physics_body && entity.physics_body.enabled);
    if (entity.has_physics_body) {
      row << " type=" << (entity.physics_body.dynamic ? "dynamic" : entity.physics_body.body_type)
          << " shape=" << entity.physics_body.shape
          << " mass=" << QtDockNumber(entity.physics_body.mass, 2);
      if (entity.physics_body.trigger) {
        row << " trigger";
      }
    }
    QtDockAddRow(panel, row.str());
  }
  QtDockAddProperty(panel, "authored bodies", std::to_string(authored));
  QtDockAddProperty(panel, "enabled bodies", std::to_string(enabled));
  QtDockAddProperty(panel, "dynamic bodies", std::to_string(dynamic));
  if (document.entities.empty()) {
    QtDockAddRow(panel, "No entities in the loaded document");
  }
  QtDockLimitRows(panel, 128u);
  return panel;
}

std::vector<QtDockPanelContent> BuildQtDockPanels(
    const vkpt::scene::SceneDocument& document,
    const vkpt::pathtracer::RTSceneData& scene,
    const vkpt::pathtracer::RenderSettings& settings,
    const vkpt::editor::UiRuntimeState& runtime,
    const vkpt::editor::SelectionState& selection,
    const vkpt::editor::UiLayoutDocument& layout,
    const vkpt::editor::BenchmarkPanelModel& benchmark,
    const QtDockFrameStats& frame_stats,
    const QtDockDeviceStats& device_stats,
    int active_camera_shot_slot,
    const std::array<bool, 4>& saved_camera_shot_slots,
    const QtDockScriptRuntimeState* script_runtime) {
  std::vector<QtDockPanelContent> panels;
  panels.reserve(15u);
  panels.push_back(BuildQtSceneTreeDock(document, selection, layout));
  panels.push_back(BuildQtInspectorDock(document, selection, runtime, layout));
  panels.push_back(BuildQtMaterialsDock(document, scene, layout));
  panels.push_back(BuildQtLightsDock(document, scene, layout));
  panels.push_back(BuildQtCameraDock(document,
                                     scene,
                                     runtime,
                                     layout,
                                     frame_stats,
                                     active_camera_shot_slot,
                                     saved_camera_shot_slots));
  panels.push_back(BuildQtRenderSettingsDock(scene, settings, runtime, layout, frame_stats));
  panels.push_back(BuildQtBenchmarkDock(benchmark, layout));
  panels.push_back(BuildQtDiagnosticsDock(runtime, selection, layout, frame_stats));
  panels.push_back(BuildQtPerformanceDock(runtime, layout, frame_stats));
  panels.push_back(BuildQtDeviceDock(scene, runtime, layout, frame_stats, device_stats));
  panels.push_back(BuildQtDebugViewsDock(runtime, layout));
  panels.push_back(BuildQtAssetBrowserDock(document, scene, layout));
  panels.push_back(BuildQtTimelineDock(document, layout));
  panels.push_back(BuildQtScriptDock(document, layout, script_runtime));
  panels.push_back(BuildQtPhysicsDock(document, layout));
  return panels;
}

vkpt::platform::QtDockArea QtDockAreaForPanel(std::string_view panel_id) {
  if (panel_id == "scene_graph" || panel_id == "asset_browser") {
    return vkpt::platform::QtDockArea::Left;
  }
  if (panel_id == "diagnostics" ||
      panel_id == "performance" ||
      panel_id == "device" ||
      panel_id == "debug_views" ||
      panel_id == "benchmark_panel" ||
      panel_id == "timeline") {
    return vkpt::platform::QtDockArea::Bottom;
  }
  return vkpt::platform::QtDockArea::Right;
}

vkpt::platform::QtDockRow ToQtPlatformDockRow(const QtDockTreeRow& row) {
  vkpt::platform::QtDockRow out;
  out.id = row.id;
  out.label = row.label;
  out.value = row.value;
  out.icon = row.icon;
  out.entity_id = row.entity_id;
  out.selected = row.selected;
  out.children.reserve(row.children.size());
  for (const auto& child : row.children) {
    out.children.push_back(ToQtPlatformDockRow(child));
  }
  return out;
}

std::vector<vkpt::platform::QtDockPanel> ToQtPlatformDockPanels(
    const std::vector<QtDockPanelContent>& panels) {
  std::vector<vkpt::platform::QtDockPanel> out;
  out.reserve(panels.size());
  for (const auto& panel : panels) {
    vkpt::platform::QtDockPanel dock;
    dock.id = panel.id;
    dock.title = panel.title;
    dock.area = QtDockAreaForPanel(panel.id);
    dock.content = panel.properties.empty()
        ? vkpt::platform::QtDockPanelContent::Tree
        : vkpt::platform::QtDockPanelContent::Properties;
    dock.visible = panel.visible && !panel.collapsed;
    dock.enabled = true;
    dock.closable = true;
    dock.movable = panel.docked;
    dock.floatable = true;
    dock.preferred_width = QtDockPreferredPixels(panel.width);
    dock.preferred_height = QtDockPreferredPixels(panel.height);
    dock.rows = panel.rows;
    dock.tree_rows.reserve(panel.tree_rows.size());
    for (const auto& row : panel.tree_rows) {
      dock.tree_rows.push_back(ToQtPlatformDockRow(row));
    }
    dock.properties.reserve(panel.properties.size());
    for (const auto& property : panel.properties) {
      vkpt::platform::QtDockProperty dockProperty;
      dockProperty.id = property.id;
      dockProperty.group = property.group;
      dockProperty.name = property.label;
      dockProperty.value = property.value;
      dockProperty.unit = property.unit;
      dockProperty.editor = property.editor;
      dockProperty.options = property.options;
      dockProperty.minimum = property.minimum;
      dockProperty.maximum = property.maximum;
      dockProperty.step = property.step;
      dockProperty.default_value = property.default_value;
      dockProperty.has_numeric_range = property.has_numeric_range;
      dockProperty.has_default = property.has_default;
      dockProperty.editable = property.editable;
      dockProperty.enabled = property.enabled;
      dock.properties.push_back(std::move(dockProperty));
    }
    out.push_back(std::move(dock));
  }
  return out;
}

std::string BuildQtStatusBarText(const vkpt::editor::StatusBarModel& status) {
  std::ostringstream out;
  out << "Scene: " << (status.active_scene.empty() ? "none" : status.active_scene)
      << " | Backend: " << status.backend
      << " | Renderer: " << status.renderer_path
      << " | SPP: " << status.spp
      << " | FPS: " << QtDockNumber(status.fps, 1)
      << " | Frame: " << QtDockNumber(status.frame_ms, 2) << " ms"
      << " | Selected: " << status.selected_entity_count
      << " | Tool: " << status.active_tool;
  if (!status.last_warning_or_error.empty()) {
    out << " | " << status.last_warning_or_error;
  }
  return out.str();
}

void ApplyQtDockPanelsToWindow(vkpt::platform::QtWindow* window,
                               const std::vector<QtDockPanelContent>& panels) {
  if (window == nullptr) {
    return;
  }
  window->set_dock_panels(ToQtPlatformDockPanels(panels));
}

template <typename WindowT>
void ApplyQtStatusBarToWindowTyped(WindowT* window, std::string_view status_text) {
  if (window == nullptr) {
    return;
  }
  if constexpr (requires(WindowT& w, std::string_view text) {
                  w.set_status_bar_text(text);
                }) {
    window->set_status_bar_text(status_text);
  } else if constexpr (requires(WindowT& w, const std::string& text) {
                         w.set_status_bar_text(text);
                       }) {
    const std::string text(status_text);
    window->set_status_bar_text(text);
  } else {
    // App-side adapter only: current QtPlatform.h has no status-bar sink yet.
    (void)status_text;
  }
}

void ApplyQtStatusBarToWindow(vkpt::platform::QtWindow* window,
                              std::string_view status_text) {
  ApplyQtStatusBarToWindowTyped(window, status_text);
}

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
