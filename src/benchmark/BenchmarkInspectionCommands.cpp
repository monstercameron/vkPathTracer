#include "benchmark/BenchmarkRuntime.h"
#include "benchmark/BenchmarkRuntimeInternal.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "benchmark/BenchmarkSchema.h"
#include "cpu/CpuFeatures.h"
#include "cpu/SimdKernel.h"
#include "materials/MaterialDescriptors.h"
#include "pathtracer/PathTracer.h"
#include "render/backends/BackendFactory.h"
#include "render/interface/RenderContracts.h"
#include "scene/Scene.h"

namespace vkpt::benchmark::ptbench {

using Path = std::filesystem::path;

void PrintHelp() {
  std::cout << "ptbench <command> [options]\n\n";
  std::cout << "commands:\n";
  std::cout << "  run\n";
  std::cout << "  echo-desc\n";
  std::cout << "  list-scenes\n";
  std::cout << "  list-backends\n";
  std::cout << "  list-renderer-paths\n";
  std::cout << "  validate-scene\n";
  std::cout << "  validate-artifacts\n";
  std::cout << "  compare\n";
  std::cout << "  dump-capabilities\n";
  std::cout << "  run-experiments\n";
  std::cout << "  backend-experiments\n";
  std::cout << "  gpu-mem-pressure\n";
  std::cout << "  material-coverage\n";
  std::cout << "  shader-matrix\n";
  std::cout << "  release-check\n";
  std::cout << "  thread-sweep\n";
  std::cout << "  simd-sweep\n";
  std::cout << "  tile-sweep\n\n";
  std::cout << "run:\n";
  std::cout << "  --desc <benchmark-run.json>\n";
  std::cout << "  --echo-desc              (parse and echo resolved descriptor, no render)\n";
  std::cout << "  --scene <path>\n";
  std::cout << "  --backend <cpu|vulkan|d3d12|d3d12-dxr|auto>\n";
  std::cout << "  --renderer-path <cpu-scalar|cpu-tiled|gpu-compute|d3d12-compute|dxr>\n";
  std::cout << "  --resolution <WxH>\n";
  std::cout << "  --spp <samples>\n";
  std::cout << "  --seed <value>\n";
  std::cout << "  --max-depth <value>\n";
  std::cout << "  --duration <seconds>\n";
  std::cout << "  --warmup-frames <count>\n";
  std::cout << "  --reference-image <path>\n";
  std::cout << "  --tolerance-policy <policy> (e.g. abs=0.001,rel=0.01)\n";
  std::cout << "  --output <artifact-dir>\n";
  std::cout << "  --workers <count>        (cpu-tiled: thread count, 0=auto)\n";
  std::cout << "  --tile-size <rows>       (cpu-tiled: rows per tile, default 16)\n";
  std::cout << "  --deterministic          (cpu-tiled: serialized execution)\n\n";
  std::cout << "echo-desc:\n";
  std::cout << "  --desc <benchmark-run.json>\n\n";
  std::cout << "validate-artifacts:\n";
  std::cout << "  --dir <artifact-dir>\n";
  std::cout << "  [--json]\n\n";
  std::cout << "thread-sweep:\n";
  std::cout << "  --scene <path>\n";
  std::cout << "  [--workers <list>]       (comma list, default available 1/2/4/8)\n";
  std::cout << "  [--spp <n>]              (samples per pixel, default 2)\n";
  std::cout << "  [--resolution <WxH>]     (default 128x128)\n";
  std::cout << "  [--output <path>]        (write thread_sweep.json to dir)\n\n";
  std::cout << "simd-sweep:\n";
  std::cout << "  [--rays <count>]         (rays per triangle test, default 1000000)\n";
  std::cout << "  [--triangles <count>]    (triangle count, default 1024)\n";
  std::cout << "  [--output <path>]        (write simd_sweep.json to dir)\n\n";
  std::cout << "tile-sweep:\n";
  std::cout << "  --scene <path>\n";
  std::cout << "  [--workers <count>]      (thread count, 0=auto)\n";
  std::cout << "  [--spp <n>]              (samples per pixel, default 4)\n";
  std::cout << "  [--resolution <WxH>]     (default 128x128)\n";
  std::cout << "  [--output <path>]        (write tile_sweep.json to dir)\n\n";
  std::cout << "validate-scene:\n";
  std::cout << "  --scene <path>\n";
  std::cout << "  [--backend <cpu|vulkan|auto>]\n";
  std::cout << "  [--renderer-path <cpu-scalar|gpu-compute>]\n";
  std::cout << "  [--json]\n\n";
  std::cout << "compare:\n";
  std::cout << "  --reference <path>\n";
  std::cout << "  --image <path>\n";
  std::cout << "  --output <path>\n";
  std::cout << "  [--tolerance-policy <policy>]\n";
  std::cout << "  [--disable-heatmap]\n";

  std::cout << "\ngpu-mem-pressure:\n";
  std::cout << "  [--max-mb <n>]          (default 512)\n";
  std::cout << "  [--step-mb <n>]         (default 64)\n";
  std::cout << "  [--output <path>]       (default artifacts/experiments)\n\n";

  std::cout << "material-coverage:\n";
  std::cout << "  [--output <path>]       (default artifacts/experiments/material_coverage.json)\n";
  std::cout << "  [--json]\n\n";

  std::cout << "shader-matrix:\n";
  std::cout << "  [--output <path>]       (default artifacts/experiments)\n\n";

  std::cout << "backend-experiments:\n";
  std::cout << "  [--output <path>]       (default artifacts/experiments)\n\n";

  std::cout << "release-check:\n";
  std::cout << "  [--scene-pack <dir>]    (default assets/scenes)\n";
  std::cout << "  [--output <path>]       (default artifacts/release_check)\n";
}


int ListScenesCommand() {
  const Path sceneDir("assets/scenes");
  std::error_code ec;
  if (!std::filesystem::exists(sceneDir, ec) || ec) {
    std::cerr << "scene directory missing: " << sceneDir.string() << "\n";
    return 1;
  }
  std::cout << "scenes:\n";
  for (std::filesystem::directory_iterator it(sceneDir, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       it != end;
       it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    const auto& entry = *it;
    std::error_code entryEc;
    if (!entry.is_regular_file(entryEc) || entryEc) {
      continue;
    }
    if (entry.path().extension() != ".json") {
      continue;
    }
    std::cout << "  " << entry.path().filename().string() << "\n";
  }
  return 0;
}

int ListBackendsCommand() {
  const auto names = vkpt::render::AvailableBackendNames();
  for (const auto& name : names) {
    auto backend = vkpt::render::CreateBackend(name);
    if (!backend) {
      std::cout << name << " unavailable\n";
      continue;
    }
    if (!backend->initialize().is_ok()) {
      std::cout << name << " failed initialize\n";
      continue;
    }
    const auto caps = backend->capabilities();
    std::cout << name << "\n";
    std::cout << "  " << vkpt::render::SerializeBackendCapabilities(caps) << "\n";
  }
  return 0;
}

int ListRendererPathsCommand(const std::vector<std::string_view>& args) {
  std::string backend = "auto";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--backend") {
      if (i + 1 < args.size()) {
        backend = std::string(args[i + 1]);
      }
      ++i;
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  const auto rendererPaths = AvailableRendererPaths(backend);
  std::cout << "renderer-paths:\n";
  for (const auto& path : rendererPaths) {
    std::cout << "  " << path << "\n";
  }
  return 0;
}

int ValidateSceneCommand(const std::vector<std::string_view>& args) {
  std::string scenePath;
  std::string backend = "cpu";
  std::string renderer = "cpu-scalar";
  bool json = false;

  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--scene") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --scene\n";
        return 1;
      }
      scenePath = std::string(args[++i]);
    } else if (args[i] == "--backend") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --backend\n";
        return 1;
      }
      backend = std::string(args[++i]);
    } else if (args[i] == "--renderer-path") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --renderer-path\n";
        return 1;
      }
      renderer = std::string(args[++i]);
    } else if (args[i] == "--json") {
      json = true;
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  if (scenePath.empty()) {
    std::cerr << "missing --scene\n";
    return 1;
  }

  const auto result = vkpt::scene::SceneDocument::load_from_file(scenePath);
  if (!result) {
    if (json) {
      std::cout << "{\"ok\":false,\"scene\":\"" << EscapeJson(scenePath) << "\",\"issues\":[\"load failed\"]}\n";
    } else {
      std::cerr << "validate failed: cannot load " << scenePath << "\n";
    }
    return 2;
  }

  std::vector<std::string> issues;
  const bool validScene = result.value().validate(&issues);
  std::string backendError;
  const bool validBackend = ValidateBackendRenderer(backend, renderer, &backendError);
  const auto snapshot = result.value().snapshot();
  const std::size_t entityCount = snapshot.entity_ids.size();
  const std::size_t cameraCount = snapshot.camera ? 1u : 0u;
  const bool lights = !snapshot.lights.empty();
  const bool materials = !result.value().materials.empty();
  const bool benchmarkSettings = result.value().benchmark.enabled && result.value().benchmark.frame_target > 0u;
  if (!validBackend && !backendError.empty()) {
    issues.push_back(backendError);
  }
  if (result.value().metadata.schema.empty()) {
    issues.push_back("metadata.schema is empty");
  }
  if (result.value().export_hash_hex().empty()) {
    issues.push_back("missing scene hash");
  }
  if (entityCount == 0) {
    issues.push_back("no entities");
  }
  if (cameraCount == 0) {
    issues.push_back("no camera");
  }
  if (!lights) {
    issues.push_back("no lights");
  }
  if (!materials) {
    issues.push_back("no materials");
  }
  if (!benchmarkSettings) {
    issues.push_back("benchmark settings are disabled or missing frame_target");
  }
  if (result.value().benchmark.warmup_frames > result.value().benchmark.frame_target &&
      result.value().benchmark.frame_target > 0u) {
    issues.push_back("benchmark warmup_frames exceeds frame_target");
  }
  std::unordered_set<std::uint64_t> materialIds;
  for (const auto& material : result.value().materials) {
    materialIds.insert(material.id);
  }
  for (const auto& geometry : result.value().geometry) {
    if (geometry.material_id != 0u && materialIds.find(geometry.material_id) == materialIds.end()) {
      issues.push_back("geometry references missing material " + std::to_string(geometry.material_id));
    }
  }
  for (const auto& renderable : snapshot.renderables) {
    if (renderable.material_id != 0u && materialIds.find(renderable.material_id) == materialIds.end()) {
      issues.push_back("renderable references missing material " + std::to_string(renderable.material_id));
    }
  }
  std::unordered_set<std::string> assetUris;
  for (const auto& asset : result.value().assets) {
    if (asset.uri.empty()) {
      issues.push_back("asset has empty uri");
    }
    if (!asset.uri.empty() && !assetUris.insert(asset.uri).second) {
      issues.push_back("duplicate asset uri " + asset.uri);
    }
  }

  const bool ok = validScene && validBackend && benchmarkSettings && issues.empty();
  if (json) {
    std::ostringstream out;
    out << "{";
    out << "\"ok\":" << (ok ? "true" : "false") << ",";
    out << "\"scene\":\"" << EscapeJson(scenePath) << "\",";
    out << "\"schema_version\":\"" << EscapeJson(result.value().metadata.schema) << "\",";
    out << "\"asset_count\":" << result.value().assets.size() << ",";
    out << "\"material_count\":" << result.value().materials.size() << ",";
    out << "\"entity_count\":" << entityCount << ",";
    out << "\"camera_count\":" << cameraCount << ",";
    out << "\"has_lights\":" << (lights ? "true" : "false") << ",";
    out << "\"has_materials\":" << (materials ? "true" : "false") << ",";
    out << "\"benchmark_settings\":" << (benchmarkSettings ? "true" : "false") << ",";
    out << "\"backend_compatible\":" << (validBackend ? "true" : "false") << ",";
    out << "\"issues\":[";
    for (std::size_t i = 0; i < issues.size(); ++i) {
      if (i) out << ",";
      out << "\"" << EscapeJson(issues[i]) << "\"";
    }
    out << "]}";
    std::cout << out.str() << "\n";
  } else {
    std::cout << "scene: " << scenePath << "\n";
    std::cout << "valid: " << (ok ? "yes" : "no") << "\n";
    std::cout << "schema: " << result.value().metadata.schema << "\n";
    std::cout << "hash: " << result.value().export_hash_hex() << "\n";
    for (const auto& issue : issues) {
      std::cout << " - " << issue << "\n";
    }
    std::cout << "backend compatibility: " << (validBackend ? "ok" : "failed") << "\n";
  }
  return ok ? 0 : 2;
}

