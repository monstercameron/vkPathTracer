#include "app/DoctorChecks.h"

#include "build_info.generated.h"
#include "core/Logging.h"
#include "jobs/JobSystem.h"
#include "pathtracer/PathTracer.h"
#include "platform/PlatformFactory.h"
#include "render/backends/BackendFactory.h"
#include "render/backends/D3D12Backend.h"
#include "render/interface/RenderContracts.h"
#include "scene/Scene.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* YesNo(bool value) {
  return value ? "yes" : "no";
}

std::string QtVersionString() {
#ifdef PT_ENABLE_QT
  return std::string(vkpt::build::kQtVersion);
#else
  return "disabled";
#endif
}

std::string QtPlatformShellString() {
#ifdef PT_ENABLE_QT
  return "available";
#else
  return "disabled";
#endif
}

bool IsRawPlatformBuilt() {
  return vkpt::platform::IsPlatformBuilt(vkpt::platform::RuntimePlatformKind::Raw);
}

bool IsQtPlatformBuilt() {
  return vkpt::platform::IsPlatformBuilt(vkpt::platform::RuntimePlatformKind::Qt);
}

}  // namespace

namespace vkpt::app {

struct DoctorCheckResult {
  std::string name;
  bool passed = false;
  std::string detail;
};

DoctorCheckResult CheckBuild() {
  DoctorCheckResult r;
  r.name = "build";
  bool ok = true;
  std::ostringstream detail;
  detail << "version=" << vkpt::build::kProjectVersion
         << " git=" << vkpt::build::kGitHash
         << " compiler=" << vkpt::build::kCompilerName
         << " target=" << vkpt::build::kTargetOs << "/" << vkpt::build::kTargetArch
         << " host=" << vkpt::platform::HostPlatformName(vkpt::platform::HostPlatform())
         << " features=[" << vkpt::build::kEnabledFeatureFlags << "]"
         << " ui_platforms=headless:" << YesNo(true)
         << ",raw:" << YesNo(IsRawPlatformBuilt())
         << ",qt:" << YesNo(IsQtPlatformBuilt())
         << " qt_version=" << QtVersionString()
         << " qt_platform_shell=" << QtPlatformShellString();
  for (const auto& platform : vkpt::platform::DescribeRuntimePlatforms()) {
    detail << " platform_" << platform.name
           << "={built:" << YesNo(platform.built)
           << ",available:" << YesNo(platform.available)
           << ",stub:" << YesNo(platform.stub)
           << ",impl:" << platform.implementation;
    if (platform.unavailable_reason != nullptr && platform.unavailable_reason[0] != '\0') {
      detail << ",reason:" << platform.unavailable_reason;
    }
    detail << "}";
  }

  // Gate 10 (F17): startup self-test (best-effort; actionable failures).
  const std::filesystem::path outDir = "artifacts/self_test";
  std::error_code ec;
  std::filesystem::create_directories(outDir, ec);
  if (ec) {
    ok = false;
    detail << " self_test_dir=FAIL(" << ec.message() << ")";
  } else {
    detail << " self_test_dir=ok";
    const auto probePath = outDir / "write_probe.txt";
    std::ofstream probe(probePath.string());
    probe << "ptapp self-test\n";
    probe.close();
    ec.clear();
    if (!std::filesystem::exists(probePath, ec) || ec) {
      ok = false;
      detail << " write_probe=FAIL";
    } else {
      detail << " write_probe=ok";
    }
  }

  // Backend selection / init smoke (non-fatal if none).
  const auto backends = vkpt::render::AvailableBackendNames();
  if (backends.empty()) {
    ok = false;
    detail << " backends=FAIL(none)";
  } else {
    auto backend = vkpt::render::CreateBackend(backends.front());
    if (!backend || !backend->initialize()) {
      ok = false;
      detail << " backend_init=FAIL(" << backends.front() << ")";
    } else {
      detail << " backend_init=ok(" << backends.front() << ")";
    }
  }

  r.passed = ok;
  r.detail = detail.str();
  return r;
}

DoctorCheckResult CheckCpu() {
  DoctorCheckResult r;
  r.name = "cpu";
  r.passed = true;
  std::ostringstream detail;
  detail << "simd_options=[" << vkpt::build::kSimdCompileOptions << "]";
#if defined(__AVX2__)
  detail << " avx2=yes";
#elif defined(__AVX__)
  detail << " avx=yes";
#else
  detail << " avx=no";
#endif
#if defined(__SSE4_2__)
  detail << " sse4.2=yes";
#endif
#if defined(__ARM_NEON)
  detail << " neon=yes";
#endif
  r.detail = detail.str();
  return r;
}

DoctorCheckResult CheckBackends() {
  DoctorCheckResult r;
  r.name = "backends";
  auto names = vkpt::render::AvailableBackendNames();
  if (names.empty()) {
    r.passed = false;
    r.detail = "no backends available";
    return r;
  }
  r.passed = true;
  std::ostringstream detail;
  for (const auto& name : names) {
    auto backend = vkpt::render::CreateBackend(name);
    if (!backend) { detail << name << ":unavailable "; continue; }
    if (!backend->initialize()) { detail << name << ":init_failed "; continue; }
    const auto caps = backend->capabilities();
    detail << name << ":ok(compute=" << (caps.compute ? "y" : "n")
           << ",rt=" << (caps.ray_tracing ? "y" : "n") << ") ";
  }
#ifdef PT_ENABLE_D3D12
  vkpt::render::RayBudgetRequest autoRequest;
  autoRequest.accelerator_preset = vkpt::render::AcceleratorSelectionPreset::Auto;
  const auto autoPlan = vkpt::render::BuildD3D12RayBudgetPlan(autoRequest);
  vkpt::render::RayBudgetRequest highPerformanceRequest = autoRequest;
  highPerformanceRequest.accelerator_preset = vkpt::render::AcceleratorSelectionPreset::HighPerformance;
  const auto highPerformancePlan = vkpt::render::BuildD3D12RayBudgetPlan(highPerformanceRequest);
  std::size_t autoActiveAssignments = 0u;
  for (const auto& assignment : autoPlan.assignments) {
    if (assignment.active) {
      ++autoActiveAssignments;
    }
  }
  std::size_t highPerformanceActiveAssignments = 0u;
  for (const auto& assignment : highPerformancePlan.assignments) {
    if (assignment.active) {
      ++highPerformanceActiveAssignments;
    }
  }
  detail << "accelerator_auto=ok(active=" << autoActiveAssignments
         << ",target_rays=" << autoPlan.total_target_rays << ") "
         << "accelerator_high_performance=ok(active=" << highPerformanceActiveAssignments
         << ",target_rays=" << highPerformancePlan.total_target_rays << ") ";
#endif
  r.detail = detail.str();
  return r;
}

DoctorCheckResult CheckAssets() {
  DoctorCheckResult r;
  r.name = "assets";
  const std::filesystem::path sceneDir = "assets/scenes";
  std::error_code ec;
  if (!std::filesystem::exists(sceneDir, ec) || ec) {
    r.passed = false;
    r.detail = "assets/scenes directory missing";
    return r;
  }
  std::size_t count = 0;
  for (std::filesystem::directory_iterator it(sceneDir, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       it != end;
       it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (it->path().extension() == ".json") ++count;
  }
  r.passed = count > 0;
  auto absoluteSceneDir = std::filesystem::absolute(sceneDir, ec);
  if (ec) {
    absoluteSceneDir = sceneDir;
    ec.clear();
  }
  r.detail = std::string("scene_files=") + std::to_string(count)
           + " path=" + absoluteSceneDir.string();
  return r;
}

DoctorCheckResult CheckShaders() {
  DoctorCheckResult r;
  r.name = "shaders";
  const std::filesystem::path shaderDir = "src/shaders";
  std::error_code ec;
  if (!std::filesystem::exists(shaderDir, ec) || ec) {
    r.passed = false;
    r.detail = "src/shaders directory missing";
    return r;
  }
  std::size_t count = 0;
  for (std::filesystem::recursive_directory_iterator it(
           shaderDir, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       it != end;
       it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    std::error_code entryEc;
    if (it->is_regular_file(entryEc) && !entryEc) ++count;
  }
  r.passed = true;
  r.detail = std::string("shader_files=") + std::to_string(count);
  return r;
}

DoctorCheckResult CheckJobSystem() {
  DoctorCheckResult r;
  r.name = "job_system";
  std::atomic<int> counter{0};
  {
    vkpt::jobs::JobSystem js(1u);
    auto handle = js.submit_job([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    js.wait(handle);
    js.shutdown();
  }
  r.passed = counter.load(std::memory_order_relaxed) == 1;
  r.detail = r.passed ? "job_ran=ok worker_count=1" : "job did not complete";
  return r;
}

DoctorCheckResult CheckSceneSchema() {
  DoctorCheckResult r;
  r.name = "scene_schema";
  const std::filesystem::path sceneDir = "assets/scenes";
  std::error_code ec;
  if (!std::filesystem::exists(sceneDir, ec) || ec) {
    r.passed = true;
    r.detail = "skipped(assets/scenes not found)";
    return r;
  }

  std::vector<std::filesystem::path> scenePaths;
  for (std::filesystem::directory_iterator it(sceneDir, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       it != end;
       it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    std::error_code entryEc;
    if (it->is_regular_file(entryEc) && !entryEc && it->path().extension() == ".json") {
      scenePaths.push_back(it->path());
    }
  }
  std::sort(scenePaths.begin(), scenePaths.end());
  if (scenePaths.empty()) {
    r.passed = true;
    r.detail = "skipped(no scene json files)";
    return r;
  }

  std::vector<std::string> issues;
  std::size_t totalEntities = 0;
  std::size_t totalMaterials = 0;
  std::size_t totalInstances = 0;
  std::size_t totalLights = 0;
  std::size_t totalSdfPrimitives = 0;

  auto checkRuntimeMaterialPresets = [&]() {
    // This synthetic scene catches material-family default drift even when no
    // checked-in sample happens to exercise a preset.
    vkpt::scene::SceneDocument presetDoc;
    presetDoc.metadata.schema = "1.0";
    presetDoc.metadata.scene_name = "material_preset_smoke";
    auto addMaterial = [&](vkpt::core::StableId id, std::string family) {
      vkpt::scene::SceneMaterialDefinition material;
      material.id = id;
      material.name = family;
      material.family = std::move(family);
      presetDoc.materials.push_back(std::move(material));
    };
    addMaterial(1u, "mirror");
    addMaterial(2u, "dielectric_glass");
    addMaterial(3u, "clearcoat");
    addMaterial(4u, "emissive");
    for (std::uint32_t i = 0; i < 4u; ++i) {
      const float x = static_cast<float>(i);
      vkpt::scene::SceneGeometryDefinition geometry;
      geometry.id = 100u + i;
      geometry.primitive = "triangle";
      geometry.material_id = i + 1u;
      geometry.vertices = {
          {x, 0.0f, 0.0f},
          {x + 0.5f, 0.0f, 0.0f},
          {x, 0.5f, 0.0f},
      };
      geometry.indices = {0u, 1u, 2u};
      presetDoc.geometry.push_back(std::move(geometry));

      vkpt::scene::SceneEntityDefinition entity;
      entity.id = 200u + i;
      entity.name = "preset_triangle_" + std::to_string(i);
      entity.has_mesh = true;
      entity.mesh.mesh_id = 100u + i;
      entity.mesh.material_id = i + 1u;
      presetDoc.entities.push_back(std::move(entity));
    }
    auto rtResult = vkpt::pathtracer::BuildSceneDataFromDocument(presetDoc);
    if (!rtResult || rtResult.value().materials.size() < 4u) {
      issues.push_back("material_preset_smoke:rt_scene_failed");
      return;
    }
    const auto& mats = rtResult.value().materials;
    if (mats[0].material_model != 2u || mats[0].roughness > 0.001f || mats[0].metallic < 0.99f) {
      issues.push_back("material_preset_smoke:mirror_defaults");
    }
    if (mats[1].material_model != 5u || mats[1].transmission < 0.99f || mats[1].alpha > 0.5f) {
      issues.push_back("material_preset_smoke:glass_defaults");
    }
    if (mats[2].material_model != 7u || mats[2].clearcoat < 0.99f) {
      issues.push_back("material_preset_smoke:clearcoat_defaults");
    }
    if (mats[3].material_model != 1u || !mats[3].is_emissive()) {
      issues.push_back("material_preset_smoke:emissive_defaults");
    }
  };
  checkRuntimeMaterialPresets();

  for (const auto& scenePath : scenePaths) {
    const auto sceneName = scenePath.filename().string();
    auto result = vkpt::scene::SceneDocument::load_from_file(scenePath.string());
    if (!result) {
      issues.push_back(sceneName + ":load=" + std::to_string(static_cast<int>(result.error())));
      continue;
    }

    const auto& document = result.value();
    std::vector<std::string> sceneIssues;
    if (!document.validate(&sceneIssues)) {
      for (const auto& issue : sceneIssues) {
        issues.push_back(sceneName + ":" + issue);
      }
    }

    auto worldResult = document.to_world();
    if (!worldResult) {
      issues.push_back(sceneName + ":to_world=" + std::to_string(static_cast<int>(worldResult.error())));
    }

    auto rtSceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(document);
    if (!rtSceneResult) {
      issues.push_back(sceneName + ":rt_scene=" + std::to_string(static_cast<int>(rtSceneResult.error())));
    } else {
      const auto& rtScene = rtSceneResult.value();
      totalInstances += rtScene.instances.size();
      totalLights += rtScene.lights.size();
      totalSdfPrimitives += rtScene.sdf_primitives.size();
    }

    totalEntities += document.entities.size();
    totalMaterials += document.materials.size();
  }

  r.passed = issues.empty();
  std::ostringstream detail;
  detail << "scenes=" << scenePaths.size()
         << " entities=" << totalEntities
         << " materials=" << totalMaterials
         << " rt_instances=" << totalInstances
         << " rt_lights=" << totalLights
         << " rt_sdf=" << totalSdfPrimitives
         << " valid=" << (r.passed ? "yes" : "no");
  if (!issues.empty()) {
    detail << " issues=[";
    for (std::size_t i = 0; i < issues.size(); ++i) {
      if (i) detail << ",";
      detail << issues[i];
    }
    detail << "]";
  }
  r.detail = detail.str();
  return r;
}

DoctorCheckResult CheckBenchmarkArtifactWrite() {
  DoctorCheckResult r;
  r.name = "benchmark_artifact_write";
  const std::filesystem::path outDir = "artifacts/self_test";
  std::error_code ec;
  std::filesystem::create_directories(outDir, ec);
  if (ec) {
    r.passed = false;
    r.detail = std::string("dir=FAIL(") + ec.message() + ")";
    return r;
  }
  const auto probePath = outDir / "bench_probe.json";
  {
    std::ofstream probe(probePath.string());
    probe << "{\"self_test\":true,\"schema\":\"benchmark_result\"}\n";
  }
  ec.clear();
  r.passed = std::filesystem::exists(probePath, ec) && !ec;
  r.detail = r.passed ? "bench_probe.json=ok" : "bench_probe.json=FAIL";
  return r;
}

void RunDoctor(bool checkBuild, bool checkCpu, bool checkBackends,
               bool checkAssets, bool checkShaders,
               bool checkJobSystem, bool checkSceneSchema, bool checkBenchmarkArtifact) {
  const std::vector<DoctorCheckResult> results = {
    checkBuild             ? CheckBuild()                  : DoctorCheckResult{"build",                    true, "skipped"},
    checkCpu               ? CheckCpu()                    : DoctorCheckResult{"cpu",                      true, "skipped"},
    checkBackends          ? CheckBackends()               : DoctorCheckResult{"backends",                 true, "skipped"},
    checkAssets            ? CheckAssets()                 : DoctorCheckResult{"assets",                   true, "skipped"},
    checkShaders           ? CheckShaders()                : DoctorCheckResult{"shaders",                  true, "skipped"},
    checkJobSystem         ? CheckJobSystem()              : DoctorCheckResult{"job_system",               true, "skipped"},
    checkSceneSchema       ? CheckSceneSchema()            : DoctorCheckResult{"scene_schema",             true, "skipped"},
    checkBenchmarkArtifact ? CheckBenchmarkArtifactWrite() : DoctorCheckResult{"benchmark_artifact_write", true, "skipped"},
  };

  bool allOk = true;
  for (const auto& r : results) {
    const char* status = r.passed ? "ok " : "FAIL";
    std::cout << "[" << status << "] " << r.name << ": " << r.detail << "\n";
    if (!r.passed) allOk = false;
  }
  std::cout << "\ndoctor: " << (allOk ? "ok" : "FAIL") << "\n";
}

}  // namespace vkpt::app
