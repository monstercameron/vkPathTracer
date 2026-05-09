#include "cpu/ParallelBvhBuilder.h"
#include "cpu/TiledCpuPathTracer.h"
#include "pathtracer/PathTracerObservability.h"
#include "render/backends/FrameGraph.h"
#include "render/RenderCoordinator.h"
#include "render/HistoryTransition.h"
#include "render/TileScheduler.h"
#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "scene/SceneSnapshot.h"
#include "scene/SnapshotRing.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

vkpt::pathtracer::PathTracerSceneSnapshot MakeScene() {
  vkpt::pathtracer::PathTracerSceneSnapshot scene;
  scene.vertices = {
      {-1.0f, 0.0f, -3.0f},
      {1.0f, 0.0f, -3.0f},
      {0.0f, 1.0f, -3.0f}};
  scene.indices = {0u, 1u, 2u};
  scene.local_vertices = scene.vertices;
  scene.local_indices = scene.indices;
  scene.materials.push_back({});
  scene.instances.push_back(vkpt::pathtracer::RTInstance{
      42u,
      7u,
      0u,
      1u,
      0u,
      vkpt::pathtracer::kRTInstanceFlagDynamicTransform,
      1u,
      0u,
      3u,
      0u,
      3u,
      {0.0f, 0.0f, 0.0f},
      {},
      {1.0f, 1.0f, 1.0f}});
  scene.camera_position = {0.0f, 0.0f, 0.0f};
  scene.camera_target = {0.0f, 0.0f, -1.0f};
  scene.camera_up = {0.0f, 1.0f, 0.0f};
  return scene;
}

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "snapshot_bus_smoke: " << message << "\n";
    return false;
  }
  return true;
}

bool CheckEquivalentPlan(const vkpt::pathtracer::InstanceTransformPlan& lhs,
                         const vkpt::pathtracer::InstanceTransformPlan& rhs,
                         const char* message) {
  if (vkpt::pathtracer::EquivalentInstanceTransformPlans(lhs, rhs)) {
    return true;
  }
  std::cerr << "snapshot_bus_smoke: " << message
            << " lhs={status=" << vkpt::pathtracer::ToString(lhs.status)
            << ", requested=" << lhs.requested_count
            << ", matched=" << lhs.matched_count
            << ", message='" << vkpt::pathtracer::TransformPlanMessage(lhs)
            << "'} rhs={status=" << vkpt::pathtracer::ToString(rhs.status)
            << ", requested=" << rhs.requested_count
            << ", matched=" << rhs.matched_count
            << ", message='" << vkpt::pathtracer::TransformPlanMessage(rhs)
            << "'}\n";
  return false;
}

bool CheckCounter(const std::vector<vkpt::core::metrics::MetricSnapshot>& metrics,
                  const std::string& name,
                  std::uint64_t expected) {
  for (const auto& metric : metrics) {
    if (metric.name == name) {
      return Check(metric.counter_value == expected, name.c_str());
    }
  }
  return Check(false, name.c_str());
}

const vkpt::core::metrics::MetricSnapshot* FindMetric(
    const std::vector<vkpt::core::metrics::MetricSnapshot>& metrics,
    std::string_view name,
    vkpt::core::metrics::Kind kind) {
  for (const auto& metric : metrics) {
    if (metric.name == name && metric.kind == kind) {
      return &metric;
    }
  }
  return nullptr;
}

bool CheckCounterAtLeast(const std::vector<vkpt::core::metrics::MetricSnapshot>& metrics,
                         std::string_view name,
                         std::uint64_t minimum) {
  const auto* metric = FindMetric(metrics, name, vkpt::core::metrics::Kind::CounterKind);
  return Check(metric != nullptr && metric->counter_value >= minimum,
               std::string(name).c_str());
}

bool CheckHistogramAtLeast(const std::vector<vkpt::core::metrics::MetricSnapshot>& metrics,
                           std::string_view name,
                           std::uint64_t minimum_count) {
  const auto* metric = FindMetric(metrics, name, vkpt::core::metrics::Kind::HistogramKind);
  return Check(metric != nullptr && metric->hist.count >= minimum_count,
               std::string(name).c_str());
}

bool CheckGaugeEquals(const std::vector<vkpt::core::metrics::MetricSnapshot>& metrics,
                      std::string_view name,
                      double expected) {
  const auto* metric = FindMetric(metrics, name, vkpt::core::metrics::Kind::GaugeKind);
  return Check(metric != nullptr && std::abs(metric->gauge_value - expected) <= 1.0e-6,
               std::string(name).c_str());
}

bool LogFieldEquals(const vkpt::core::log::LogEvent& event,
                    std::string_view key,
                    std::string_view expected) {
  for (std::uint8_t i = 0; i < event.field_count; ++i) {
    const auto& field = event.fields[i];
    if (std::string_view(field.key) != key) {
      continue;
    }
    if (field.value.kind == vkpt::core::log::FieldValue::Kind::StrInline) {
      return std::string_view(field.value.storage.str_inline) == expected;
    }
    if (field.value.kind == vkpt::core::log::FieldValue::Kind::StrHeap) {
      return field.value.str_heap == expected;
    }
    return false;
  }
  return false;
}

bool LogFieldStringValue(const vkpt::core::log::LogEvent& event,
                         std::string_view key,
                         std::string& out) {
  for (std::uint8_t i = 0; i < event.field_count; ++i) {
    const auto& field = event.fields[i];
    if (std::string_view(field.key) != key) {
      continue;
    }
    if (field.value.kind == vkpt::core::log::FieldValue::Kind::StrInline) {
      out = field.value.storage.str_inline;
      return true;
    }
    if (field.value.kind == vkpt::core::log::FieldValue::Kind::StrHeap) {
      out = field.value.str_heap;
      return true;
    }
    return false;
  }
  return false;
}

bool LogFieldUnsignedValue(const vkpt::core::log::LogEvent& event,
                           std::string_view key,
                           std::uint64_t& out) {
  for (std::uint8_t i = 0; i < event.field_count; ++i) {
    const auto& field = event.fields[i];
    if (std::string_view(field.key) != key) {
      continue;
    }
    if (field.value.kind == vkpt::core::log::FieldValue::Kind::U64) {
      out = field.value.storage.u64;
      return true;
    }
    if (field.value.kind == vkpt::core::log::FieldValue::Kind::I64 &&
        field.value.storage.i64 >= 0) {
      out = static_cast<std::uint64_t>(field.value.storage.i64);
      return true;
    }
    return false;
  }
  return false;
}

bool LogFieldBoolEquals(const vkpt::core::log::LogEvent& event,
                        std::string_view key,
                        bool expected) {
  for (std::uint8_t i = 0; i < event.field_count; ++i) {
    const auto& field = event.fields[i];
    if (std::string_view(field.key) == key &&
        field.value.kind == vkpt::core::log::FieldValue::Kind::Bool) {
      return field.value.storage.b == expected;
    }
  }
  return false;
}

bool LogFieldUnsignedEquals(const vkpt::core::log::LogEvent& event,
                            std::string_view key,
                            std::uint64_t expected) {
  std::uint64_t actual = 0u;
  return LogFieldUnsignedValue(event, key, actual) && actual == expected;
}

bool IsAllowedCpuKernel(std::string_view kernel) {
  return kernel == "scalar" ||
         kernel == "avx2" ||
         kernel == "avx512" ||
         kernel == "neon";
}

bool IsAllowedCpuSimdReason(std::string_view reason) {
  return reason == "cpu_feature" ||
         reason == "forced" ||
         reason == "fallback";
}

bool HasCpuSimdSelectedEvent(
    const std::vector<vkpt::core::log::LogEvent>& events) {
  for (const auto& event : events) {
    if (std::string_view(event.component) != "cpu" ||
        std::string_view(event.event) != "simd_selected") {
      continue;
    }
    std::string kernel;
    std::string reason;
    return Check(LogFieldStringValue(event, "kernel", kernel) &&
                     IsAllowedCpuKernel(kernel),
                 "cpu simd_selected should include an allowed kernel") &&
           Check(LogFieldStringValue(event, "reason", reason) &&
                     IsAllowedCpuSimdReason(reason),
                 "cpu simd_selected should include a selection reason");
  }
  return Check(false, "cpu simd_selected event should be emitted");
}

bool HasBvhBuildCompletedEvent(std::uint64_t prim_count,
                               std::uint64_t worker_count,
                               const std::vector<vkpt::core::log::LogEvent>& events) {
  for (const auto& event : events) {
    if (std::string_view(event.component) != "bvh" ||
        std::string_view(event.event) != "build_completed") {
      continue;
    }
    std::uint64_t nodes = 0u;
    std::uint64_t prims = 0u;
    std::uint64_t workers = 0u;
    if (LogFieldUnsignedValue(event, "node_count", nodes) &&
        LogFieldUnsignedValue(event, "prim_count", prims) &&
        LogFieldUnsignedValue(event, "worker_count", workers) &&
        nodes > 0u &&
        prims == prim_count &&
        workers == worker_count) {
      return true;
    }
  }
  return Check(false, "bvh build_completed event should carry build counts");
}

bool HasCpuEventWithFlow(const std::vector<vkpt::core::log::LogEvent>& events,
                         std::string_view event_name,
                         std::uint64_t flow_id) {
  for (const auto& event : events) {
    if (std::string_view(event.component) == "cpu" &&
        std::string_view(event.event) == event_name &&
        LogFieldUnsignedEquals(event, "flow_id", flow_id)) {
      return true;
    }
  }
  return false;
}

bool HasPathTracerAccumulationResetEvent(
    const std::vector<vkpt::core::log::LogEvent>& events,
    std::string_view reason,
    std::uint64_t gen) {
  for (const auto& event : events) {
    if (std::string_view(event.component) == "pathtracer" &&
        std::string_view(event.event) == "accumulation_reset" &&
        LogFieldEquals(event, "reason", reason) &&
        LogFieldUnsignedEquals(event, "gen", gen) &&
        LogFieldBoolEquals(event, "success", true)) {
      return true;
    }
  }
  return false;
}

bool HasPathTracerSceneDeltaEvent(
    const std::vector<vkpt::core::log::LogEvent>& events,
    std::uint64_t material_count,
    std::uint64_t light_count,
    std::uint64_t instance_count,
    bool environment_changed) {
  for (const auto& event : events) {
    if (std::string_view(event.component) == "pathtracer" &&
        std::string_view(event.event) == "scene_delta_applied" &&
        LogFieldUnsignedEquals(event, "material_count", material_count) &&
        LogFieldUnsignedEquals(event, "light_count", light_count) &&
        LogFieldUnsignedEquals(event, "instance_count", instance_count) &&
        LogFieldBoolEquals(event, "environment_changed", environment_changed)) {
      return true;
    }
  }
  return false;
}

bool HasPathTracerEventWithFlow(
    const std::vector<vkpt::core::log::LogEvent>& events,
    std::string_view event_name,
    std::uint64_t flow_id) {
  for (const auto& event : events) {
    if (std::string_view(event.component) == "pathtracer" &&
        std::string_view(event.event) == event_name &&
        LogFieldUnsignedEquals(event, "flow_id", flow_id)) {
      return true;
    }
  }
  return false;
}

bool HasPathTracerAnomaly(
    const std::vector<vkpt::core::log::LogEvent>& events,
    std::string_view operation) {
  for (const auto& event : events) {
    if (std::string_view(event.component) == "pathtracer" &&
        std::string_view(event.event) == "operation_failed" &&
        LogFieldEquals(event, "operation", operation)) {
      return true;
    }
  }
  return false;
}

bool HasRenderEvent(std::string_view eventName) {
  const auto events = vkpt::core::log::Logger::instance().dump_crash_rings();
  for (const auto& event : events) {
    if (std::string_view(event.component) == "render" &&
        std::string_view(event.event) == eventName) {
      return true;
    }
  }
  return false;
}

bool HasRenderEventWithFlow(std::string_view eventName,
                            std::uint64_t flow_id) {
  const auto events = vkpt::core::log::Logger::instance().dump_crash_rings();
  for (const auto& event : events) {
    if (std::string_view(event.component) == "render" &&
        std::string_view(event.event) == eventName &&
        LogFieldUnsignedEquals(event, "flow_id", flow_id)) {
      return true;
    }
  }
  return false;
}

bool HasRenderDropReason(std::string_view reason) {
  const auto events = vkpt::core::log::Logger::instance().dump_crash_rings();
  for (const auto& event : events) {
    if (std::string_view(event.component) == "render" &&
        std::string_view(event.event) == "frame_dropped" &&
        LogFieldEquals(event, "reason", reason)) {
      return true;
    }
  }
  return false;
}

std::size_t CountCrashEvents(std::string_view component,
                             std::string_view event_name) {
  const auto events = vkpt::core::log::Logger::instance().dump_crash_rings();
  return static_cast<std::size_t>(
      std::count_if(events.begin(), events.end(), [&](const auto& event) {
        return std::string_view(event.component) == component &&
               std::string_view(event.event) == event_name;
      }));
}

bool CloseEnough(float lhs, float rhs) {
  return std::abs(lhs - rhs) <= 1.0e-5f;
}