int MaterialCoverageCommand(const std::vector<std::string_view>& args) {
  bool json = false;
  std::string output = "artifacts/experiments/material_coverage.json";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--json") {
      json = true;
    } else if (args[i] == "--output") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --output value\n";
        return 1;
      }
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }

  const auto& descriptors = vkpt::materials::GetMaterialRegistry();
  vkpt::scene::SceneDocument doc;
  doc.metadata.schema = "1.0";
  doc.metadata.scene_name = "material_coverage";
  doc.benchmark.enabled = true;
  doc.benchmark.frame_target = 1;
  doc.benchmark.warmup_frames = 0;

  auto is_one_of = [](std::string_view id, std::initializer_list<std::string_view> values) {
    for (const auto value : values) {
      if (id == value) {
        return true;
      }
    }
    return false;
  };

  for (std::size_t i = 0; i < descriptors.size(); ++i) {
    const auto& descriptor = descriptors[i];
    const auto materialId = static_cast<vkpt::core::StableId>(1000u + i);

    vkpt::scene::SceneMaterialDefinition material;
    material.id = materialId;
    material.name = descriptor.display_name;
    material.family = descriptor.id;
    doc.materials.push_back(std::move(material));

    vkpt::scene::SceneGeometryDefinition geometry;
    geometry.id = static_cast<vkpt::core::StableId>(2000u + i);
    geometry.primitive = "triangle";
    geometry.material_id = materialId;
    const float x = static_cast<float>(i % 12u);
    const float y = static_cast<float>(i / 12u);
    geometry.vertices = {{x, y, 0.0f}, {x + 0.8f, y, 0.0f}, {x + 0.4f, y + 0.75f, 0.0f}};
    geometry.indices = {0u, 1u, 2u};
    doc.geometry.push_back(std::move(geometry));

    vkpt::scene::SceneEntityDefinition entity;
    entity.id = static_cast<vkpt::core::StableId>(3000u + i);
    entity.name = descriptor.id;
    entity.has_mesh = true;
    entity.mesh.mesh_id = static_cast<vkpt::core::StableId>(2000u + i);
    entity.mesh.material_id = materialId;
    doc.entities.push_back(std::move(entity));
  }

  vkpt::scene::SceneEntityDefinition camera;
  camera.id = 4000u;
  camera.name = "coverage_camera";
  camera.has_camera = true;
  camera.camera.fov = 45.0f;
  doc.entities.push_back(std::move(camera));

  vkpt::scene::SceneEntityDefinition light;
  light.id = 4001u;
  light.name = "coverage_light";
  light.has_light = true;
  light.light.type = "point";
  light.light.color = {1.0f, 1.0f, 1.0f};
  light.light.intensity = 4.0f;
  doc.entities.push_back(std::move(light));

  std::vector<std::string> issues;
  auto rtResult = vkpt::pathtracer::BuildSceneDataFromDocument(doc);
  if (!rtResult) {
    issues.push_back("rt_scene_build_failed");
  }

  struct Row {
    std::string id;
    std::string status;
    uint32_t model = 0;
    uint32_t effect = 0;
    float roughness = 0.0f;
    float metallic = 0.0f;
    float transmission = 0.0f;
    float clearcoat = 0.0f;
    float alpha = 1.0f;
    bool emissive = false;
    std::string issue;
  };
  std::vector<Row> rows;
  std::array<std::uint32_t, 16> modelCounts{};
  std::array<std::uint32_t, 32> effectCounts{};

  if (rtResult) {
    const auto& rtScene = rtResult.value();
    if (rtScene.materials.size() != descriptors.size()) {
      issues.push_back("material_count_mismatch:" + std::to_string(rtScene.materials.size()) +
                       "!=" + std::to_string(descriptors.size()));
    }
    const std::size_t count = std::min(rtScene.materials.size(), descriptors.size());
    for (std::size_t i = 0; i < count; ++i) {
      const auto& descriptor = descriptors[i];
      const auto& material = rtScene.materials[i];
      const std::string id = descriptor.id;
      Row row;
      row.id = id;
      row.status = vkpt::materials::ToString(descriptor.status);
      row.model = material.material_model;
      row.effect = material.material_effect;
      row.roughness = material.roughness;
      row.metallic = material.metallic;
      row.transmission = material.transmission;
      row.clearcoat = material.clearcoat;
      row.alpha = material.alpha;
      row.emissive = material.is_emissive();

      if (row.model < modelCounts.size()) {
        ++modelCounts[row.model];
      }
      if (row.effect < effectCounts.size()) {
        ++effectCounts[row.effect];
      }

      auto fail = [&](std::string message) {
        if (!row.issue.empty()) {
          row.issue += ";";
        }
        row.issue += std::move(message);
      };

      if (descriptor.status != vkpt::materials::ImplementationStatus::Implemented) {
        fail("descriptor_not_implemented");
      }
      if (!std::isfinite(row.roughness) || !std::isfinite(row.metallic) ||
          !std::isfinite(row.transmission) || !std::isfinite(row.clearcoat) ||
          !std::isfinite(row.alpha)) {
        fail("non_finite_material_value");
      }
      if (row.roughness < 0.0f || row.roughness > 1.0f ||
          row.metallic < 0.0f || row.metallic > 1.0f ||
          row.transmission < 0.0f || row.transmission > 1.0f ||
          row.clearcoat < 0.0f || row.clearcoat > 1.0f ||
          row.alpha < 0.0f || row.alpha > 1.0f) {
        fail("material_value_out_of_range");
      }

      const bool emissive = is_one_of(id, {"emissive", "environment_emissive", "blackbody_emission",
                                           "fire_plasma", "fire_sparkle_emission",
                                           "light_emitting_textile", "bokeh_motion_blur_stress"});
      const bool metal = is_one_of(id, {"ggx_rough_conductor", "metallic_pbr", "anisotropic_ggx",
                                        "brushed_metal", "ground_metal"});
      const bool glass = is_one_of(id, {"ggx_rough_dielectric", "dielectric_glass",
                                        "spectral_glass_approx", "frosted_glass", "dirty_glass",
                                        "water_fluid_surface", "ice_crystal", "resin", "epoxy",
                                        "gemstone", "frosted_acrylic", "translucent_polymer"});
      const bool coated = is_one_of(id, {"clearcoat", "paint", "car_paint", "porcelain_ceramic",
                                         "wet_surface", "energy_conserving_layered",
                                         "thin_film_iridescent", "diffraction_grating",
                                         "holographic_coating", "retroreflector",
                                         "caustics_inspired_response"});
      const bool sheen = is_one_of(id, {"velvet", "fabric_cloth", "hair_fur_lobes", "pearl_lustre"});
      const bool toon = is_one_of(id, {"toon_surface", "stylized_diffuse", "xray"});
      const bool volume = is_one_of(id, {"volumetric_medium", "volumetric_shafts", "smoke", "chromatic_dust"});

      if (id == "diffuse" && row.model != 0u) {
        fail("diffuse_model");
      }
      if (emissive && (row.model != 1u || !row.emissive)) {
        fail("emissive_runtime");
      }
      if (id == "mirror" && (row.model != 2u || row.roughness > 0.01f || row.metallic < 0.99f)) {
        fail("mirror_runtime");
      }
      if (is_one_of(id, {"specular", "glossy", "normal_mapped_pbr", "plastic", "rubber"}) &&
          row.model != 3u) {
        fail("specular_runtime");
      }
      if (metal && (row.model != 4u || row.metallic < 0.9f)) {
        fail("metal_runtime");
      }
      if (glass && (row.model != 5u || row.transmission <= 0.05f)) {
        fail("glass_runtime");
      }
      if (coated && (row.model != 7u || row.clearcoat <= 0.05f)) {
        fail("clearcoat_runtime");
      }
      if (sheen && (row.model != 6u || (row.effect == 0u && row.clearcoat <= 0.05f))) {
        fail("sheen_runtime");
      }
      if (toon && row.model != 8u) {
        fail("toon_runtime");
      }
      const bool validVolumeEffect = row.effect == 15u || (id == "chromatic_dust" && row.effect == 6u);
      if (volume && (row.model != 9u || !validVolumeEffect || row.alpha >= 0.99f)) {
        fail("volume_runtime");
      }
      if (id == "rubber" && (row.transmission > 0.05f || row.model == 5u)) {
        fail("rubber_not_glass");
      }
      if (id != "diffuse" && !emissive && row.model == 0u && row.effect == 0u) {
        fail("unclassified_diffuse_fallback");
      }

      if (!row.issue.empty()) {
        issues.push_back(id + ":" + row.issue);
      }
      rows.push_back(std::move(row));
    }
  }

  const bool ok = issues.empty();
  EnsureDirectory(Path(output).parent_path());
  std::ofstream out{Path(output)};
  if (out.is_open()) {
    out << "{\n";
    out << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
    out << "  \"material_count\": " << descriptors.size() << ",\n";
    out << "  \"model_counts\": [";
    for (std::size_t i = 0; i < modelCounts.size(); ++i) {
      if (i) out << ",";
      out << modelCounts[i];
    }
    out << "],\n";
    out << "  \"effect_counts\": [";
    for (std::size_t i = 0; i < effectCounts.size(); ++i) {
      if (i) out << ",";
      out << effectCounts[i];
    }
    out << "],\n";
    out << "  \"issues\": [";
    for (std::size_t i = 0; i < issues.size(); ++i) {
      if (i) out << ",";
      out << "\"" << EscapeJson(issues[i]) << "\"";
    }
    out << "],\n";
    out << "  \"rows\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& row = rows[i];
      out << "    {\"id\":\"" << EscapeJson(row.id) << "\","
          << "\"status\":\"" << EscapeJson(row.status) << "\","
          << "\"model\":" << row.model << ","
          << "\"effect\":" << row.effect << ","
          << "\"roughness\":" << row.roughness << ","
          << "\"metallic\":" << row.metallic << ","
          << "\"transmission\":" << row.transmission << ","
          << "\"clearcoat\":" << row.clearcoat << ","
          << "\"alpha\":" << row.alpha << ","
          << "\"emissive\":" << (row.emissive ? "true" : "false") << ","
          << "\"issue\":\"" << EscapeJson(row.issue) << "\"}";
      if (i + 1 < rows.size()) out << ",";
      out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
  }

  if (json) {
    std::cout << "{\"ok\":" << (ok ? "true" : "false")
              << ",\"material_count\":" << descriptors.size()
              << ",\"issue_count\":" << issues.size()
              << ",\"artifact\":\"" << EscapeJson(output) << "\"";
    if (!issues.empty()) {
      std::cout << ",\"issues\":[";
      for (std::size_t i = 0; i < issues.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << "\"" << EscapeJson(issues[i]) << "\"";
      }
      std::cout << "]";
    }
    std::cout << "}\n";
  } else {
    std::cout << "material coverage: " << (ok ? "ok" : "failed") << "\n";
    std::cout << "materials: " << descriptors.size() << "\n";
    std::cout << "artifact: " << output << "\n";
    for (const auto& issue : issues) {
      std::cout << " - " << issue << "\n";
    }
  }
  return ok ? 0 : 2;
}

