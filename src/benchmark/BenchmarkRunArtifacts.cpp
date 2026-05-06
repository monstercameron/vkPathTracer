#include "benchmark/BenchmarkRuntimeInternal.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "pathtracer/PathTracer.h"
#include "scene/Scene.h"

namespace vkpt::benchmark::ptbench {

using Path = std::filesystem::path;

std::string SerializeSceneSnapshot(const vkpt::scene::SceneSnapshot& snapshot) {
  std::ostringstream out;
  out << "{";
  out << "\"asset_refs\":[";
  for (std::size_t i = 0; i < snapshot.asset_refs.size(); ++i) {
    if (i) out << ",";
    out << "\"" << EscapeJson(snapshot.asset_refs[i]) << "\"";
  }
  out << "],\"entity_ids\":[";
  for (std::size_t i = 0; i < snapshot.entity_ids.size(); ++i) {
    if (i) out << ",";
    out << snapshot.entity_ids[i];
  }
  out << "],\"renderables\":[";
  for (std::size_t i = 0; i < snapshot.renderables.size(); ++i) {
    const auto& renderable = snapshot.renderables[i];
    if (i) out << ",";
    out << "{";
    out << "\"entity_id\":" << renderable.entity_id << ",";
    out << "\"mesh_id\":" << renderable.mesh_id << ",";
    out << "\"material_id\":" << renderable.material_id << ",";
    out << "\"transform\":[";
    out << renderable.transform.translation.x << ",";
    out << renderable.transform.translation.y << ",";
    out << renderable.transform.translation.z << ",";
    out << renderable.transform.rotation.x << ",";
    out << renderable.transform.rotation.y << ",";
    out << renderable.transform.rotation.z << ",";
    out << renderable.transform.rotation.w << ",";
    out << renderable.transform.scale.x << ",";
    out << renderable.transform.scale.y << ",";
    out << renderable.transform.scale.z << "]";
    out << "}";
  }
  out << "],\"lights\":[";
  for (std::size_t i = 0; i < snapshot.lights.size(); ++i) {
    const auto& light = snapshot.lights[i];
    if (i) out << ",";
    out << "{";
    out << "\"entity_id\":" << light.entity_id << ",";
    out << "\"type\":\"" << EscapeJson(light.light.type) << "\",";
    out << "\"color\":[" << light.light.color.x << "," << light.light.color.y << "," << light.light.color.z << "],";
    out << "\"intensity\":" << light.light.intensity << ",";
    out << "\"radius\":" << light.light.radius << "}";
  }
  out << "],\"materials\":[";
  for (std::size_t i = 0; i < snapshot.materials.size(); ++i) {
    const auto& mat = snapshot.materials[i];
    if (i) out << ",";
    out << "{";
    out << "\"id\":" << mat.id << ",";
    out << "\"name\":\"" << EscapeJson(mat.material.name) << "\",";
    out << "\"albedo\":[" << mat.material.albedo.x << "," << mat.material.albedo.y << "," << mat.material.albedo.z << "],";
    out << "\"roughness\":" << mat.material.roughness << ",";
    out << "\"emission\":[" << mat.material.emission.x << "," << mat.material.emission.y << "," << mat.material.emission.z << "],";
    out << "\"emission_intensity\":" << mat.material.emission_intensity << "}";
  }
  out << "],\"benchmark_enabled\":" << (snapshot.benchmark.enabled ? "true" : "false") << ",";
  out << "\"frame_target\":" << snapshot.benchmark.frame_target << ",";
  out << "\"warmup_frames\":" << snapshot.benchmark.warmup_frames << ",";
  out << "\"scene_hash\":\"" << Hex64(0) << "\"";
  out << "}";
  return out.str();
}

std::string SerializeMetadata(const vkpt::benchmark::BenchmarkResult& result,
                             const vkpt::scene::SceneDocument& scene,
                             const std::string& rendererPath) {
  std::ostringstream out;
  out << "{";
  out << "\"run_id\":\"" << EscapeJson(result.run_id) << "\",";
  out << "\"command\":\"ptbench run\",";
  out << "\"scene_path\":\"" << EscapeJson(result.scene) << "\",";
  out << "\"backend\":\"" << EscapeJson(result.backend) << "\",";
  out << "\"renderer_path\":\"" << EscapeJson(rendererPath) << "\",";
  out << "\"scene_name\":\"" << EscapeJson(scene.metadata.scene_name) << "\",";
  out << "\"schema\":\"" << EscapeJson(scene.metadata.schema) << "\",";
  out << "\"asset_count\":" << scene.assets.size() << ",";
  out << "\"material_count\":" << scene.materials.size() << ",";
  out << "\"geometry_count\":" << scene.geometry.size() << ",";
  out << "\"sdf_count\":" << scene.sdf_primitives.size() << ",";
  out << "\"entity_count\":" << scene.entities.size() << ",";
  out << "\"benchmark_enabled\":" << (scene.benchmark.enabled ? "true" : "false") << ",";
  out << "\"benchmark_capabilities\":" << vkpt::benchmark::SerializeBenchmarkCapabilities(vkpt::benchmark::DefaultBenchmarkCapabilities()) << ",";
  out << "\"profiler_capabilities\":" << vkpt::benchmark::SerializeProfilerCapabilities(vkpt::benchmark::DefaultProfilerCapabilities());
  out << "}";
  return out.str();
}

bool WriteRunArtifacts(const vkpt::benchmark::BenchmarkResult& result,
                      const Path& artifactDir,
                      const vkpt::scene::SceneDocument& scene,
                      const vkpt::pathtracer::RTSceneLayoutManifest& manifest,
                      const Path& referencePath,
                      const Path& diffHeatmapPath,
                      const bool includeReference,
                      std::string* error) {
  (void)diffHeatmapPath;
  const Path resultsPath = artifactDir / "results.json";
  const Path resultsCsvPath = artifactDir / "results.csv";
  const Path metadataPath = artifactDir / "metadata.json";
  const Path snapshotPath = artifactDir / "scene_snapshot.json";
  const Path shaderManifestPath = artifactDir / "shader_manifest.json";
  const Path assetManifestPath = artifactDir / "asset_manifest.json";
  const Path logsPath = artifactDir / "logs.jsonl";
  const Path profilerTracePath = artifactDir / "profiler_trace.json";
  const Path required[] = {resultsPath, resultsCsvPath, metadataPath, snapshotPath, shaderManifestPath,
                           assetManifestPath, result.beauty_png, result.beauty_exr, logsPath, profilerTracePath};
  if (!WriteFile(resultsPath, vkpt::benchmark::SerializeBenchmarkResult(result), error)) {
    return false;
  }
  std::ofstream csv(resultsCsvPath);
  if (!csv.is_open()) {
    if (error) {
      *error = "failed to write results.csv";
    }
    return false;
  }
  csv << "run_id,scene,backend,renderer_path,resolution_width,resolution_height,spp,max_depth,total_ms,build_ms,render_ms,"
         "cpu_ms,paths_per_sec,samples_per_sec,samples_per_sec_per_thread,paths_per_sec_per_thread,"
         "samples_per_sec_per_megapixel,normalized_score,reference_error,image_hash\n";
  csv << EscapeJson(result.run_id) << "," << EscapeJson(result.scene) << ","
      << EscapeJson(result.backend) << "," << EscapeJson(result.renderer_path) << ","
      << result.resolution.width << "," << result.resolution.height << "," << result.spp << "," << result.max_depth << ","
      << std::fixed << std::setprecision(6) << result.timing.total_ms << "," << result.timing.build_ms << ","
      << result.timing.render_ms << "," << result.timing.cpu_ms << "," << result.throughput.paths_per_sec << ","
      << result.throughput.samples_per_sec << "," << result.score.samples_per_sec_per_thread << ","
      << result.score.paths_per_sec_per_thread << "," << result.score.samples_per_sec_per_megapixel << ","
      << result.score.normalized_score << "," << result.reference_error << ","
      << EscapeJson(result.image_hash) << "\n";
  csv.close();

  if (!WriteFile(metadataPath, SerializeMetadata(result, scene, result.renderer_path), error)) {
    return false;
  }
  if (!WriteFile(snapshotPath, SerializeSceneSnapshot(scene.snapshot()), error)) {
    return false;
  }
  if (!WriteFile(shaderManifestPath, vkpt::pathtracer::SerializeRTSceneDataLayoutManifest(manifest), error)) {
    return false;
  }
  std::vector<std::string> assetRefs;
  assetRefs.reserve(scene.assets.size());
  for (const auto& asset : scene.assets) {
    assetRefs.push_back(asset.uri);
  }
  if (!WriteFile(assetManifestPath, SerializeManifest(assetRefs), error)) {
    return false;
  }

  if (includeReference) {
    std::error_code ec;
    std::filesystem::copy_file(referencePath, artifactDir / "reference.exr", std::filesystem::copy_options::overwrite_existing, ec);
  }

  for (const auto& requiredPath : required) {
    std::error_code ec;
    if (!std::filesystem::exists(requiredPath, ec) || !std::filesystem::is_regular_file(requiredPath, ec)) {
      if (error) {
        *error = "required artifact not created: " + requiredPath.string();
      }
      return false;
    }
    if (std::filesystem::file_size(requiredPath, ec) == 0u && !ec) {
      if (error) {
        *error = "required artifact is empty: " + requiredPath.string();
      }
      return false;
    }
  }

  return true;
}


}  // namespace vkpt::benchmark::ptbench