bool CheckFilmEqual(const vkpt::pathtracer::FilmBuffer& lhs,
                    const vkpt::pathtracer::FilmBuffer& rhs,
                    const char* message) {
  if (!Check(lhs.width() == rhs.width() && lhs.height() == rhs.height(),
             message) ||
      !Check(lhs.sample_counts().size() == rhs.sample_counts().size(),
             message) ||
      !Check(lhs.raw().size() == rhs.raw().size(),
             message)) {
    return false;
  }
  for (std::size_t i = 0u; i < lhs.sample_counts().size(); ++i) {
    if (!Check(lhs.sample_counts()[i] == rhs.sample_counts()[i], message)) {
      return false;
    }
  }
  for (std::size_t i = 0u; i < lhs.raw().size(); ++i) {
    const auto& a = lhs.raw()[i];
    const auto& b = rhs.raw()[i];
    if (!Check(CloseEnough(a.x, b.x) &&
                   CloseEnough(a.y, b.y) &&
                   CloseEnough(a.z, b.z),
               message)) {
      return false;
    }
  }
  return true;
}

vkpt::pathtracer::FilmBuffer MakeFilledFilm(std::uint32_t width,
                                            std::uint32_t height,
                                            std::uint32_t samples) {
  vkpt::pathtracer::FilmBuffer film(width, height);
  for (std::uint32_t y = 0u; y < height; ++y) {
    for (std::uint32_t x = 0u; x < width; ++x) {
      const vkpt::pathtracer::Vec3 color{
          0.25f + static_cast<float>(x) * 0.01f,
          0.35f + static_cast<float>(y) * 0.01f,
          0.5f};
      film.set_pixel_raw(x,
                         y,
                         {color.x * static_cast<float>(samples),
                          color.y * static_cast<float>(samples),
                          color.z * static_cast<float>(samples)},
                         samples);
    }
  }
  return film;
}

bool AllSampleCountsZero(const vkpt::pathtracer::FilmBuffer& film) {
  for (const auto count : film.sample_counts()) {
    if (count != 0u) {
      return false;
    }
  }
  return true;
}

vkpt::pathtracer::PathTracerSceneSnapshot MakeOneMoverScene() {
  vkpt::pathtracer::PathTracerSceneSnapshot scene;
  scene.materials.push_back({});
  scene.materials.push_back({});
  scene.local_vertices = {
      {-1.8f, -1.0f, -4.0f},
      {1.8f, -1.0f, -4.0f},
      {0.0f, 1.7f, -4.0f},
      {-0.18f, -0.12f, -3.0f},
      {0.18f, -0.12f, -3.0f},
      {0.0f, 0.24f, -3.0f}};
  scene.local_indices = {0u, 1u, 2u, 3u, 4u, 5u};
  scene.vertices = scene.local_vertices;
  scene.indices = scene.local_indices;
  scene.instances.push_back(vkpt::pathtracer::RTInstance{
      100u,
      1u,
      0u,
      1u,
      0u,
      vkpt::pathtracer::kRTInstanceFlagNone,
      1u,
      0u,
      3u,
      0u,
      3u,
      {0.0f, 0.0f, 0.0f},
      {},
      {1.0f, 1.0f, 1.0f}});
  scene.instances.push_back(vkpt::pathtracer::RTInstance{
      200u,
      2u,
      1u,
      1u,
      1u,
      vkpt::pathtracer::kRTInstanceFlagDynamicTransform,
      1u,
      3u,
      3u,
      3u,
      3u,
      {0.0f, 0.0f, 0.0f},
      {},
      {1.0f, 1.0f, 1.0f}});
  scene.camera_position = {0.0f, 0.0f, 0.0f};
  scene.camera_target = {0.0f, 0.0f, -1.0f};
  scene.camera_up = {0.0f, 1.0f, 0.0f};
  return scene;
}

bool ConfigureScalar(vkpt::pathtracer::ScalarCpuPathTracer& tracer,
                     const vkpt::pathtracer::RenderSettings& settings,
                     const vkpt::pathtracer::PathTracerSceneSnapshot& scene) {
  return tracer.configure(settings) &&
         tracer.load_scene_snapshot(scene) &&
         tracer.build_or_update_acceleration() &&
         tracer.reset_accumulation();
}

class TestFlowSource final : public vkpt::core::contracts::IFlowSource {
 public:
  explicit TestFlowSource(std::uint64_t flow_id) : m_flowId(flow_id) {}
  std::uint64_t current_flow_id() const noexcept override { return m_flowId; }

 private:
  std::uint64_t m_flowId = 0u;
};

struct DeterminismHasher {
  std::uint64_t value = 1469598103934665603ull;

  void append_byte(std::uint8_t byte) {
    value ^= byte;
    value *= 1099511628211ull;
  }

  void append_u64(std::uint64_t input) {
    for (int shift = 0; shift < 64; shift += 8) {
      append_byte(static_cast<std::uint8_t>((input >> shift) & 0xffu));
    }
  }

  void append_u32(std::uint32_t input) {
    append_u64(input);
  }

  void append_bool(bool input) {
    append_byte(input ? 1u : 0u);
  }

  void append_float(float input) {
    std::uint32_t bits = 0u;
    std::memcpy(&bits, &input, sizeof(bits));
    append_u32(bits);
  }

  void append_string(std::string_view input) {
    append_u64(static_cast<std::uint64_t>(input.size()));
    for (char ch : input) {
      append_byte(static_cast<std::uint8_t>(ch));
    }
  }

  std::string hex() const {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
  }
};

void HashVec2(DeterminismHasher& hash, const vkpt::pathtracer::Vec2& value) {
  hash.append_float(value.u);
  hash.append_float(value.v);
}

void HashVec3(DeterminismHasher& hash, const vkpt::pathtracer::Vec3& value) {
  hash.append_float(value.x);
  hash.append_float(value.y);
  hash.append_float(value.z);
}

void HashQuat4(DeterminismHasher& hash, const vkpt::pathtracer::Quat4& value) {
  hash.append_float(value.x);
  hash.append_float(value.y);
  hash.append_float(value.z);
  hash.append_float(value.w);
}

void HashMaterial(DeterminismHasher& hash, const vkpt::pathtracer::RTMaterial& value) {
  HashVec3(hash, value.albedo);
  HashVec3(hash, value.emissive);
  hash.append_float(value.roughness);
  hash.append_float(value.metallic);
  hash.append_float(value.ior);
  hash.append_float(value.transmission);
  hash.append_float(value.clearcoat);
  hash.append_float(value.sheen);
  hash.append_float(value.anisotropy);
  hash.append_float(value.alpha);
  hash.append_u32(value.material_model);
  hash.append_u32(value.material_effect);
  hash.append_u32(value.material_flags);
  hash.append_u32(value.base_color_texture_index);
  hash.append_u32(value.normal_texture_index);
}

void HashInstance(DeterminismHasher& hash, const vkpt::pathtracer::RTInstance& value) {
  hash.append_u64(value.entity_id);
  hash.append_u32(value.geometry_id);
  hash.append_u32(value.first_triangle);
  hash.append_u32(value.triangle_count);
  hash.append_u32(value.material_index);
  hash.append_u32(value.flags);
  hash.append_u32(value.transform_revision);
  hash.append_u32(value.local_first_vertex);
  hash.append_u32(value.local_vertex_count);
  hash.append_u32(value.local_first_index);
  hash.append_u32(value.local_index_count);
  HashVec3(hash, value.translation);
  HashQuat4(hash, value.rotation);
  HashVec3(hash, value.scale);
}

void HashTransformUpdate(DeterminismHasher& hash,
                         const vkpt::pathtracer::RTInstanceTransformUpdate& value) {
  hash.append_u64(value.entity_id);
  hash.append_u32(value.instance_index);
  hash.append_u32(value.flags);
  hash.append_u32(value.transform_revision);
  HashVec3(hash, value.translation);
  HashQuat4(hash, value.rotation);
  HashVec3(hash, value.scale);
}

void HashLight(DeterminismHasher& hash, const vkpt::pathtracer::RTHitLight& value) {
  HashVec3(hash, value.position);
  HashVec3(hash, value.color);
  hash.append_float(value.intensity);
  hash.append_float(value.radius);
  HashVec3(hash, value.direction);
  hash.append_float(value.spot_inner_cos);
  hash.append_float(value.spot_outer_cos);
}

void HashCamera(DeterminismHasher& hash, const vkpt::pathtracer::RTCameraState& value) {
  HashVec3(hash, value.position);
  HashVec3(hash, value.target);
  HashVec3(hash, value.up);
  hash.append_float(value.fov_deg);
  hash.append_float(value.focal_length_mm);
  hash.append_float(value.sensor_width_mm);
  hash.append_float(value.sensor_height_mm);
  hash.append_float(value.aperture_radius);
  hash.append_float(value.focus_distance);
  hash.append_float(value.f_stop);
  hash.append_float(value.shutter_seconds);
  hash.append_float(value.iso);
  hash.append_float(value.exposure_compensation);
  hash.append_float(value.white_balance_kelvin);
  hash.append_u32(value.iris_blade_count);
  hash.append_float(value.iris_rotation_degrees);
  hash.append_float(value.iris_roundness);
  hash.append_float(value.anamorphic_squeeze);
}

void HashInstanceMotion(DeterminismHasher& hash,
                        const vkpt::scene::RenderInstanceMotion& value) {
  hash.append_u32(value.entity_id);
  hash.append_u32(value.instance_index);
  hash.append_bool(value.previous_valid);
  HashInstance(hash, value.previous);
  HashInstance(hash, value.current);
}

template <typename T, typename Fn>
void HashCowArray(DeterminismHasher& hash,
                  const vkpt::scene::CowArray<T>& values,
                  Fn hash_value) {
  hash.append_u64(static_cast<std::uint64_t>(values.size()));
  for (const auto& value : values.view()) {
    hash_value(hash, value);
  }
}

std::string HashSnapshotOutputs(const vkpt::scene::RenderSceneSnapshot& snapshot) {
  DeterminismHasher hash;
  hash.append_u64(snapshot.generation);
  hash.append_u64(snapshot.topology_revision);
  hash.append_u64(snapshot.transform_revision);
  hash.append_u64(snapshot.camera_revision);
  hash.append_u64(snapshot.material_revision);
  HashCowArray(hash, snapshot.vertices, HashVec3);
  HashCowArray(hash, snapshot.texcoords, HashVec2);
  HashCowArray(hash, snapshot.indices, [](DeterminismHasher& out, std::uint32_t value) {
    out.append_u32(value);
  });
  HashCowArray(hash, snapshot.local_vertices, HashVec3);
  HashCowArray(hash, snapshot.local_indices, [](DeterminismHasher& out, std::uint32_t value) {
    out.append_u32(value);
  });
  HashCowArray(hash, snapshot.instances, HashInstance);
  HashCowArray(hash, snapshot.materials, HashMaterial);
  HashCowArray(hash, snapshot.textures, [](DeterminismHasher& out, const std::string& value) {
    out.append_string(value);
  });
  HashCowArray(hash, snapshot.lights, HashLight);
  HashCowArray(hash, snapshot.environment_map, HashVec3);
  HashCowArray(hash, snapshot.instance_motion, HashInstanceMotion);
  HashVec3(hash, snapshot.environment_color);
  HashVec3(hash, snapshot.environment_map_scale);
  hash.append_u32(snapshot.environment_map_width);
  hash.append_u32(snapshot.environment_map_height);
  HashCamera(hash, snapshot.camera);
  hash.append_bool(snapshot.acceleration.valid());
  hash.append_bool(snapshot.acceleration.reused_from_previous);
  hash.append_bool(snapshot.acceleration.transform_refit_descriptor);
  hash.append_bool(snapshot.acceleration.cpu_bvh_info.built);
  hash.append_u64(static_cast<std::uint64_t>(snapshot.acceleration.cpu_bvh_info.primitive_count));
  hash.append_u64(static_cast<std::uint64_t>(snapshot.acceleration.cpu_bvh_info.node_count));
  hash.append_u64(static_cast<std::uint64_t>(snapshot.acceleration.cpu_bvh_info.leaf_count));
  hash.append_bool(snapshot.acceleration.cpu_bvh_info.deterministic);
  hash.append_u64(static_cast<std::uint64_t>(snapshot.acceleration.refit_updates.size()));
  for (const auto& update : snapshot.acceleration.refit_updates) {
    HashTransformUpdate(hash, update);
  }
  return hash.hex();
}

std::vector<std::string> BuildDeterministicSnapshotHashStream() {
  auto scene = MakeScene();
  vkpt::scene::RenderSceneSnapshotRevisions rev{};
  vkpt::scene::RenderSceneSnapshot::Ptr previous;
  std::vector<std::string> hashes;
  hashes.reserve(3u);

  for (std::uint64_t step = 0u; step < 3u; ++step) {
    rev.generation = step + 1u;
    rev.wall_time_ns = 0u;
    if (step == 1u) {
      rev.transform_revision = 2u;
      scene.instances[0].translation.x = 0.5f;
      scene.instances[0].transform_revision = 2u;
    } else if (step == 2u) {
      rev.camera_revision = 2u;
      scene.camera_position.y = 0.125f;
    }
    previous = vkpt::scene::BuildRenderSceneSnapshot(scene, previous.get(), rev);
    hashes.push_back(HashSnapshotOutputs(*previous));
  }

  return hashes;
}

bool CheckDeterministicSnapshotOutputsHashStream() {
  std::array<std::vector<std::string>, 3u> runs = {
      BuildDeterministicSnapshotHashStream(),
      BuildDeterministicSnapshotHashStream(),
      BuildDeterministicSnapshotHashStream(),
  };

  bool ok = true;
  for (std::size_t run = 0u; run < runs.size(); ++run) {
    ok &= Check(runs[run].size() == 3u,
                "determinism snapshot stream should contain three hashes");
    for (std::size_t index = 0u; index < runs[run].size(); ++index) {
      std::cerr << "determinism.snapshot.outputs_hash run=" << run
                << " index=" << index
                << " hash=" << runs[run][index] << "\n";
    }
  }
  ok &= Check(runs[0] == runs[1] && runs[0] == runs[2],
              "determinism.snapshot.outputs_hash streams should match across 3 runs");
  ok &= Check(runs[0][0] != runs[0][1] && runs[0][1] != runs[0][2],
              "determinism snapshot hash stream should reflect transform and camera changes");
  return ok;
}