int DumpCapabilitiesCommand() {
  const auto cpuFeatures = vkpt::cpu::QueryCpuFeatures();
  const auto bestSimd = vkpt::cpu::SelectBestSimdMode(cpuFeatures);

  std::cout << "{\n";
  std::cout << "  \"cpu\":" << vkpt::cpu::SerializeCpuFeatures(cpuFeatures) << ",\n";
  std::cout << "  \"simd_mode\":\"" << vkpt::cpu::SimdModeName(bestSimd) << "\",\n";
  std::cout << "  \"backends\": [\n";
  const auto names = vkpt::render::AvailableBackendNames();
  for (std::size_t i = 0; i < names.size(); ++i) {
    const auto name = names[i];
    const auto backend = vkpt::render::CreateBackend(name);
    std::cout << "    {\n";
    std::cout << "      \"name\":\"" << EscapeJson(name) << "\",\n";
    if (backend && backend->initialize().is_ok()) {
      std::cout << "      \"available\":true,\n";
      std::cout << "      \"capabilities\":" << vkpt::render::SerializeBackendCapabilities(backend->capabilities()) << "\n";
    } else {
      std::cout << "      \"available\":false,\n";
      std::cout << "      \"capabilities\":null\n";
    }
    std::cout << "    }";
    if (i + 1 < names.size()) {
      std::cout << ",";
    }
    std::cout << "\n";
  }
  std::cout << "  ],\n";
  std::cout << "  \"benchmark_capabilities\":"
            << vkpt::benchmark::SerializeBenchmarkCapabilities(vkpt::benchmark::DefaultBenchmarkCapabilities()) << ",\n";
  std::cout << "  \"profiler_capabilities\":"
            << vkpt::benchmark::SerializeProfilerCapabilities(vkpt::benchmark::DefaultProfilerCapabilities()) << "\n";
  std::cout << "}\n";
  return 0;
}

