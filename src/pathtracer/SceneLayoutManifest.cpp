#include "pathtracer/SceneLayoutManifest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vkpt::pathtracer {

vkpt::core::Result<RTSceneLayoutManifest> BuildRTSceneDataLayoutManifest(
    std::vector<std::string>* diagnostics) {
  if (diagnostics) {
    diagnostics->clear();
  }

  const auto sceneResult = BuildSceneDataFromDocument(vkpt::scene::SceneDocument{});
  if (!sceneResult) {
    if (diagnostics) {
      diagnostics->push_back("failed to construct fallback scene data");
    }
    return vkpt::core::Result<RTSceneLayoutManifest>::error(vkpt::core::ErrorCode::Internal);
  }

  const auto& scene = sceneResult.value();

  auto align_up = [](std::size_t value, std::size_t alignment) {
    if (alignment == 0u) {
      return value;
    }
    const std::size_t mask = alignment - 1u;
    return (value + mask) & ~mask;
  };

  auto append_field = [&](std::vector<GpuLayoutField>& fields,
                        std::size_t& cpu_cursor,
                        std::size_t& gpu_cursor,
                        const std::string& struct_name,
                        const std::string& field,
                        std::size_t element_size,
                        std::size_t element_count,
                        std::size_t alignment) {
    GpuLayoutField out;
    out.struct_name = struct_name;
    out.field = field;
    out.cpu_alignment = alignment;
    out.gpu_alignment = alignment;
    out.cpu_size = element_size * element_count;
    out.gpu_size = element_size * element_count;
    cpu_cursor = align_up(cpu_cursor, std::max<std::size_t>(1u, alignment));
    gpu_cursor = align_up(gpu_cursor, std::max<std::size_t>(1u, alignment));
    out.cpu_offset = cpu_cursor;
    out.gpu_offset = gpu_cursor;
    cpu_cursor += out.cpu_size;
    gpu_cursor += out.gpu_size;
    fields.push_back(std::move(out));
  };

  RTSceneLayoutManifest manifest;
  manifest.schema_version = "1.0";

  std::size_t cpuCursor = 0u;
  std::size_t gpuCursor = 0u;
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_position", sizeof(Vec3), 1u, alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_target", sizeof(Vec3), 1u, alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_up", sizeof(Vec3), 1u, alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_fov_deg", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_focal_length_mm", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_sensor_width_mm", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_sensor_height_mm", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_aperture_radius", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_focus_distance", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_f_stop", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_shutter_seconds", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_iso", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_exposure_compensation", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_white_balance_kelvin", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_iris_blade_count", sizeof(std::uint32_t), 1u, alignof(std::uint32_t));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_iris_rotation_degrees", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_iris_roundness", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_anamorphic_squeeze", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "environment_color", sizeof(Vec3), 1u, alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "materials", sizeof(RTMaterial), scene.materials.size(), alignof(RTMaterial));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "vertices", sizeof(Vec3), scene.vertices.size(), alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "texcoords", sizeof(Vec2), scene.texcoords.size(), alignof(Vec2));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "indices", sizeof(std::uint32_t), scene.indices.size(), alignof(std::uint32_t));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "local_vertices", sizeof(Vec3), scene.local_vertices.size(), alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "local_indices", sizeof(std::uint32_t), scene.local_indices.size(), alignof(std::uint32_t));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "instances", sizeof(RTInstance), scene.instances.size(), alignof(RTInstance));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "tessellation_requests", sizeof(RTTessellationRequest), scene.tessellation_requests.size(), alignof(RTTessellationRequest));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "sdf_primitives", sizeof(RTSdfPrimitive), scene.sdf_primitives.size(), alignof(RTSdfPrimitive));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "lights", sizeof(RTHitLight), scene.lights.size(), alignof(RTHitLight));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "textures_count", sizeof(std::uint64_t), 1u, alignof(std::uint64_t));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "texture_names", sizeof(char), 0u, alignof(char));

  manifest.total_cpu_bytes = cpuCursor;
  manifest.total_gpu_bytes = gpuCursor;

  if (diagnostics) {
    diagnostics->push_back("layout manifest built from fallback scene template");
  }
  return vkpt::core::Result<RTSceneLayoutManifest>::ok(std::move(manifest));
}

std::string SerializeRTSceneDataLayoutManifest(const RTSceneLayoutManifest& manifest) {
  std::ostringstream out;
  out << "{";
  out << "\"schema_version\":\"" << manifest.schema_version << "\",";
  out << "\"total_cpu_bytes\":" << manifest.total_cpu_bytes << ",";
  out << "\"total_gpu_bytes\":" << manifest.total_gpu_bytes << ",";
  out << "\"fields\":[";
  for (std::size_t i = 0; i < manifest.fields.size(); ++i) {
    const auto& field = manifest.fields[i];
    if (i > 0u) {
      out << ",";
    }
    out << "{";
    out << "\"struct_name\":\"" << field.struct_name << "\",";
    out << "\"field\":\"" << field.field << "\",";
    out << "\"cpu_offset\":" << field.cpu_offset << ",";
    out << "\"cpu_size\":" << field.cpu_size << ",";
    out << "\"cpu_alignment\":" << field.cpu_alignment << ",";
    out << "\"gpu_offset\":" << field.gpu_offset << ",";
    out << "\"gpu_size\":" << field.gpu_size << ",";
    out << "\"gpu_alignment\":" << field.gpu_alignment;
    out << "}";
  }
  out << "]";
  out << "}";
  return out.str();
}

}  // namespace vkpt::pathtracer