bool CheckCowReuse() {
  auto scene = MakeScene();
  vkpt::scene::RenderSceneSnapshotRevisions rev{};
  rev.generation = 1u;
  auto first = vkpt::scene::BuildRenderSceneSnapshot(scene, nullptr, rev);
  if (!Check(first->acceleration.valid(),
             "snapshot should own a precomputed CPU BVH handle") ||
      !Check(first->acceleration.cpu_bvh_info.built,
             "snapshot CPU BVH handle should expose build info")) {
    return false;
  }
  auto identical = vkpt::scene::BuildRenderSceneSnapshot(scene, first.get(), rev);
  if (!Check(identical->vertices.shares_storage_with(first->vertices),
             "identical snapshot should share vertices") ||
      !Check(identical->indices.shares_storage_with(first->indices),
             "identical snapshot should share indices") ||
      !Check(identical->instances.shares_storage_with(first->instances),
             "identical snapshot should share instances")) {
    return false;
  }

  ++rev.generation;
  ++rev.camera_revision;
  scene.camera_position.x = 0.25f;
  auto cameraOnly = vkpt::scene::BuildRenderSceneSnapshot(scene, first.get(), rev);
  if (!Check(cameraOnly->vertices.shares_storage_with(first->vertices),
             "camera-only snapshot should share vertices") ||
      !Check(cameraOnly->indices.shares_storage_with(first->indices),
             "camera-only snapshot should share indices") ||
      !Check(cameraOnly->instances.shares_storage_with(first->instances),
             "camera-only snapshot should share instances") ||
      !Check(cameraOnly->acceleration.reused_from_previous,
             "camera-only snapshot should reuse previous BVH handle")) {
    return false;
  }

  ++rev.generation;
  ++rev.transform_revision;
  scene.instances[0].translation.x = 2.0f;
  scene.instances[0].transform_revision = 2u;
  auto transformOnly = vkpt::scene::BuildRenderSceneSnapshot(scene, cameraOnly.get(), rev);
  if (!Check(transformOnly->vertices.shares_storage_with(cameraOnly->vertices),
             "transform-only snapshot should share vertices") ||
      !Check(transformOnly->indices.shares_storage_with(cameraOnly->indices),
             "transform-only snapshot should share indices") ||
      !Check(!transformOnly->instances.shares_storage_with(cameraOnly->instances),
             "transform-only snapshot should replace instance array")) {
    return false;
  }
  if (!Check(transformOnly->instance_motion.size() == 1u,
             "transform-only snapshot should carry one motion pair") ||
      !Check(transformOnly->instance_motion[0].previous.translation.x == 0.0f,
             "motion pair should retain previous transform") ||
      !Check(transformOnly->instance_motion[0].current.translation.x == 2.0f,
             "motion pair should retain current transform") ||
      !Check(transformOnly->acceleration.valid(),
             "transform-only snapshot should own a valid refit BVH handle") ||
      !Check(transformOnly->acceleration.transform_refit_descriptor,
             "transform-only snapshot should carry a BVH refit descriptor") ||
      !Check(transformOnly->acceleration.refit_updates.size() == 1u,
             "BVH refit descriptor should carry one transform update")) {
    return false;
  }
  vkpt::pathtracer::RayQueryHit transformedHit;
  vkpt::pathtracer::Ray transformedRay;
  transformedRay.origin = {2.0f, 0.25f, 0.0f};
  transformedRay.direction = {0.0f, 0.0f, -1.0f};
  if (!Check(transformOnly->acceleration.cpu_bvh->intersect(
                 transformedRay,
                 transformedHit,
                 nullptr) &&
                 transformedHit.hit,
             "snapshot-owned refit BVH should hit the moved instance")) {
    return false;
  }

  const auto changes =
      vkpt::scene::CompareRenderSceneSnapshots(cameraOnly.get(), *transformOnly);
  if (!Check(vkpt::scene::HasChange(changes, vkpt::scene::RenderSceneSnapshotChange::Transform),
             "transform revision should be reported") ||
      !Check(!vkpt::scene::HasChange(changes, vkpt::scene::RenderSceneSnapshotChange::Topology),
             "transform-only snapshot should not report topology")) {
    return false;
  }

  const auto resetDecision =
      vkpt::scene::DecideSnapshotTransition(cameraOnly.get(), *transformOnly);
  vkpt::scene::SnapshotTransitionCapabilities motionCaps;
  motionCaps.transform_motion_vectors = true;
  const auto perPixelDecision =
      vkpt::scene::DecideSnapshotTransition(cameraOnly.get(), *transformOnly, motionCaps);
  vkpt::scene::SnapshotTransitionCapabilities cameraCaps;
  cameraCaps.camera_reprojection = true;
  const auto cameraDecision =
      vkpt::scene::DecideSnapshotTransition(first.get(), *cameraOnly, cameraCaps);
  ++rev.generation;
  auto staticNext = vkpt::scene::BuildRenderSceneSnapshot(scene, transformOnly.get(), rev);
  const auto continueDecision =
      vkpt::scene::DecideSnapshotTransition(transformOnly.get(), *staticNext);
  if (!Check(resetDecision.action == vkpt::scene::SnapshotTransitionAction::ResetAccumulation,
             "default transform transition should reset accumulation") ||
      !Check(resetDecision.rebuild_tile_schedule,
             "changed snapshot should rebuild tile schedule") ||
      !Check(perPixelDecision.action == vkpt::scene::SnapshotTransitionAction::ResetMovingPixels,
             "motion-vector-capable transform transition should reset moving pixels") ||
      !Check(cameraDecision.action == vkpt::scene::SnapshotTransitionAction::ReprojectCamera,
             "camera reprojection capability should select camera reprojection") ||
      !Check(continueDecision.action == vkpt::scene::SnapshotTransitionAction::Continue,
             "unchanged revisions should continue across snapshot generations")) {
    return false;
  }

  const auto updates = vkpt::scene::DiffInstanceTransforms(*cameraOnly, *transformOnly);
  return Check(updates.size() == 1u, "one transform update should be diffed") &&
         Check(updates[0].entity_id == 42u, "transform diff should preserve entity id") &&
         Check(updates[0].translation.x == 2.0f, "transform diff should carry new translation");
}

bool CheckHistoryCameraSweepValidation() {
  auto scene = MakeScene();
  vkpt::scene::RenderSceneSnapshotRevisions rev{};
  rev.generation = 1u;
  auto previous = vkpt::scene::BuildRenderSceneSnapshot(scene, nullptr, rev);

  ++rev.generation;
  ++rev.camera_revision;
  scene.camera_target.x = 0.20f;
  auto current = vkpt::scene::BuildRenderSceneSnapshot(scene, previous.get(), rev);

  vkpt::scene::SnapshotTransitionCapabilities caps;
  caps.camera_reprojection = true;
  const auto decision =
      vkpt::scene::DecideSnapshotTransition(previous.get(), *current, caps);
  const auto history = vkpt::render::ApplyCameraHistoryTransition(
      MakeFilledFilm(32u, 16u, 12u),
      previous->camera,
      current->camera);

  const std::uint64_t totalPixels = 32u * 16u;
  return Check(decision.action == vkpt::scene::SnapshotTransitionAction::ReprojectCamera,
               "camera-only snapshot should select reprojection") &&
         Check(history.applied,
               "camera sweep history reprojection should apply") &&
         Check(history.pixels_reprojected > totalPixels * 2u / 3u,
               "camera sweep should keep most prior history") &&
         Check(history.pixels_reset > 0u,
               "camera sweep should expose a measurable border variance bump") &&
         Check(history.pixels_reset < totalPixels / 3u,
               "camera sweep reprojection reset area should stay bounded") &&
         Check(history.variance_bump > 0.0 && history.variance_bump <= 32.0,
               "camera sweep sample-count variance bump should be bounded");
}

bool CheckOneMoverHistoryValidation() {
  auto scene = MakeOneMoverScene();
  vkpt::scene::RenderSceneSnapshotRevisions rev{};
  rev.generation = 1u;
  auto previous = vkpt::scene::BuildRenderSceneSnapshot(scene, nullptr, rev);

  ++rev.generation;
  ++rev.transform_revision;
  scene.instances[1].translation.x = 0.55f;
  scene.instances[1].transform_revision = 2u;
  auto current = vkpt::scene::BuildRenderSceneSnapshot(scene, previous.get(), rev);

  vkpt::scene::SnapshotTransitionCapabilities caps;
  caps.transform_motion_vectors = true;
  const auto decision =
      vkpt::scene::DecideSnapshotTransition(previous.get(), *current, caps);
  const auto motion = vkpt::render::RasterizeCoarseMotionVectors(
      *previous,
      *current,
      48u,
      32u,
      2u);
  const auto history =
      vkpt::render::ApplyTransformHistoryTransition(MakeFilledFilm(48u, 32u, 8u),
                                                    motion);

  bool movingCellHasVector = false;
  for (const auto& cell : motion.cells) {
    movingCellHasVector = movingCellHasVector ||
                          (cell.moving() && std::abs(cell.dx) > 0.01f);
  }

  bool movingPixelReset = false;
  bool backgroundKept = false;
  for (std::uint32_t y = 0u; y < history.film.height(); ++y) {
    for (std::uint32_t x = 0u; x < history.film.width(); ++x) {
      const std::size_t index =
          static_cast<std::size_t>(y) * history.film.width() + x;
      if (motion.moving_pixel(x, y)) {
        movingPixelReset = movingPixelReset ||
                           history.film.sample_counts()[index] == 0u;
      } else {
        backgroundKept = backgroundKept ||
                         history.film.sample_counts()[index] == 8u;
      }
    }
  }

  const std::uint64_t totalPixels = 48u * 32u;
  return Check(decision.action == vkpt::scene::SnapshotTransitionAction::ResetMovingPixels,
               "one-mover snapshot should select moving-pixel reset") &&
         Check(current->instance_motion.size() == 1u,
               "one-mover snapshot should carry one transform motion pair") &&
         Check(motion.valid() && motion.moving_cell_count() > 0u,
               "one-mover should rasterize coarse motion cells") &&
         Check(movingCellHasVector,
               "one-mover coarse motion cells should carry non-zero vectors") &&
         Check(history.applied,
               "one-mover history transition should apply") &&
         Check(history.pixels_reset == motion.moving_pixel_count(),
               "one-mover should reset exactly motion-vector-covered pixels") &&
         Check(history.pixels_reset > 0u && history.pixels_reset < totalPixels / 2u,
               "one-mover reset area should be local, not full-frame") &&
         Check(history.pixels_kept > history.pixels_reset,
               "one-mover background should keep more history than mover resets") &&
         Check(movingPixelReset,
               "one-mover moving pixels should reset sample counts") &&
         Check(backgroundKept,
               "one-mover background pixels should keep accumulated samples");
}

bool CheckMaterialReshadeInvalidation() {
  auto scene = MakeScene();
  vkpt::scene::RenderSceneSnapshotRevisions rev{};
  rev.generation = 1u;
  auto previous = vkpt::scene::BuildRenderSceneSnapshot(scene, nullptr, rev);

  ++rev.generation;
  ++rev.material_revision;
  scene.materials[0].albedo = {0.8f, 0.2f, 0.1f};
  auto current = vkpt::scene::BuildRenderSceneSnapshot(scene, previous.get(), rev);

  vkpt::scene::SnapshotTransitionCapabilities caps;
  caps.material_reshade = true;
  const auto decision =
      vkpt::scene::DecideSnapshotTransition(previous.get(), *current, caps);
  const auto history =
      vkpt::render::ApplyMaterialHistoryTransition(MakeFilledFilm(12u, 10u, 9u));

  return Check(decision.action == vkpt::scene::SnapshotTransitionAction::InvalidateShading,
               "material-only snapshot should select shading invalidation") &&
         Check(current->geometry_storage_reused_from(*previous),
               "material-only snapshot should keep geometry storage") &&
         Check(current->acceleration.reused_from_previous,
               "material-only snapshot should keep acceleration history") &&
         Check(history.applied && history.shading_invalidated,
               "material-only history should invalidate shading") &&
         Check(history.geometry_samples_preserved == 12u * 10u * 9u,
               "material-only history should account for preserved geometry samples") &&
         Check(history.pixels_reset == 12u * 10u,
               "material-only history should reset shading for every pixel") &&
         Check(AllSampleCountsZero(history.film),
               "material-only shading invalidation should clear radiance sample counts");
}