int CompareCommand(const std::vector<std::string_view>& args) {
  std::string referencePath;
  std::string imagePath;
  std::string outputPath;
  std::string tolerance = "abs=0.001";
  bool noHeatmap = false;

  for (std::size_t i = 2; i < args.size(); ++i) {
    const auto token = args[i];
    if (token == "--reference") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --reference\n";
        return 1;
      }
      referencePath = std::string(args[++i]);
    } else if (token == "--image") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --image\n";
        return 1;
      }
      imagePath = std::string(args[++i]);
    } else if (token == "--output") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --output\n";
        return 1;
      }
      outputPath = std::string(args[++i]);
    } else if (token == "--tolerance-policy") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --tolerance-policy\n";
        return 1;
      }
      tolerance = std::string(args[++i]);
    } else if (token == "--disable-heatmap") {
      noHeatmap = true;
    } else {
      std::cerr << "unknown option: " << token << "\n";
      return 1;
    }
  }
  if (referencePath.empty() || imagePath.empty() || outputPath.empty()) {
    std::cerr << "compare requires --reference --image --output\n";
    return 1;
  }
  TolerancePolicy policy;
  std::string parseError;
  if (!ParseTolerance(tolerance, policy, &parseError)) {
    std::cerr << "invalid tolerance: " << parseError << "\n";
    return 1;
  }
  ImageRgb reference;
  ImageRgb candidate;
  std::string err;
  if (!LoadImage(referencePath, reference, &err) || !LoadImage(imagePath, candidate, &err)) {
    std::cerr << "compare load failed: " << err << "\n";
    return 2;
  }
  const auto result = CompareImages(reference, candidate, policy);
  std::cout << std::fixed << std::setprecision(8);
  std::cout << "mean_abs_error: " << result.mean_abs_error << "\n";
  std::cout << "max_error: " << result.max_error << "\n";
  std::cout << "rmse: " << result.rmse << "\n";
  std::cout << "nan_inf_count: " << result.nan_inf_count << "\n";

  if (!noHeatmap) {
    Path out(outputPath);
    EnsureDirectory(out);
    Path heatmap = out / "diff_heatmap.png";
    // CompareImages stores per-channel difference in candidate-sized vector.
    // Rebuild heatmap by reusing stored diff vector from another compare.
    const auto diffResult = CompareImages(reference, candidate, policy);
    if (SaveDiffHeatmap(heatmap, reference.width, reference.height, std::vector<float>(diffResult.diff), nullptr)) {
      std::cout << "heatmap: " << heatmap.string() << "\n";
    }
  }
  return 0;
}


}  // namespace vkpt::benchmark::ptbench