bool CheckSimTickSnapshotPublish() {
  vkpt::core::metrics::MetricsRegistry::instance().reset("vkp.scene.");
  auto scene = MakeScene();
  vkpt::scene::SnapshotRing ring;
  vkpt::scene::RenderSceneSnapshotRevisions rev{};
  rev.generation = 1u;

  vkpt::scene::SimSnapshotTickRequest initialTick;
  initialTick.snapshots = &ring;
  initialTick.scene = &scene;
  initialTick.revisions = rev;
  const auto initial = vkpt::scene::PublishSimTickSnapshot(initialTick);
  if (!Check(initial.published, "sim tick should publish initial snapshot") ||
      !Check(initial.status, "initial sim tick status should be ok") ||
      !Check(initial.snapshot && initial.snapshot->generation == 1u,
             "initial sim tick should build generation 1") ||
      !Check(ring.current() && ring.current()->generation == 1u,
             "initial sim tick should publish through ring") ||
      !Check(initial.snapshot->acceleration.valid(),
             "initial sim tick should precompute BVH before publish")) {
    return false;
  }

  bool rejectedValidateRan = false;
  bool rejectedApplyRan = false;
  vkpt::scene::RenderSceneSnapshotRevisions rejectedRev = rev;
  rejectedRev.generation = 2u;
  vkpt::scene::SimSnapshotTickRequest rejectedTick;
  rejectedTick.snapshots = &ring;
  rejectedTick.scene = &scene;
  rejectedTick.revisions = rejectedRev;
  rejectedTick.validate_writes = [&](const vkpt::pathtracer::PathTracerSceneSnapshot&) {
    rejectedValidateRan = true;
    return vkpt::core::Status::error(vkpt::core::StatusCode::InvalidArgument,
                                     "validation rejected test writes");
  };
  rejectedTick.apply_writes = [&](vkpt::pathtracer::PathTracerSceneSnapshot& tickScene) {
    rejectedApplyRan = true;
    tickScene.instances[0].translation.x = 99.0f;
  };
  const auto rejected = ring.publish_sim_tick(rejectedTick);
  if (!Check(rejectedValidateRan,
             "sim tick validation should run before apply") ||
      !Check(!rejectedApplyRan,
             "sim tick apply should not run after validation failure") ||
      !Check(rejected.status.is_error() && !rejected.published,
             "invalid sim tick should return failed status without publish") ||
      !Check(ring.current() && ring.current()->generation == 1u,
             "invalid sim tick should not advance the published generation") ||
      !Check(scene.instances[0].translation.x == 0.0f,
             "invalid sim tick should leave scene writes unapplied")) {
    return false;
  }

  vkpt::scene::SnapshotTransitionCapabilities motionCaps;
  motionCaps.transform_motion_vectors = true;
  bool writesRan = false;
  std::uint64_t generationObservedDuringWrites = 0u;
  ++rev.generation;
  ++rev.transform_revision;
  vkpt::scene::SimSnapshotTickRequest moveTick;
  moveTick.snapshots = &ring;
  moveTick.scene = &scene;
  moveTick.revisions = rev;
  moveTick.transition_capabilities = motionCaps;
  moveTick.apply_writes = [&](vkpt::pathtracer::PathTracerSceneSnapshot& tickScene) {
    writesRan = true;
    if (const auto current = ring.current()) {
      generationObservedDuringWrites = current->generation;
    }
    tickScene.instances[0].translation.x = 3.0f;
    tickScene.instances[0].transform_revision = 2u;
    tickScene.instances[0].flags |= vkpt::pathtracer::kRTInstanceFlagTransformDirty;
  };

  const auto moved = vkpt::scene::PublishSimTickSnapshot(moveTick);
  const auto publishedMoved = ring.current();
  if (!Check(writesRan, "sim tick should apply transform writes") ||
      !Check(moved.status, "moved sim tick status should be ok") ||
      !Check(generationObservedDuringWrites == 1u,
             "sim tick should apply transform writes before publish") ||
      !Check(moved.previous && moved.previous->generation == 1u,
             "sim tick should diff against previously published snapshot") ||
      !Check(moved.published && publishedMoved && publishedMoved->generation == 2u,
             "sim tick should publish generation 2 after writes") ||
      !Check(publishedMoved->instances[0].translation.x == 3.0f,
             "published sim tick snapshot should include transform writes") ||
      !Check(publishedMoved->instance_motion.size() == 1u,
             "published sim tick snapshot should include motion pair") ||
      !Check(publishedMoved->instance_motion[0].previous.translation.x == 0.0f,
             "published sim tick motion pair should include previous transform") ||
      !Check(publishedMoved->instance_motion[0].current.translation.x == 3.0f,
             "published sim tick motion pair should include current transform") ||
      !Check(moved.build_stats.acceleration_built,
             "sim tick should build acceleration before publish") ||
      !Check(publishedMoved->acceleration.transform_refit_descriptor,
             "sim tick should refit BVH before publish") ||
      !Check(moved.transition.action == vkpt::scene::SnapshotTransitionAction::ResetMovingPixels,
             "sim tick with motion vectors should reset moving pixels")) {
    return false;
  }

  vkpt::pathtracer::RayQueryHit movedHit;
  vkpt::pathtracer::Ray movedRay;
  movedRay.origin = {3.0f, 0.25f, 0.0f};
  movedRay.direction = {0.0f, 0.0f, -1.0f};
  if (!Check(publishedMoved->acceleration.cpu_bvh->intersect(
                 movedRay,
                 movedHit,
                 nullptr) &&
                 movedHit.hit,
             "published sim tick refit BVH should hit moved instance")) {
    return false;
  }

  const auto sceneMetrics =
      vkpt::core::metrics::MetricsRegistry::instance().snapshot_prefix("vkp.scene.");
  if (!CheckGaugeEquals(sceneMetrics, "vkp.scene.entity_count", 1.0) ||
      !CheckGaugeEquals(sceneMetrics, "vkp.scene.transform_dirty_count", 1.0)) {
    return false;
  }

  ++rev.generation;
  ++rev.transform_revision;
  vkpt::scene::SimSnapshotTickRequest fallbackTick;
  fallbackTick.snapshots = &ring;
  fallbackTick.scene = &scene;
  fallbackTick.revisions = rev;
  fallbackTick.transition_capabilities = motionCaps;
  const auto fallback = vkpt::scene::PublishSimTickSnapshot(fallbackTick);
  return Check(fallback.published && fallback.snapshot,
               "sim tick fallback case should publish") &&
         Check(fallback.status, "fallback sim tick status should be ok") &&
         Check(fallback.snapshot->instance_motion.empty(),
               "transform revision without transform pairs should have no motion vectors") &&
         Check(fallback.transition.action == vkpt::scene::SnapshotTransitionAction::ResetAccumulation,
               "sim tick should full-reset when motion vectors are unavailable") &&
         Check(fallback.transition.reset_accumulation,
               "motion-vector fallback should request accumulation reset");
}

bool CheckRingReaders() {
  vkpt::core::metrics::MetricsRegistry::instance().reset("vkp.snapshot.");
  vkpt::core::metrics::MetricsRegistry::instance().reset("vkp.scene.");
  vkpt::scene::SnapshotRing ring;
  const auto readerA = ring.register_reader("tracer");
  const auto readerB = ring.register_reader("ui");
  if (!Check(readerA != vkpt::scene::SnapshotRing::kInvalidReader,
             "tracer reader should register") ||
      !Check(readerB != vkpt::scene::SnapshotRing::kInvalidReader,
             "ui reader should register")) {
    return false;
  }

  auto scene = MakeScene();
  vkpt::scene::RenderSceneSnapshotRevisions rev{};
  for (std::uint64_t gen = 1u; gen <= 4u; ++gen) {
    rev.generation = gen;
    auto snapshot = vkpt::scene::BuildRenderSceneSnapshot(scene, ring.current().get(), rev);
    ring.publish(snapshot);
    if (gen < 4u) {
      (void)ring.current(readerA);
    }
  }
  rev.generation = 5u;
  auto droppedSnapshot = vkpt::scene::BuildRenderSceneSnapshot(scene, ring.current().get(), rev);
  ring.publish(droppedSnapshot);

  const auto stats = ring.stats();
  if (!Check(stats.publish_total == 5u, "ring should count all publishes") ||
      !Check(stats.latest_generation == 5u, "ring should report latest generation") ||
      !Check(stats.dropped_total == 1u, "ring should count unobserved snapshot drops")) {
    return false;
  }
  const auto metrics =
      vkpt::core::metrics::MetricsRegistry::instance().snapshot_prefix("vkp.snapshot.");
  if (!CheckCounter(metrics, "vkp.snapshot.publish_total", 5u) ||
      !CheckCounter(metrics, "vkp.snapshot.dropped_total", 1u)) {
    return false;
  }
  const auto sceneMetrics =
      vkpt::core::metrics::MetricsRegistry::instance().snapshot_prefix("vkp.scene.");
  if (!CheckCounter(sceneMetrics, "vkp.scene.snapshot_published_total", 5u) ||
      !CheckCounter(sceneMetrics, "vkp.scene.snapshot_dropped_total", 1u) ||
      !CheckHistogramAtLeast(sceneMetrics, "vkp.scene.snapshot_build_us", 5u) ||
      !Check(FindMetric(sceneMetrics,
                        "vkp.scene.snapshot_cow_reuse_ratio",
                        vkpt::core::metrics::Kind::GaugeKind) != nullptr,
             "scene snapshot COW reuse gauge should be published")) {
    return false;
  }
  const auto lag = ring.reader_stats(readerB);
  const auto lagWarningEvents = CountCrashEvents("scene", "lag_warning_emitted");
  return Check(lag.lag == 5u, "unobserved reader should lag by latest generation") &&
         Check(lag.lag_warning_emitted,
               "unobserved reader should latch lag warning state") &&
         Check(lagWarningEvents == 1u,
               "snapshot reader lag should emit one warning event before catch-up");
}

bool CheckRingFuzz() {
  vkpt::scene::SnapshotRing ring;
  const auto fast = ring.register_reader("fast");
  const auto slow = ring.register_reader("slow");
  const auto bursty = ring.register_reader("bursty");
  std::atomic_bool done{false};
  std::atomic_bool bad{false};

  auto reader = [&](std::uint32_t id, int sleep_us) {
    std::uint64_t last = 0u;
    while (!done.load(std::memory_order_acquire)) {
      if (auto snapshot = ring.current(id)) {
        if (snapshot->generation < last) {
          bad.store(true, std::memory_order_release);
        }
        last = snapshot->generation;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    }
  };

  std::thread fastThread(reader, fast, 50);
  std::thread slowThread(reader, slow, 500);
  std::thread burstyThread(reader, bursty, 125);

  auto scene = MakeScene();
  vkpt::scene::RenderSceneSnapshotRevisions rev{};
  vkpt::scene::RenderSceneSnapshot::Ptr previous;
  for (std::uint64_t gen = 1u; gen <= 120u; ++gen) {
    rev.generation = gen;
    rev.camera_revision = gen;
    scene.camera_position.x = static_cast<float>(gen) * 0.001f;
    previous = vkpt::scene::BuildRenderSceneSnapshot(scene, previous.get(), rev);
    ring.publish(previous);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  done.store(true, std::memory_order_release);
  fastThread.join();
  slowThread.join();
  burstyThread.join();
  return Check(!bad.load(std::memory_order_acquire), "readers should never observe generation regression");
}

bool CheckTileScheduler() {
  vkpt::render::TileScheduler scheduler;
  scheduler.configure(vkpt::render::TileSchedulerConfig{64u, 35u, 16u, 2u});
  scheduler.begin_sample(9u, 3u);
  std::vector<vkpt::pathtracer::RenderTile> tiles;
  vkpt::pathtracer::RenderTile tile;
  while (scheduler.next_tile(tile)) {
    tiles.push_back(tile);
  }
  return Check(tiles.size() == 3u, "35px frame at 16px tile height should produce 3 tiles") &&
         Check(tiles[0].y == 0u && tiles[0].height == 16u, "first tile rows should match") &&
         Check(tiles[2].y == 32u && tiles[2].height == 3u, "last tile should clamp height") &&
         Check(tiles[1].gpu_id == 1u, "scheduler should shard gpu id by tile id") &&
         Check(tiles[2].sample_index == 3u, "scheduler should stamp sample index");
}

bool CheckTileSchedulerPriority() {
  vkpt::render::TileScheduler scheduler;
  scheduler.configure(vkpt::render::TileSchedulerConfig{32u, 32u, 8u, 1u});
  const std::vector<vkpt::render::TilePriorityFeedback> feedback = {
      {0u, 0.10, 9u, false},
      {1u, 0.80, 9u, false},
      {2u, 0.80, 2u, false},
      {3u, 0.30, 1u, false},
  };
  scheduler.set_feedback(feedback);
  scheduler.begin_sample(10u, 4u);
  std::vector<std::uint32_t> order;
  vkpt::pathtracer::RenderTile tile;
  while (scheduler.next_tile(tile)) {
    order.push_back(tile.tile_id);
  }
  if (!Check(order.size() == 4u, "priority scheduler should emit every tile")) {
    return false;
  }
  const bool varianceAndSamples =
      order[0] == 2u && order[1] == 1u && order[2] == 3u && order[3] == 0u;

  const std::vector<vkpt::render::TilePriorityFeedback> dirty_feedback = {
      {0u, 0.10, 9u, false},
      {1u, 0.80, 9u, false},
      {2u, 0.80, 2u, false},
      {3u, 0.01, 99u, true},
  };
  scheduler.set_feedback(dirty_feedback);
  scheduler.begin_sample(10u, 5u);
  order.clear();
  while (scheduler.next_tile(tile)) {
    order.push_back(tile.tile_id);
  }
  return Check(varianceAndSamples,
               "scheduler should prioritize highest variance and lowest sample count") &&
         Check(!order.empty() && order[0] == 3u,
               "dirty tile feedback should take priority over variance");
}

bool CheckTileSchedulerFoveated() {
  vkpt::render::TileSchedulerConfig config;
  config.width = 64u;
  config.height = 40u;
  config.tile_height = 8u;
  config.gpu_count = 2u;
  config.foveated_center_extra_samples = 2u;
  config.foveated_center_radius = 0.11;

  vkpt::render::TileScheduler scheduler;
  scheduler.configure(config);
  scheduler.begin_sample(12u, 7u);

  std::vector<std::uint32_t> tileIds;
  std::vector<std::uint32_t> sampleIndices;
  vkpt::pathtracer::RenderTile tile;
  while (scheduler.next_tile(tile)) {
    tileIds.push_back(tile.tile_id);
    sampleIndices.push_back(tile.sample_index);
  }
  const auto stats = scheduler.stats();
  const std::vector<std::uint32_t> expectedIds = {0u, 1u, 2u, 3u, 4u, 2u, 2u};
  const std::vector<std::uint32_t> expectedSamples = {21u, 21u, 21u, 21u, 21u, 22u, 23u};
  return Check(tileIds == expectedIds,
               "foveated scheduler should add extra center tile slices") &&
         Check(sampleIndices == expectedSamples,
               "foveated scheduler should use unique center sample indices") &&
         Check(stats.tile_count == 5u && stats.scheduled_tile_count == 7u,
               "foveated scheduler stats should distinguish physical and scheduled tiles") &&
         Check(stats.gpu_scheduled_tile_count.size() == 2u &&
                   stats.gpu_scheduled_tile_count[0] == 5u &&
                   stats.gpu_scheduled_tile_count[1] == 2u,
               "scheduler should count scheduled slices per GPU");
}

bool CheckScalarRenderTilePrimitive() {
  vkpt::pathtracer::RenderSettings settings;
  settings.width = 12u;
  settings.height = 10u;
  settings.spp = 2u;
  settings.max_depth = 1u;
  settings.deterministic = true;
  settings.seed = 0x45u;
  const auto scene = MakeScene();

  vkpt::pathtracer::ScalarCpuPathTracer batchTracer;
  vkpt::pathtracer::ScalarCpuPathTracer tileTracer;
  if (!Check(ConfigureScalar(batchTracer, settings, scene),
             "batch scalar tracer should configure") ||
      !Check(ConfigureScalar(tileTracer, settings, scene),
             "tile scalar tracer should configure")) {
    return false;
  }
  vkpt::pathtracer::RenderTile fullTile;
  fullTile.x = 0u;
  fullTile.y = 0u;
  fullTile.width = settings.width;
  fullTile.height = settings.height;
  fullTile.sample_index = 0u;
  if (!Check(batchTracer.render_tile(fullTile, 0u),
             "scalar full-tile render should succeed") ||
      !Check(tileTracer.render_tile(fullTile, 0u),
             "scalar tile render should succeed") ||
      !CheckFilmEqual(batchTracer.film(),
                      tileTracer.film(),
                      "full-width scalar tile renders should match")) {
    return false;
  }

  vkpt::pathtracer::ScalarCpuPathTracer partialTracer;
  if (!Check(ConfigureScalar(partialTracer, settings, scene),
             "partial scalar tracer should configure")) {
    return false;
  }
  vkpt::pathtracer::RenderTile partialTile;
  partialTile.x = 3u;
  partialTile.y = 2u;
  partialTile.width = 5u;
  partialTile.height = 4u;
  partialTile.sample_index = 0u;
  if (!Check(partialTracer.render_tile(partialTile, 0u),
             "partial scalar tile render should succeed")) {
    return false;
  }
  const auto& counts = partialTracer.film().sample_counts();
  for (std::uint32_t y = 0u; y < settings.height; ++y) {
    for (std::uint32_t x = 0u; x < settings.width; ++x) {
      const bool inside =
          x >= partialTile.x &&
          x < partialTile.x + partialTile.width &&
          y >= partialTile.y &&
          y < partialTile.y + partialTile.height;
      const std::uint32_t expected = inside ? 1u : 0u;
      const std::size_t index =
          static_cast<std::size_t>(y) * settings.width + x;
      if (!Check(counts[index] == expected,
                 "partial scalar tile should only touch covered pixels")) {
        return false;
      }
    }
  }
  return true;
}

bool CheckCpuBvhDynamicTransformRefit() {
  auto scene = MakeScene();
  auto accelerator = vkpt::pathtracer::CreateCpuBvhAccelerator();
  if (!Check(static_cast<bool>(accelerator),
             "CPU BVH accelerator should be constructible") ||
      !Check(accelerator->build(scene, true),
             "CPU BVH accelerator should build smoke scene")) {
    return false;
  }

  vkpt::pathtracer::RayQueryHit hit;
  vkpt::pathtracer::Ray ray;
  ray.origin = {0.0f, 0.25f, 0.0f};
  ray.direction = {0.0f, 0.0f, -1.0f};
  if (!Check(accelerator->intersect(ray, hit, nullptr) && hit.hit,
             "CPU BVH should hit triangle before dynamic transform")) {
    return false;
  }

  vkpt::pathtracer::RTInstanceTransformUpdate update;
  update.entity_id = 42u;
  update.translation = {5.0f, 0.0f, 0.0f};
  update.rotation = {};
  update.scale = {1.0f, 1.0f, 1.0f};
  update.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform;
  update.transform_revision = 2u;
  const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates = {update};
  vkpt::pathtracer::InstanceTransformUpdateOptions options;
  options.reason = vkpt::pathtracer::RenderUpdateReason::PhysicsMotion;
  const auto plan = accelerator->plan_instance_transform_update(scene, updates, options);
  const auto repeatPlan = accelerator->plan_instance_transform_update(scene, updates, options);
  vkpt::pathtracer::RayQueryHit plannedHit;
  const bool planLeftBvhUntouched =
      accelerator->intersect(ray, plannedHit, nullptr) && plannedHit.hit;
  const auto result = accelerator->apply_instance_transform_update(scene, updates, options);
  if (!Check(plan.status == vkpt::pathtracer::InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate,
             "CPU BVH should plan dynamic transform refit") ||
      !CheckEquivalentPlan(plan,
                           repeatPlan,
                           "CPU BVH transform planning should be stable for unchanged inputs") ||
      !Check(planLeftBvhUntouched,
             "CPU BVH transform planning should not mutate acceleration state") ||
      !Check(result.status == vkpt::pathtracer::InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate,
             "CPU BVH should apply dynamic transform refit") ||
      !Check(result.full_rebuild_ms == 0.0,
             "CPU BVH dynamic transform should not perform a full rebuild")) {
    return false;
  }

  vkpt::pathtracer::RayQueryHit oldHit;
  const bool oldRayHit = accelerator->intersect(ray, oldHit, nullptr) && oldHit.hit;
  vkpt::pathtracer::Ray movedRay;
  movedRay.origin = {5.0f, 0.25f, 0.0f};
  movedRay.direction = {0.0f, 0.0f, -1.0f};
  vkpt::pathtracer::RayQueryHit movedHit;
  const bool movedRayHit = accelerator->intersect(movedRay, movedHit, nullptr) && movedHit.hit;
  return Check(!oldRayHit,
               "CPU BVH refit should move triangle away from old ray") &&
         Check(movedRayHit,
               "CPU BVH refit should hit triangle at new transform");
}

bool CheckPathtracerCpuRuntimeMetrics() {
  auto& registry = vkpt::core::metrics::MetricsRegistry::instance();
  registry.reset("vkp.pathtracer.");
  registry.reset("vkp.cpu.");

  vkpt::pathtracer::RenderSettings settings;
  settings.width = 16u;
  settings.height = 8u;
  settings.spp = 1u;
  settings.max_depth = 1u;
  settings.deterministic = true;
  settings.seed = 0x123u;
  const auto scene = MakeScene();

  vkpt::pathtracer::ScalarCpuPathTracer scalarTracer;
  if (!Check(ConfigureScalar(scalarTracer, settings, scene),
             "metrics scalar tracer should configure")) {
    return false;
  }
  vkpt::pathtracer::RenderTile fullTile;
  fullTile.x = 0u;
  fullTile.y = 0u;
  fullTile.width = settings.width;
  fullTile.height = settings.height;
  fullTile.sample_index = 0u;
  if (!Check(scalarTracer.render_tile(fullTile, 0u),
             "metrics scalar render should succeed")) {
    return false;
  }

  vkpt::jobs::JobSystem bvhJobs(vkpt::jobs::JobSystemConfig{
      2u,
      vkpt::jobs::WorkerThreadPriority::Background,
      true});
  std::vector<vkpt::cpu::BvhAabb> aabbs;
  aabbs.reserve(8u);
  for (std::uint32_t i = 0u; i < 8u; ++i) {
    vkpt::cpu::BvhAabb aabb{};
    aabb.min[0] = static_cast<float>(i);
    aabb.min[1] = 0.0f;
    aabb.min[2] = -1.0f;
    aabb.max[0] = static_cast<float>(i) + 0.5f;
    aabb.max[1] = 0.5f;
    aabb.max[2] = -0.5f;
    aabbs.push_back(aabb);
  }
  vkpt::cpu::ParallelBvhBuilder builder;
  const auto builtBvh = builder.build(aabbs, &bvhJobs, true, 1u);
  const auto bvhWorkerCount = bvhJobs.worker_count();
  bvhJobs.shutdown();
  if (!Check(!builtBvh.nodes.empty() && builtBvh.prim_indices.size() == aabbs.size(),
             "parallel BVH builder should build telemetry test primitives")) {
    return false;
  }

  const auto pathMetrics = registry.snapshot_prefix("vkp.pathtracer.");
  const auto cpuMetrics = registry.snapshot_prefix("vkp.cpu.");
  const auto events = vkpt::core::log::Logger::instance().dump_crash_rings();
  bool ok = true;
  ok &= CheckHistogramAtLeast(pathMetrics, "vkp.pathtracer.sample_us", 1u);
  ok &= CheckCounterAtLeast(pathMetrics, "vkp.pathtracer.bvh_node_visits", 1u);
  ok &= CheckCounterAtLeast(pathMetrics, "vkp.pathtracer.ray_count", 1u);
  ok &= CheckCounterAtLeast(pathMetrics, "vkp.pathtracer.triangle_tests", 1u);
  ok &= CheckHistogramAtLeast(cpuMetrics, "vkp.cpu.bvh_build_us", 1u);
  ok &= Check(HasCpuSimdSelectedEvent(events),
              "CPU SIMD selection event should be observable");
  ok &= Check(HasBvhBuildCompletedEvent(aabbs.size(), bvhWorkerCount, events),
              "BVH build completion event should be observable");
  return ok;
}

bool CheckCpuPathTracerContract() {
  auto& logger = vkpt::core::log::Logger::instance();
  const auto previousLogLevel = logger.min_level();
  logger.set_min_level(vkpt::core::log::Level::Debug);
  auto restoreLogLevel = [&]() {
    logger.set_min_level(previousLogLevel);
  };

  std::vector<std::string> diagnostics;
  const auto contract = vkpt::cpu::BuildStandardCpuPathTracerContract();
  if (!Check(vkpt::cpu::ValidateStandardCpuPathTracerContract(contract, &diagnostics),
             "standard CPU path tracer contract should validate") ||
      !Check(contract.operations_return_status &&
                 contract.exposes_determinism_context &&
                 contract.naming_uses_cpu_path_tracer_status,
             "standard CPU path tracer contract should pin Status, determinism, and naming") ||
      !Check(contract.state_machine[2].from == vkpt::cpu::CpuPathTracerLifecycle::SceneLoaded &&
                 std::string_view(contract.state_machine[2].operation) ==
                     "build_or_update_acceleration" &&
                 contract.state_machine[2].to == vkpt::cpu::CpuPathTracerLifecycle::Ready,
             "standard CPU path tracer contract should carry acceleration transition")) {
    for (const auto& diagnostic : diagnostics) {
      std::cerr << "cpu contract diagnostic: " << diagnostic << "\n";
    }
    restoreLogLevel();
    return false;
  }

  vkpt::cpu::TiledRenderConfig config;
  config.tile_height = 4u;
  config.worker_count = 1u;
  const auto determinism =
      vkpt::core::MakeDeterminismContext(true, 0xC011u, 19u, "cpu-contract");
  config.set_determinism(determinism);

  TestFlowSource flowSource(0xA11CEu);
  vkpt::cpu::TiledCpuPathTracer tracer(config);
  tracer.set_flow_source(&flowSource);
  vkpt::pathtracer::IPathTracer& iface = tracer;

  const auto scene = MakeScene();
  const auto loadBeforeConfigure = iface.load_scene_snapshot_status(scene);
  if (!Check(loadBeforeConfigure.is_error() &&
                 loadBeforeConfigure.code == vkpt::core::StatusCode::NotReady,
             "tiled CPU load_scene_snapshot should return typed not-ready Status before configure")) {
    restoreLogLevel();
    return false;
  }

  vkpt::pathtracer::RenderSettings settings;
  settings.width = 16u;
  settings.height = 8u;
  settings.spp = 1u;
  settings.max_depth = 1u;
  settings.set_determinism(determinism);

  const auto configure = iface.configure_status(settings);
  const auto buildBeforeLoad = iface.build_or_update_acceleration_status();
  const auto load = iface.load_scene_snapshot_status(scene);
  const auto build = iface.build_or_update_acceleration_status();
  if (!Check(configure.is_ok(),
             "tiled CPU configure should return Status ok") ||
      !Check(buildBeforeLoad.is_error() &&
                 buildBeforeLoad.code == vkpt::core::StatusCode::NotReady,
             "tiled CPU build should return typed not-ready Status before scene load") ||
      !Check(load.is_ok(),
             "tiled CPU load_scene_snapshot should return Status ok after configure") ||
      !Check(build.is_ok(),
             "tiled CPU build should return Status ok after scene load")) {
    restoreLogLevel();
    return false;
  }

  const auto cpuReady = tracer.cpu_status();
  const auto pathReady = iface.status();
  const auto generic = vkpt::cpu::ToSubsystemStatus(cpuReady);
  const auto health = vkpt::cpu::EvaluateCpuPathTracerHealth(cpuReady);
  auto probe = vkpt::cpu::CreateCpuPathTracerHealthProbe(
      [&tracer]() { return tracer.cpu_status(); });
  if (!Check(cpuReady.lifecycle == vkpt::cpu::CpuPathTracerLifecycle::Ready &&
                 cpuReady.configured &&
                 cpuReady.scene_loaded &&
                 cpuReady.accel_valid &&
                 cpuReady.ready_to_render,
             "CpuPathTracerStatus should expose ready lifecycle and state flags") ||
      !Check(cpuReady.deterministic &&
                 cpuReady.determinism_base_seed == determinism.base_seed &&
                 cpuReady.determinism_frame_index == determinism.frame_index &&
                 cpuReady.determinism_scenario_id == determinism.scenario_id,
             "CpuPathTracerStatus should retain DeterminismContext") ||
      !Check(cpuReady.current_flow_id == flowSource.current_flow_id(),
             "CpuPathTracerStatus should expose current flow id") ||
      !Check(generic.name == "cpu" &&
                 generic.status == vkpt::core::contracts::SubsystemHealth::Ok,
             "CpuPathTracerStatus should convert to generic subsystem status") ||
      !Check(health.status == vkpt::core::health::Status::Ok &&
                 probe &&
                 probe->name() == "cpu" &&
                 probe->check().status == vkpt::core::health::Status::Ok,
             "CPU path tracer health probe should report ok for ready tracer") ||
      !Check(pathReady.backend == "tiled-cpu" &&
                 pathReady.lifecycle == vkpt::pathtracer::PathTracerLifecycle::Ready,
             "tiled CPU should bridge CPU status to IPathTracer status")) {
    restoreLogLevel();
    return false;
  }

  vkpt::pathtracer::RenderTile tile;
  tile.x = 0u;
  tile.y = 0u;
  tile.width = settings.width;
  tile.height = settings.height;
  tile.sample_index = 0u;
  tile.tile_id = 0u;
  if (!Check(iface.render_tile(tile, 0u),
             "tiled CPU render_tile should render through status-backed contract")) {
    restoreLogLevel();
    return false;
  }

  const auto rendered = tracer.cpu_status();
  if (!Check(rendered.current_sample == 1u &&
                 rendered.total_samples >=
                     static_cast<std::uint64_t>(settings.width) * settings.height,
             "CpuPathTracerStatus should expose sample progress after rendering") ||
      !Check(rendered.last_tile_us_p99 > 0u,
             "CpuPathTracerStatus should expose tile p99 timing after rendering")) {
    restoreLogLevel();
    return false;
  }

  tracer.shutdown();
  const auto stopped = tracer.cpu_status();
  const auto events = vkpt::core::log::Logger::instance().dump_crash_rings();
  bool ok = true;
  ok &= Check(stopped.lifecycle == vkpt::cpu::CpuPathTracerLifecycle::Uninitialized,
              "CPU path tracer shutdown should return to uninitialized state");
  ok &= Check(HasCpuEventWithFlow(events, "config", flowSource.current_flow_id()),
              "CPU path tracer should emit cpu.config with flow id");
  ok &= Check(HasCpuEventWithFlow(events, "started", flowSource.current_flow_id()),
              "CPU path tracer should emit cpu.started with flow id");
  ok &= Check(HasCpuEventWithFlow(events, "stopped", flowSource.current_flow_id()),
              "CPU path tracer should emit cpu.stopped with flow id");
  ok &= Check(HasCpuEventWithFlow(events, "acceleration_failed", flowSource.current_flow_id()),
              "CPU path tracer should emit anomaly event with flow id");

  restoreLogLevel();
  return ok;
}

bool CheckPathtracerStatusEventsAndResetContract() {
  auto& logger = vkpt::core::log::Logger::instance();
  const auto previousLogLevel = logger.min_level();
  logger.set_min_level(vkpt::core::log::Level::Debug);
  auto restoreLogLevel = [&]() {
    logger.set_min_level(previousLogLevel);
  };

  vkpt::pathtracer::RenderSettings settings;
  settings.width = 16u;
  settings.height = 8u;
  settings.spp = 1u;
  settings.max_depth = 1u;
  settings.deterministic = true;
  settings.seed = 0x234u;
  auto scene = MakeScene();
  vkpt::pathtracer::RTHitLight light;
  light.position = {0.0f, 2.0f, -2.0f};
  light.color = {1.0f, 0.8f, 0.6f};
  light.intensity = 1.0f;
  scene.lights.push_back(light);

  vkpt::pathtracer::ScalarCpuPathTracer tracer;
  vkpt::pathtracer::IPathTracer& iface = tracer;
  const auto initial = iface.status();
  if (!Check(initial.backend == "scalar-cpu",
             "PathTracerStatus should report scalar backend") ||
      !Check(initial.lifecycle == vkpt::pathtracer::PathTracerLifecycle::Uninitialized,
             "new scalar tracer should start uninitialized")) {
    restoreLogLevel();
    return false;
  }

  const auto loadBeforeConfigure = iface.load_scene_snapshot_status(scene);
  if (!Check(loadBeforeConfigure.is_error() &&
                 loadBeforeConfigure.code == vkpt::core::Status::Code::NotReady &&
                 loadBeforeConfigure.message == "load_scene_snapshot called before configure",
             "load_scene_snapshot_status should expose not-ready failure before configure")) {
    restoreLogLevel();
    return false;
  }

  vkpt::pathtracer::NullPathTracer nullTracer;
  vkpt::pathtracer::IPathTracer& nullIface = nullTracer;
  const auto nullConfigure = nullIface.configure_status(settings);
  const auto nullBuildBeforeLoad = nullIface.build_or_update_acceleration_status();
  if (!Check(nullConfigure.is_ok(),
             "configure_status should return ok for null tracer configure") ||
      !Check(nullBuildBeforeLoad.is_error() &&
                 nullBuildBeforeLoad.code == vkpt::core::Status::Code::NotReady &&
                 nullBuildBeforeLoad.message ==
                     "build_or_update_acceleration called before load_scene_snapshot",
             "build_or_update_acceleration_status should expose null tracer build precondition failure")) {
    restoreLogLevel();
    return false;
  }

  const auto contract = vkpt::pathtracer::BuildStandardPathTracerContract();
  std::vector<std::string> diagnostics;
  if (!Check(vkpt::pathtracer::ValidateStandardPathTracerContract(contract, &diagnostics),
             "standard path tracer contract should validate state machine") ||
      !Check(contract.lifecycle.reset_preserves_acceleration,
             "standard path tracer contract should pin film-only reset") ||
      !Check(contract.transforms.plan_is_stable_for_same_inputs,
             "standard path tracer contract should pin stable transform planning") ||
      !Check(contract.transforms.plan_is_commit_free,
             "standard path tracer contract should pin commit-free transform planning") ||
      !Check(contract.state_machine[3].from == vkpt::pathtracer::PathTracerLifecycle::Ready &&
                 std::string_view(contract.state_machine[3].operation) == "reset_accumulation" &&
                 contract.state_machine[3].to == vkpt::pathtracer::PathTracerLifecycle::Ready,
             "standard path tracer contract should carry reset Ready-to-Ready transition")) {
    for (const auto& diagnostic : diagnostics) {
      std::cerr << "contract diagnostic: " << diagnostic << "\n";
    }
    restoreLogLevel();
    return false;
  }

  const auto configureStatus = iface.configure_status(settings);
  const auto loadStatus = iface.load_scene_snapshot_status(scene);
  const auto buildStatus = iface.build_or_update_acceleration_status();
  if (!Check(configureStatus.is_ok(),
             "configure_status should return ok for scalar tracer configure") ||
      !Check(loadStatus.is_ok(),
             "load_scene_snapshot_status should return ok after configure") ||
      !Check(buildStatus.is_ok(),
             "build_or_update_acceleration_status should return ok after scene load") ||
      !Check(tracer.reset_accumulation(),
             "status scalar tracer should reset after status-based setup")) {
    restoreLogLevel();
    return false;
  }
  const auto ready = iface.status();
  auto pathProbe = vkpt::pathtracer::observability::CreatePathTracerHealthProbe(
      [&iface]() { return iface.status(); });
  const auto pathHealth = pathProbe->check();
  const auto genericPathStatus =
      vkpt::pathtracer::observability::ToSubsystemStatus(ready);
  if (!Check(ready.lifecycle == vkpt::pathtracer::PathTracerLifecycle::Ready,
             "configured scalar tracer should be ready") ||
      !Check(ready.scene_loaded && ready.accel_valid && ready.ready_to_render,
             "ready PathTracerStatus should include scene and acceleration flags") ||
      !Check(ready.accumulation_gen == 1u,
             "ConfigureScalar reset should advance accumulation generation") ||
      !Check(ready.total_samples == 0u && ready.current_sample == 0u,
             "ready PathTracerStatus should start with empty accumulation") ||
      !Check(pathProbe->name() == "pathtracer" &&
                 pathHealth.status == vkpt::core::health::Status::Ok,
             "pathtracer health probe should report a ready tracer as ok") ||
      !Check(genericPathStatus.name == "pathtracer" &&
                 genericPathStatus.status ==
                     vkpt::core::contracts::SubsystemHealth::Ok,
             "PathTracerStatus should convert to generic subsystem status")) {
    restoreLogLevel();
    return false;
  }

  vkpt::pathtracer::RenderTile fullTile;
  fullTile.x = 0u;
  fullTile.y = 0u;
  fullTile.width = settings.width;
  fullTile.height = settings.height;
  fullTile.sample_index = 0u;
  if (!Check(tracer.render_tile(fullTile, 0u),
             "status scalar render should succeed")) {
    restoreLogLevel();
    return false;
  }
  const auto rendered = iface.status();
  if (!Check(rendered.current_sample == 1u,
             "PathTracerStatus should track current rendered sample") ||
      !Check(rendered.total_samples >=
                 static_cast<std::uint64_t>(settings.width) * settings.height,
             "PathTracerStatus should expose accumulated sample counters")) {
    restoreLogLevel();
    return false;
  }

  vkpt::pathtracer::RTSceneDeltaUpdate delta;
  auto material = scene.materials[0];
  material.albedo = {0.35f, 0.2f, 0.1f};
  delta.materials.push_back(vkpt::pathtracer::RTMaterialUpdate{0u, material});
  auto changedLight = scene.lights[0];
  changedLight.intensity = 2.0f;
  delta.lights.push_back(vkpt::pathtracer::RTLightUpdate{0u, changedLight});
  delta.environment_color_changed = true;
  delta.environment_color = {0.01f, 0.02f, 0.03f};
  if (!Check(tracer.update_scene_delta(delta),
             "status scalar scene delta should apply")) {
    restoreLogLevel();
    return false;
  }
  const auto afterDelta = iface.status();
  if (!Check(afterDelta.accel_valid && afterDelta.ready_to_render,
             "scene delta should preserve acceleration validity")) {
    restoreLogLevel();
    return false;
  }

  vkpt::pathtracer::RTInstanceTransformUpdate update;
  update.entity_id = 42u;
  update.translation = {0.5f, 0.0f, 0.0f};
  update.rotation = {};
  update.scale = {1.0f, 1.0f, 1.0f};
  update.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform;
  update.transform_revision = 2u;
  const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates = {update};
  auto options = vkpt::pathtracer::MakeStandardTransformUpdateOptions(
      vkpt::pathtracer::RenderUpdateReason::PhysicsMotion,
      0u,
      "status-smoke");
  options.reset_accumulation = false;
  const auto beforeTransformPlan = iface.status();
  const auto transformPlan = tracer.plan_instance_transform_update(updates, options);
  const auto repeatTransformPlan = tracer.plan_instance_transform_update(updates, options);
  const auto afterTransformPlan = iface.status();
  if (!Check(transformPlan.status ==
                 vkpt::pathtracer::InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate,
             "status scalar transform delta should plan dynamic acceleration update") ||
      !CheckEquivalentPlan(transformPlan,
                           repeatTransformPlan,
                           "scalar transform planning should be stable for unchanged inputs") ||
      !Check(afterTransformPlan.accumulation_gen == beforeTransformPlan.accumulation_gen &&
                 afterTransformPlan.current_sample == beforeTransformPlan.current_sample &&
                 afterTransformPlan.total_samples == beforeTransformPlan.total_samples &&
                 afterTransformPlan.accel_valid == beforeTransformPlan.accel_valid &&
                 afterTransformPlan.ready_to_render == beforeTransformPlan.ready_to_render,
             "scalar transform planning should not commit tracer state")) {
    restoreLogLevel();
    return false;
  }
  const auto transformResult = tracer.apply_instance_transform_update(updates, options);
  if (!Check(transformResult.applied(),
             "status scalar transform delta should apply")) {
    restoreLogLevel();
    return false;
  }
  const auto afterTransform = iface.status();
  if (!Check(afterTransform.accel_valid && afterTransform.ready_to_render,
             "transform delta should preserve acceleration validity")) {
    restoreLogLevel();
    return false;
  }

  const auto beforeResetGen = afterTransform.accumulation_gen;
  if (!Check(tracer.reset_accumulation(),
             "status scalar reset should succeed")) {
    restoreLogLevel();
    return false;
  }
  const auto afterReset = iface.status();
  tracer.shutdown();
  const auto afterShutdown = iface.status();
  const auto events = logger.dump_crash_rings();
  restoreLogLevel();

  bool ok = true;
  ok &= Check(afterReset.lifecycle == vkpt::pathtracer::PathTracerLifecycle::Ready,
              "reset_accumulation should keep tracer ready");
  ok &= Check(afterReset.accel_valid && afterReset.ready_to_render,
              "reset_accumulation should preserve acceleration validity");
  ok &= Check(afterReset.current_sample == 0u && afterReset.total_samples == 0u,
              "reset_accumulation should clear current sample and counters");
  ok &= Check(afterReset.accumulation_gen == beforeResetGen + 1u,
              "reset_accumulation should advance accumulation generation");
  ok &= Check(afterShutdown.lifecycle ==
                  vkpt::pathtracer::PathTracerLifecycle::Uninitialized,
              "shutdown should return PathTracerStatus to uninitialized");
  ok &= Check(HasPathTracerEventWithFlow(events, "config", 0u),
              "pathtracer lifecycle config event should include flow_id");
  ok &= Check(HasPathTracerEventWithFlow(events, "started", 0u),
              "pathtracer lifecycle started event should include flow_id");
  ok &= Check(HasPathTracerEventWithFlow(
                  events,
                  "stopped",
                  afterReset.accumulation_gen),
              "pathtracer lifecycle stopped event should include latest flow_id");
  ok &= Check(HasPathTracerAnomaly(events, "load_scene_snapshot"),
              "pathtracer operation failure should emit an anomaly event");
  ok &= Check(HasPathTracerAccumulationResetEvent(
                  events,
                  "transform",
                  afterReset.accumulation_gen),
              "pathtracer.accumulation_reset should include gen and reason");
  ok &= Check(HasPathTracerSceneDeltaEvent(events, 1u, 1u, 0u, true),
              "pathtracer.scene_delta_applied should include material and light counts");
  ok &= Check(HasPathTracerSceneDeltaEvent(events, 0u, 0u, 1u, false),
              "pathtracer.scene_delta_applied should include instance counts");
  return ok;
}

bool CheckRenderCoordinatorTileBudget() {
  auto& registry = vkpt::core::metrics::MetricsRegistry::instance();
  registry.reset("vkp.render.");

  vkpt::pathtracer::RenderSettings settings;
  settings.width = 32u;
  settings.height = 32u;
  settings.spp = 4u;

  vkpt::render::RenderCoordinatorConfig config;
  config.publish_hz = 60u;
  config.immediate_publish_count = settings.spp;
  config.tile_height = 8u;
  config.gpu_count = 2u;
  config.tile_latency_budget_us = 2000.0;

  vkpt::scene::SnapshotRing snapshots;
  config.snapshot_ring = &snapshots;

  vkpt::render::RenderCoordinator coordinator(
      std::make_unique<vkpt::pathtracer::NullPathTracer>(),
      settings,
      MakeScene(),
      config);
  if (!Check(coordinator.start(), "render coordinator should start")) {
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  vkpt::render::RenderCoordinatorStats stats;
  do {
    stats = coordinator.stats();
    if (stats.failed) {
      coordinator.stop();
      return Check(false, "render coordinator should not fail");
    }
    if (stats.sample_count >= settings.spp) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);

  coordinator.stop();
  stats = coordinator.stats();
  const auto renderMetrics = registry.snapshot_prefix("vkp.render.");
  const auto* tileLatency =
      FindMetric(renderMetrics,
                 "vkp.render.tile_latency_us",
                 vkpt::core::metrics::Kind::HistogramKind);
  const auto* tilesRendered =
      FindMetric(renderMetrics,
                 "vkp.render.tiles_rendered_total",
                 vkpt::core::metrics::Kind::CounterKind);
  const auto* overBudget =
      FindMetric(renderMetrics,
                 "vkp.render.tile_latency_over_budget_total",
                 vkpt::core::metrics::Kind::CounterKind);
  const auto* running =
      FindMetric(renderMetrics,
                 "vkp.render.running",
                 vkpt::core::metrics::Kind::GaugeKind);
  const auto* legacyLast =
      FindMetric(renderMetrics,
                 "vkp.render.tile_latency_last_us",
                 vkpt::core::metrics::Kind::GaugeKind);
  const auto* legacyMax =
      FindMetric(renderMetrics,
                 "vkp.render.tile_latency_max_us",
                 vkpt::core::metrics::Kind::GaugeKind);
  return Check(stats.sample_count >= settings.spp,
               "coordinator should finish smoke samples") &&
         Check(stats.tiles_rendered_total >= settings.spp * 4u,
               "coordinator should render row-band tiles") &&
         Check(stats.gpu_tiles_rendered_total.size() == 2u &&
                   stats.gpu_tiles_rendered_total[0] >= settings.spp * 2u &&
                   stats.gpu_tiles_rendered_total[1] >= settings.spp * 2u,
               "coordinator should count scheduled tiles per GPU shard") &&
         Check(stats.tile_latency_over_budget_total == 0u,
               "all smoke tiles should stay under 2 ms") &&
         Check(stats.tile_latency_max_us < config.tile_latency_budget_us,
               "max smoke tile latency should stay under budget") &&
         Check(tileLatency != nullptr &&
                   tileLatency->hist.count >= stats.tiles_rendered_total,
               "render tile latency should be recorded as histogram") &&
         Check(tilesRendered != nullptr &&
                   tilesRendered->counter_value == stats.tiles_rendered_total,
               "render stats should mirror tiles_rendered_total counter") &&
         Check(overBudget != nullptr &&
                   overBudget->counter_value == stats.tile_latency_over_budget_total,
               "render stats should mirror tile latency budget counter") &&
         Check(running != nullptr && running->gauge_value == 0.0,
               "render running gauge should clear after coordinator stop") &&
         Check(legacyLast == nullptr && legacyMax == nullptr,
               "render tile latency last/max gauges should be replaced by histogram");
}

struct FakeRenderHealthRegistry {
  std::shared_ptr<vkpt::core::health::IHealthProbe> probe;

  void register_probe(std::shared_ptr<vkpt::core::health::IHealthProbe> next) {
    probe = std::move(next);
  }
};

struct FakeRenderRepl {
  std::string command_name;
  std::string help_text;
  std::function<std::string(const std::vector<std::string>&)> handler;

  void register_command(
      std::string name,
      std::string help,
      std::function<std::string(const std::vector<std::string>&)> fn) {
    command_name = std::move(name);
    help_text = std::move(help);
    handler = std::move(fn);
  }
};

class FakeFrameGraphContext final : public vkpt::render::IRenderCommandContext {
 public:
  bool fail_dispatch = false;
  std::uint32_t begin_frame_count = 0u;
  std::uint32_t end_frame_count = 0u;
  std::uint32_t begin_pass_count = 0u;
  std::uint32_t end_pass_count = 0u;
  std::uint32_t dispatch_count = 0u;

  bool begin_frame() override {
    ++begin_frame_count;
    return true;
  }
  bool end_frame() override {
    ++end_frame_count;
    return true;
  }
  bool begin_pass(vkpt::render::PassType, std::string_view) override {
    ++begin_pass_count;
    return true;
  }
  bool end_pass() override {
    ++end_pass_count;
    return true;
  }
  bool dispatch(std::uint32_t, std::uint32_t, std::uint32_t) override {
    ++dispatch_count;
    return !fail_dispatch;
  }
  bool copy_buffer_to_texture(vkpt::render::ResourceHandle,
                              vkpt::render::ResourceHandle) override {
    return true;
  }
  bool barrier(vkpt::render::ResourceHandle, std::uint32_t, std::uint32_t) override {
    return true;
  }
};

bool CheckFrameGraphResultContract() {
  vkpt::render::FrameGraph graph;
  const auto write = graph.add_pass("write", vkpt::render::PassType::Compute, {}, {11u});
  const auto read = graph.add_pass("read", vkpt::render::PassType::Compute, {11u}, {12u});
  if (!Check(graph.add_dependency(write, read),
             "frame graph result contract should add dependency")) {
    return false;
  }

  vkpt::render::FrameContext frame;
  frame.viewport_width = 16u;
  frame.viewport_height = 16u;

  FakeFrameGraphContext success_context;
  const auto success = graph.execute(success_context, frame);
  if (!Check(success.is_ok(),
             "frame graph result should report overall success") ||
      !Check(success.per_pass.size() == 2u &&
                 success.per_pass[0].status.is_ok() &&
                 success.per_pass[1].status.is_ok(),
             "frame graph result should report per-pass success") ||
      !Check(success_context.dispatch_count == 2u,
             "frame graph result contract should execute compute passes")) {
    return false;
  }

  FakeFrameGraphContext failing_context;
  failing_context.fail_dispatch = true;
  const auto failure = graph.execute(failing_context, frame);
  if (!Check(failure.is_error(),
             "frame graph result should report failed dispatch") ||
      !Check(failure.per_pass.size() == 1u &&
                 failure.per_pass.front().pass_name == "write" &&
                 failure.per_pass.front().status.is_error(),
             "frame graph result should identify failing pass")) {
    return false;
  }

  std::vector<std::uint32_t> invalid_order = {write, 99u};
  const auto invalid = graph.execute(success_context, frame, &invalid_order);
  return Check(invalid.is_error() &&
                   invalid.overall.code == vkpt::core::StatusCode::InvalidArgument,
               "frame graph result should report invalid explicit execution order");
}

bool CheckRenderCoordinatorStatusHealthAndRepl() {
  vkpt::pathtracer::RenderSettings settings;
  settings.width = 16u;
  settings.height = 16u;
  settings.spp = 2u;

  vkpt::render::RenderCoordinatorConfig config;
  config.publish_hz = 60u;
  config.immediate_publish_count = settings.spp;
  config.tile_height = 8u;
  config.tile_latency_budget_us = 2000.0;

  vkpt::scene::SnapshotRing snapshots;
  config.snapshot_ring = &snapshots;

  vkpt::render::RenderCoordinator coordinator(
      std::make_unique<vkpt::pathtracer::NullPathTracer>(),
      settings,
      MakeScene(),
      config);

  FakeRenderHealthRegistry healthRegistry;
  vkpt::render::RegisterRenderCoordinatorHealthProbe(coordinator, healthRegistry);
  if (!Check(static_cast<bool>(healthRegistry.probe),
             "render health probe should register") ||
      !Check(healthRegistry.probe->name() == "render",
             "render health probe should be named render")) {
    return false;
  }

  FakeRenderRepl repl;
  vkpt::render::RegisterRenderCoordinatorRepl(coordinator, repl);
  if (!Check(repl.command_name == "render",
             "render REPL handler should register render command") ||
      !Check(repl.help_text.find("render status") != std::string::npos,
             "render REPL handler help should mention render status") ||
      !Check(static_cast<bool>(repl.handler),
             "render REPL handler should install a callable")) {
    return false;
  }
  const auto usage = repl.handler({});
  if (!Check(usage.find("usage: render status") != std::string::npos,
             "render REPL handler should reject unknown render subcommands")) {
    return false;
  }

  const auto initialProbe = healthRegistry.probe->check();
  if (!Check(initialProbe.status == vkpt::core::health::Status::Ok,
             "render health probe should be ok before start")) {
    return false;
  }

  if (!Check(coordinator.start(), "status coordinator should start")) {
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  vkpt::render::RenderCoordinatorStatus status;
  do {
    status = coordinator.status();
    if (status.sample_count >= settings.spp && status.frames_published_total >= 1u) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);

  status = coordinator.status();
  const auto genericStatus = vkpt::render::ToSubsystemStatus(status);
  const auto replStatus = repl.handler({"status"});
  const auto runningProbe = healthRegistry.probe->check();
  coordinator.stop();
  const auto stoppedStatus = coordinator.status();

  bool ok = true;
  ok &= Check(status.health == vkpt::core::contracts::SubsystemHealth::Ok,
              "render status should be healthy during normal smoke render");
  ok &= Check(status.lifecycle == vkpt::render::RenderCoordinatorLifecycle::Running,
              "render status should expose running lifecycle");
  ok &= Check(status.last_tick_ns != 0u && status.ticks_total != 0u,
              "render status should expose tick timestamps and count");
  ok &= Check(status.sample_count >= settings.spp &&
                  status.target_sample_count == settings.spp,
              "render status should expose current and target sample counts");
  ok &= Check(status.snapshot_generation >= status.generation,
              "render status should expose latest snapshot generation");
  ok &= Check(status.frames_published_total >= 1u &&
                  status.last_frame_published_ns != 0u,
              "render status should expose frame publish progress");
  ok &= Check(status.tile_budget == vkpt::render::RenderTileBudgetState::WithinBudget,
              "render status should expose tile budget state");
  ok &= Check(genericStatus.name == "render" &&
                  genericStatus.status == vkpt::core::contracts::SubsystemHealth::Ok &&
                  genericStatus.last_tick_ns == status.last_tick_ns,
              "render status should convert to generic subsystem status");
  ok &= Check(replStatus.find("render status: ok") != std::string::npos &&
                  replStatus.find("gen_lag:") != std::string::npos &&
                  replStatus.find("tile_budget:") != std::string::npos,
              "render status REPL handler should print status fields");
  ok &= Check(runningProbe.status == vkpt::core::health::Status::Ok,
              "render health probe should return ok for normal render status");
  ok &= Check(stoppedStatus.lifecycle == vkpt::render::RenderCoordinatorLifecycle::Stopped &&
                  stoppedStatus.health == vkpt::core::contracts::SubsystemHealth::Ok,
              "render status should be stopped and healthy after stop");

  vkpt::render::RenderCoordinator failedStartCoordinator(
      std::unique_ptr<vkpt::pathtracer::IPathTracer>{},
      settings,
      MakeScene(),
      config);
  const auto failedStart = failedStartCoordinator.start();
  const auto failedStartStatus = failedStartCoordinator.status();
  const auto failedStartRepl = vkpt::render::FormatRenderCoordinatorStatus(failedStartStatus);
  ok &= Check(failedStart.is_error() &&
                  failedStart.code == vkpt::core::StatusCode::NotReady &&
                  failedStart.message == "render coordinator has no tracer",
              "render start should return typed Status on initialization failure");
  ok &= Check(failedStartStatus.error_history.size() == 1u &&
                  failedStartStatus.error_history.front() ==
                      "render coordinator has no tracer",
              "render status should expose bounded error history");
  ok &= Check(failedStartRepl.find("error_history: 1") != std::string::npos,
              "render REPL status should expose error-history count");

  vkpt::render::RenderCoordinatorStatus degraded;
  degraded.lifecycle = vkpt::render::RenderCoordinatorLifecycle::Running;
  degraded.observed_at_ns = 2'000'000'000ull;
  degraded.tracer_gen_lag = 6u;
  const auto degradedReport = vkpt::render::EvaluateRenderCoordinatorHealth(degraded);
  ok &= Check(degradedReport.status == vkpt::core::health::Status::Degraded,
              "render health should degrade when gen_lag exceeds 5");

  vkpt::render::RenderCoordinatorStatus failedNoFrame;
  failedNoFrame.lifecycle = vkpt::render::RenderCoordinatorLifecycle::Running;
  failedNoFrame.observed_at_ns = 2'000'000'001ull;
  failedNoFrame.work_available = true;
  failedNoFrame.work_available_since_ns = 1u;
  const auto failedNoFrameReport =
      vkpt::render::EvaluateRenderCoordinatorHealth(failedNoFrame);
  ok &= Check(failedNoFrameReport.status == vkpt::core::health::Status::Failed,
              "render health should fail when no frame publishes for 1s of work");

  vkpt::render::RenderCoordinatorStatus failedError;
  failedError.lifecycle = vkpt::render::RenderCoordinatorLifecycle::Failed;
  failedError.last_error = "boom";
  const auto failedErrorReport =
      vkpt::render::EvaluateRenderCoordinatorHealth(failedError);
  ok &= Check(failedErrorReport.status == vkpt::core::health::Status::Failed &&
                  failedErrorReport.reason == "boom",
              "render health should fail with coordinator last_error");

  return ok;
}

bool CheckStaticGenerationsNoReset() {
  auto scene = MakeScene();
  vkpt::pathtracer::RenderSettings settings;
  settings.width = 16u;
  settings.height = 16u;
  settings.spp = 10000u;

  vkpt::render::RenderCoordinatorConfig config;
  config.publish_hz = 60u;
  config.immediate_publish_count = 0u;
  config.tile_height = 4u;
  config.tile_latency_budget_us = 2000.0;

  vkpt::scene::SnapshotRing snapshots;
  config.snapshot_ring = &snapshots;

  vkpt::render::RenderCoordinator coordinator(
      std::make_unique<vkpt::pathtracer::NullPathTracer>(),
      settings,
      scene,
      config);
  if (!Check(coordinator.start(), "static generation coordinator should start")) {
    return false;
  }

  vkpt::scene::RenderSceneSnapshot::Ptr previous;
  const auto snapshotDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  do {
    previous = snapshots.current();
    if (previous) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < snapshotDeadline);

  if (!previous) {
    coordinator.stop();
    return Check(false, "coordinator should publish initial snapshot");
  }

  vkpt::scene::RenderSceneSnapshotRevisions rev;
  rev.topology_revision = previous->topology_revision;
  rev.transform_revision = previous->transform_revision;
  rev.camera_revision = previous->camera_revision;
  rev.material_revision = previous->material_revision;
  const auto startGeneration = previous->generation;
  for (std::uint64_t gen = startGeneration + 1u;
       gen <= startGeneration + 120u;
       ++gen) {
    rev.generation = gen;
    previous = vkpt::scene::BuildRenderSceneSnapshot(scene, previous.get(), rev);
    snapshots.publish(previous);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  const auto targetGeneration = previous->generation;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  vkpt::render::RenderCoordinatorStats stats;
  do {
    stats = coordinator.stats();
    if (stats.failed || stats.generation >= targetGeneration) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);

  coordinator.stop();
  stats = coordinator.stats();
  return Check(!stats.failed, "static generation coordinator should not fail") &&
         Check(stats.generation >= targetGeneration,
               "coordinator should observe 100+ static snapshot generations") &&
         Check(stats.sample_count > 0u,
               "samples should continue accumulating across static generations") &&
         Check(stats.snapshot_reset_total == 0u,
               "static snapshot generations should not reset accumulation");
}

bool CheckCoordinatorTransformFallbackAndFirstTilePublish() {
  auto& logger = vkpt::core::log::Logger::instance();
  const auto previousLogLevel = logger.min_level();
  logger.set_min_level(vkpt::core::log::Level::Debug);

  auto scene = MakeScene();
  vkpt::pathtracer::RenderSettings settings;
  settings.width = 16u;
  settings.height = 16u;
  settings.spp = 10000u;

  vkpt::render::RenderCoordinatorConfig config;
  config.publish_hz = 1000u;
  config.immediate_publish_count = 0u;
  config.tile_height = 4u;
  config.tile_latency_budget_us = 2000.0;

  vkpt::scene::SnapshotRing snapshots;
  config.snapshot_ring = &snapshots;

  vkpt::render::RenderCoordinator coordinator(
      std::make_unique<vkpt::pathtracer::NullPathTracer>(),
      settings,
      scene,
      config);
  if (!Check(coordinator.start(), "fallback coordinator should start")) {
    logger.set_min_level(previousLogLevel);
    return false;
  }

  vkpt::scene::RenderSceneSnapshot::Ptr previous;
  const auto initialDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  do {
    previous = snapshots.current();
    if (previous) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < initialDeadline);

  if (!previous) {
    coordinator.stop();
    logger.set_min_level(previousLogLevel);
    return Check(false, "fallback coordinator should publish initial snapshot");
  }

  const auto baselineDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  vkpt::render::RenderCoordinatorStats baselineStats;
  do {
    baselineStats = coordinator.stats();
    if (baselineStats.failed ||
        (baselineStats.generation >= previous->generation &&
         baselineStats.tiles_rendered_total > 0u)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < baselineDeadline);

  if (baselineStats.failed ||
      baselineStats.generation < previous->generation ||
      baselineStats.tiles_rendered_total == 0u) {
    coordinator.stop();
    logger.set_min_level(previousLogLevel);
    return Check(false, "fallback coordinator should consume initial snapshot before transform publish");
  }

  vkpt::scene::RenderSceneSnapshotRevisions rev;
  rev.generation = previous->generation + 1u;
  rev.topology_revision = previous->topology_revision;
  rev.transform_revision = previous->transform_revision + 1u;
  rev.camera_revision = previous->camera_revision;
  rev.material_revision = previous->material_revision;
  auto moved = vkpt::scene::BuildRenderSceneSnapshot(scene, previous.get(), rev);
  if (!Check(moved->instance_motion.empty(),
             "fallback coordinator fixture should publish no motion vectors")) {
    coordinator.stop();
    logger.set_min_level(previousLogLevel);
    return false;
  }
  snapshots.publish(moved);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  vkpt::render::RenderCoordinatorStats stats;
  do {
    stats = coordinator.stats();
    if (stats.failed ||
        (stats.generation >= moved->generation &&
         stats.snapshot_reset_total >= 1u &&
         stats.snapshot_first_tile_publish_total >= 1u)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);

  const auto frame = coordinator.acquire_latest_frame();
  coordinator.stop();
  stats = coordinator.stats();
  const bool sawFramePublished = HasRenderEvent("frame_published");
  const bool sawFrameDropped = HasRenderDropReason("framering_full");
  const bool sawSnapshotConsumed = HasRenderEvent("snapshot_consumed");
  const bool sawStopped = HasRenderEventWithFlow("stopped", stats.generation);
  logger.set_min_level(previousLogLevel);
  return Check(!stats.failed, "fallback coordinator should not fail") &&
         Check(stats.generation >= moved->generation,
               "fallback coordinator should consume transform snapshot") &&
         Check(stats.snapshot_reset_total >= 1u,
               "transform snapshot without motion vectors should fall back to full reset") &&
         Check(stats.snapshot_first_tile_publish_total >= 1u,
               "snapshot change should publish after first rendered tile") &&
         Check(stats.snapshot_first_tile_publish_max_tiles == 1u,
               "snapshot change should need no more than one tile before publish") &&
         Check(frame && frame->generation >= moved->generation,
               "latest display frame should reflect moved snapshot generation") &&
         Check(sawFramePublished,
               "render handoff should emit frame_published event") &&
         Check(sawFrameDropped,
               "render handoff should emit frame_dropped event with reason") &&
         Check(sawSnapshotConsumed,
               "render coordinator should emit snapshot_consumed event") &&
         Check(sawStopped,
               "render coordinator should emit stopped lifecycle event with flow_id") &&
         Check(HasRenderEvent("snapshot_consumed"),
               "render snapshot event should remain observable");
}

}  // namespace

bool CheckLazyPathTracerSceneRace() {
  // Repro for the lazy-init race that crashed FPS demo right-click ADS:
  // the COW path leaves path_tracer_scene null on a camera-only delta, so the
  // first concurrent readers all enter the build path. Before the CAS fix, the
  // racing builders' plain stores trampled each other and the loser's local
  // shared_ptr was the sole owner of its build, dangling the returned const&
  // when it went out of scope.
  for (int trial = 0; trial < 32; ++trial) {
    auto scene = MakeScene();
    vkpt::scene::RenderSceneSnapshotRevisions rev{};
    rev.generation = 1u;
    auto base = vkpt::scene::BuildRenderSceneSnapshot(scene, nullptr, rev);
    if (!Check(base != nullptr, "base snapshot must build")) {
      return false;
    }
    ++rev.generation;
    ++rev.camera_revision;
    scene.camera_position.x = 0.5f + 0.001f * static_cast<float>(trial);
    auto cameraOnly = vkpt::scene::BuildRenderSceneSnapshot(scene, base.get(), rev);
    if (!Check(cameraOnly != nullptr, "camera-only snapshot must build")) {
      return false;
    }
    if (!Check(!cameraOnly->path_tracer_scene.load(std::memory_order_acquire),
               "COW camera-change path should leave path_tracer_scene null for lazy build")) {
      return false;
    }

    constexpr int kThreads = 16;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<const vkpt::pathtracer::PathTracerSceneSnapshot*> seen(kThreads, nullptr);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
      threads.emplace_back([&, i]() {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (!go.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        const auto& view = cameraOnly->path_tracer_scene_snapshot();
        // Touch fields to force the CPU to actually read through the reference;
        // a dangling read here would crash or read garbage.
        volatile std::size_t triangles = view.indices.size();
        (void)triangles;
        seen[static_cast<std::size_t>(i)] = &view;
      });
    }
    while (ready.load(std::memory_order_acquire) < kThreads) {
      std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads) {
      t.join();
    }
    const auto* expected = seen.front();
    if (!Check(expected != nullptr, "lazy build must populate path_tracer_scene")) {
      return false;
    }
    for (const auto* observed : seen) {
      if (!Check(observed == expected,
                 "all concurrent readers must observe the same path_tracer_scene")) {
        return false;
      }
    }
    auto stored = cameraOnly->path_tracer_scene.load(std::memory_order_acquire);
    if (!Check(stored.get() == expected,
               "atomic must hold the same pointer all readers observed")) {
      return false;
    }
  }
  return true;
}

int main() {
  auto run = [](const char* name, bool (*fn)()) {
    if (fn()) {
      return true;
    }
    std::cerr << "snapshot_bus_smoke: section failed: " << name << "\n";
    return false;
  };

  if (!run("deterministic snapshot output hashes", CheckDeterministicSnapshotOutputsHashStream) ||
      !run("copy-on-write snapshot reuse", CheckCowReuse) ||
      !run("lazy path_tracer_scene race", CheckLazyPathTracerSceneRace) ||
      !run("history camera sweep validation", CheckHistoryCameraSweepValidation) ||
      !run("one-mover history validation", CheckOneMoverHistoryValidation) ||
      !run("material reshade invalidation", CheckMaterialReshadeInvalidation) ||
      !run("sim tick snapshot publish", CheckSimTickSnapshotPublish) ||
      !run("snapshot ring readers", CheckRingReaders) ||
      !run("snapshot ring fuzz", CheckRingFuzz) ||
      !run("tile scheduler", CheckTileScheduler) ||
      !run("tile scheduler priority", CheckTileSchedulerPriority) ||
      !run("tile scheduler foveated", CheckTileSchedulerFoveated) ||
      !run("scalar render tile primitive", CheckScalarRenderTilePrimitive) ||
      !run("CPU BVH dynamic transform refit", CheckCpuBvhDynamicTransformRefit) ||
      !run("pathtracer CPU runtime metrics", CheckPathtracerCpuRuntimeMetrics) ||
      !run("CPU path tracer contract", CheckCpuPathTracerContract) ||
      !run("pathtracer status events and reset contract",
           CheckPathtracerStatusEventsAndResetContract) ||
      !run("render coordinator tile budget", CheckRenderCoordinatorTileBudget) ||
      !run("frame graph result contract", CheckFrameGraphResultContract) ||
      !run("render coordinator status health and REPL",
           CheckRenderCoordinatorStatusHealthAndRepl) ||
      !run("static generations no reset", CheckStaticGenerationsNoReset) ||
      !run("coordinator transform fallback and first tile publish",
           CheckCoordinatorTransformFallbackAndFirstTilePublish)) {
    return 1;
  }
  std::cout << "snapshot_bus_smoke: ok\n" << std::flush;
  // This broad smoke exercises process-lifetime registries and thread-local
  // loggers without running the full app shutdown path. All checked objects
  // have already stopped explicitly, so avoid unrelated CRT/static teardown.
  std::quick_exit(0);
}
