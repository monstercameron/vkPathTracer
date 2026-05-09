#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "benchmark/BenchmarkRuntime.h"

#include "benchmark/BenchmarkSchema.h"
#include "render/backends/BackendFactory.h"
#include "render/interface/RenderContracts.h"

namespace vkpt::benchmark::ptbench {

using Path = std::filesystem::path;

bool EnsureDirectory(const Path& path);
std::string EscapeJson(std::string_view text);

bool ParseSize(std::string_view text, std::size_t& out) {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return false;
  }
  std::size_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    return false;
  }
  out = value;
  return true;
}

bool BackendRegistered(std::string_view name) {
  const auto normalized = vkpt::render::NormalizeBackendName(name);
  const auto names = vkpt::render::AvailableBackendNames();
  return std::find(names.begin(), names.end(), normalized) != names.end();
}

int BackendExperimentsCommand(const std::vector<std::string_view>& args) {
  std::string output = "artifacts/experiments";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--output") {
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

  struct Target {
    std::string experiment;
    std::string backend;
    std::string renderer_path;
    bool requires_rt = false;
    bool requires_render_path = true;
  };
  const std::vector<Target> targets = {
      {"vulkan_compute_vs_rt", "vulkan", "gpu-compute", false, true},
      {"vulkan_compute_vs_rt", "vulkan", "gpu-rt", true, true},
      {"d3d12_compute_vs_dxr", "d3d12", "d3d12-compute", false, true},
      {"d3d12_compute_vs_dxr", "d3d12-dxr", "dxr", true, true},
      {"metal_compute_vs_rt", "metal", "metal-compute", false, true},
      {"metal_compute_vs_rt", "metal-rt", "metal-rt", true, true},
      {"webgpu_workgroup_sweep", "webgpu", "webgpu-compute", false, true},
  };

  struct Row {
    std::string experiment;
    std::string backend;
    std::string renderer_path;
    std::string status;
    std::string reason;
    std::string capability_summary;
  };
  std::vector<Row> rows;
  rows.reserve(targets.size());

  for (const auto& target : targets) {
    Row row;
    row.experiment = target.experiment;
    row.backend = target.backend;
    row.renderer_path = target.renderer_path;
    if (!BackendRegistered(target.backend)) {
      row.status = "skipped";
      row.reason = "backend is not registered in this build";
      rows.push_back(std::move(row));
      continue;
    }
    auto backend = vkpt::render::CreateBackend(target.backend);
    if (!backend || !backend->initialize().is_ok()) {
      row.status = "skipped";
      row.reason = backend ? backend->last_error() : "backend creation failed";
      rows.push_back(std::move(row));
      continue;
    }
    const auto caps = backend->capabilities();
    row.capability_summary = vkpt::render::SerializeBackendCapabilities(caps);
    if (target.requires_rt && !caps.ray_tracing) {
      row.status = "skipped";
      row.reason = "ray tracing capability is not exposed by this backend";
    } else if (target.renderer_path != "gpu-compute") {
      row.status = "skipped";
      row.reason = "backend adapter exists, but ptbench render path is not wired yet";
    } else {
      row.status = "available";
      row.reason = caps.is_simulated ? "simulated compute backend available" : "compute backend available";
    }
    rows.push_back(std::move(row));
  }

  EnsureDirectory(Path(output));
  const Path outPath = Path(output) / "backend_experiments.json";
  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n  \"rows\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& row = rows[i];
      out << "    {\"experiment\":\"" << EscapeJson(row.experiment) << "\","
          << "\"backend\":\"" << EscapeJson(row.backend) << "\","
          << "\"renderer_path\":\"" << EscapeJson(row.renderer_path) << "\","
          << "\"status\":\"" << EscapeJson(row.status) << "\","
          << "\"reason\":\"" << EscapeJson(row.reason) << "\","
          << "\"capabilities\":" << (row.capability_summary.empty() ? "null" : row.capability_summary) << "}";
      if (i + 1 < rows.size()) out << ",";
      out << "\n";
    }
    out << "  ]\n}\n";
  }
  std::cout << "results: " << outPath.string() << "\n";
  return 0;
}

int GpuMemPressureCommand(const std::vector<std::string_view>& args) {
  std::size_t max_mb = 512;
  std::size_t step_mb = 64;
  std::string backendName = "auto";
  std::string output = "artifacts/experiments";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--max-mb") {
      if (i + 1 >= args.size()) { std::cerr << "missing --max-mb value\n"; return 1; }
      if (!ParseSize(args[++i], max_mb)) { std::cerr << "invalid --max-mb value\n"; return 1; }
    } else if (args[i] == "--step-mb") {
      if (i + 1 >= args.size()) { std::cerr << "missing --step-mb value\n"; return 1; }
      if (!ParseSize(args[++i], step_mb)) { std::cerr << "invalid --step-mb value\n"; return 1; }
    } else if (args[i] == "--backend") {
      if (i + 1 >= args.size()) { std::cerr << "missing --backend value\n"; return 1; }
      backendName = std::string(args[++i]);
    } else if (args[i] == "--output") {
      if (i + 1 >= args.size()) { std::cerr << "missing --output value\n"; return 1; }
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  if (step_mb == 0) step_mb = 1;
  if (max_mb < step_mb) max_mb = step_mb;

  EnsureDirectory(Path(output));
  const Path outPath = Path(output) / "gpu_mem_pressure.json";

  std::vector<std::vector<std::uint8_t>> allocations;
  allocations.reserve(max_mb / step_mb + 1);
  std::size_t allocated_mb = 0;
  bool ok = true;
  std::string fail_reason;

  for (std::size_t target_mb = step_mb; target_mb <= max_mb; target_mb += step_mb) {
    try {
      allocations.emplace_back(step_mb * 1024ull * 1024ull, 0u);
      allocated_mb = target_mb;
      std::cout << "allocated_mb=" << allocated_mb << "\n";
    } catch (const std::bad_alloc&) {
      ok = false;
      fail_reason = "std::bad_alloc";
      break;
    } catch (const std::exception& ex) {
      ok = false;
      fail_reason = ex.what();
      break;
    }
  }

  std::string backendCaps = "null";
  std::string backendStatus = "skipped";
  std::string backendReason = "backend unavailable";
  if (auto backend = vkpt::render::CreateBackend(backendName)) {
    if (backend->initialize().is_ok()) {
      backendCaps = vkpt::render::SerializeBackendCapabilities(backend->capabilities());
      backendStatus = "available";
      backendReason = backend->capabilities().is_simulated ? "simulated backend; host allocation pressure used" :
                                                            "backend available; API memory budget not exposed";
    } else {
      backendReason = backend->last_error();
    }
  }

  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n";
    out << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
    out << "  \"backend\": \"" << EscapeJson(backendName) << "\",\n";
    out << "  \"backend_status\": \"" << EscapeJson(backendStatus) << "\",\n";
    out << "  \"backend_reason\": \"" << EscapeJson(backendReason) << "\",\n";
    out << "  \"backend_capabilities\": " << backendCaps << ",\n";
    out << "  \"allocated_mb\": " << allocated_mb << ",\n";
    out << "  \"max_mb\": " << max_mb << ",\n";
    out << "  \"step_mb\": " << step_mb << ",\n";
    out << "  \"fail_reason\": \"" << EscapeJson(fail_reason) << "\",\n";
    out << "  \"stress_rows\": [\n";
    const std::vector<std::string> stress = {"texture_size_stress", "bvh_size_stress", "readback_pressure", "upload_pressure"};
    for (std::size_t i = 0; i < stress.size(); ++i) {
      out << "    {\"name\":\"" << stress[i] << "\",\"status\":\""
          << (ok ? "ok" : "failed") << "\",\"mode\":\"simulated-host-allocation\","
          << "\"allocated_mb\":" << allocated_mb << ",\"fallback_decision\":\""
          << (ok ? "continue" : "fail with diagnostics") << "\"}";
      if (i + 1 < stress.size()) out << ",";
      out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
  }
  std::cout << "results: " << outPath.string() << "\n";
  return ok ? 0 : 2;
}

int ShaderMatrixCommand(const std::vector<std::string_view>& args) {
  std::string output = "artifacts/experiments";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--output") {
      if (i + 1 >= args.size()) { std::cerr << "missing --output value\n"; return 1; }
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  EnsureDirectory(Path(output));
  const Path outPath = Path(output) / "shader_matrix.json";

  struct MatrixTarget {
    std::string backend;
    std::string shader_family;
    std::string variant;
    std::vector<std::string> defines;
    std::string required_feature;
    bool cpu_validation = false;
  };
  const std::vector<MatrixTarget> targets = {
      {"vulkan", "pathtrace", "compute_minimum", {}, "compute", false},
      {"vulkan", "pathtrace", "vulkan_rt_optional", {"RT=1"}, "ray-tracing", false},
      {"d3d12", "pathtrace", "d3d12_compute", {}, "compute", false},
      {"d3d12-dxr", "pathtrace", "d3d12_dxr_optional", {"DXR=1"}, "ray-tracing", false},
      {"metal", "pathtrace", "metal_compute", {}, "compute", false},
      {"metal-rt", "pathtrace", "metal_rt_optional", {"RT=1"}, "ray-tracing", false},
      {"webgpu", "pathtrace", "webgpu_wgsl", {"WGSL=1"}, "compute", false},
      {"cpu", "materials", "cpu_material_validation", {}, "", true},
  };

  struct Row {
    std::string backend;
    std::string shader_family;
    std::string variant;
    std::string status;
    std::string diagnostics;
    std::string artifact;
  };
  std::vector<Row> rows;
  bool failed = false;

  for (const auto& target : targets) {
    if (target.cpu_validation) {
      const std::string coverageArtifact = (Path(output) / "material_coverage.json").string();
      std::vector<std::string> coverageArgs = {"ptbench", "material-coverage", "--output", coverageArtifact};
      std::vector<std::string_view> coverageViews;
      for (const auto& item : coverageArgs) {
        coverageViews.emplace_back(item);
      }
      const int coverageRc = MaterialCoverageCommand(coverageViews);
      if (coverageRc != 0) {
        failed = true;
      }
      rows.push_back({target.backend,
                      target.shader_family,
                      target.variant,
                      coverageRc == 0 ? "ok" : "failed",
                      coverageRc == 0 ? "all material families have runtime model/effect coverage"
                                      : "material coverage command failed",
                      coverageArtifact});
      continue;
    }
    if (!BackendRegistered(target.backend)) {
      rows.push_back({target.backend, target.shader_family, target.variant, "skipped", "backend is not registered in this build", ""});
      continue;
    }
    auto backend = vkpt::render::CreateBackend(target.backend);
    if (!backend || !backend->initialize().is_ok()) {
      rows.push_back({target.backend, target.shader_family, target.variant, "skipped",
                      backend ? backend->last_error() : "create_backend_failed", ""});
      continue;
    }
    auto* compiler = backend->compiler();
    if (!compiler) {
      rows.push_back({target.backend, target.shader_family, target.variant, "skipped", "no compiler", ""});
      continue;
    }
    if (!target.required_feature.empty() && !compiler->supports_feature(target.required_feature)) {
      rows.push_back({target.backend, target.shader_family, target.variant, "skipped",
                      "compiler does not support feature: " + target.required_feature, ""});
      continue;
    }

    vkpt::render::ComputePipelineDesc desc;
    desc.source_path = "shaders/ptbench_matrix.comp";
    desc.entry_point = "main";
    desc.debug_label = "ptbench_matrix";
    desc.defines = target.defines;

    std::string artifact;
    std::string diag;
    const auto status = compiler->compile_compute_shader(desc, artifact, &diag);
    const bool ok = status.is_ok();
    if (!ok) {
      failed = true;
    }
    rows.push_back({target.backend, target.shader_family, target.variant, ok ? "ok" : "failed", diag, artifact});
  }

  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n  \"rows\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& r = rows[i];
      out << "    {\"backend\":\"" << EscapeJson(r.backend) << "\","
          << "\"shader_family\":\"" << EscapeJson(r.shader_family) << "\","
          << "\"variant\":\"" << EscapeJson(r.variant) << "\","
          << "\"status\":\"" << EscapeJson(r.status) << "\","
          << "\"artifact\":\"" << EscapeJson(r.artifact) << "\","
          << "\"diagnostics\":\"" << EscapeJson(r.diagnostics) << "\"}";
      if (i + 1 < rows.size()) out << ",";
      out << "\n";
    }
    out << "  ]\n}\n";
  }
  std::cout << "results: " << outPath.string() << "\n";
  return failed ? 2 : 0;
}

int ReleaseCheckCommand(const std::vector<std::string_view>& args) {
  std::string scenePack = "assets/scenes";
  std::string output = "artifacts/release_check";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--scene-pack") {
      if (i + 1 >= args.size()) { std::cerr << "missing --scene-pack value\n"; return 1; }
      scenePack = std::string(args[++i]);
    } else if (args[i] == "--output") {
      if (i + 1 >= args.size()) { std::cerr << "missing --output value\n"; return 1; }
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  EnsureDirectory(Path(output));

  std::size_t scene_ok = 0;
  std::size_t scene_fail = 0;
  std::error_code scenePackEc;
  if (!std::filesystem::exists(Path(scenePack), scenePackEc) || scenePackEc) {
    std::cerr << "missing scene pack path: " << scenePack << "\n";
    return 2;
  }
  for (std::filesystem::directory_iterator it(
           Path(scenePack), std::filesystem::directory_options::skip_permission_denied, scenePackEc),
       end;
       it != end;
       it.increment(scenePackEc)) {
    if (scenePackEc) {
      scenePackEc.clear();
      continue;
    }
    const auto& entry = *it;
    std::error_code entryEc;
    if (!entry.is_regular_file(entryEc) || entryEc || entry.path().extension() != ".json") continue;
    const auto scenePath = entry.path().string();
    std::vector<std::string_view> v = {"ptbench", "validate-scene", "--scene", scenePath};
    const int rc = ValidateSceneCommand(v);
    if (rc == 0) ++scene_ok; else ++scene_fail;
  }

  const Path runOut = Path(output) / "cpu_scalar_cornell";
  const std::string runOutStr = runOut.string();
  std::vector<std::string_view> runArgs = {
    "ptbench", "run",
    "--scene", "assets/scenes/cornell_native.json",
    "--backend", "cpu",
    "--renderer-path", "cpu-scalar",
    "--resolution", "128x128",
    "--spp", "2",
    "--output", runOutStr
  };
  const int runRc = RunCommand(runArgs);
  const auto artifactValidation = vkpt::benchmark::ValidateBenchmarkArtifactsOnDisk(runOut.string());

  const Path threadedOut = Path(output) / "thread_sweep";
  const std::string threadedOutStr = threadedOut.string();
  std::vector<std::string_view> threadArgs = {
    "ptbench", "thread-sweep",
    "--scene", "assets/scenes/cornell_native.json",
    "--workers", "1,2",
    "--resolution", "64x64",
    "--spp", "1",
    "--output", threadedOutStr
  };
  const int threadRc = ThreadSweepCommand(threadArgs);

  const Path backendOut = Path(output) / "backend_experiments";
  const std::string backendOutStr = backendOut.string();
  std::vector<std::string_view> backendArgs = {"ptbench", "backend-experiments", "--output", backendOutStr};
  const int backendRc = BackendExperimentsCommand(backendArgs);

  const Path outPath = Path(output) / "release_check.json";
  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n";
    out << "  \"scene_validate_ok\": " << scene_ok << ",\n";
    out << "  \"scene_validate_fail\": " << scene_fail << ",\n";
    out << "  \"cpu_scalar_run_rc\": " << runRc << ",\n";
    out << "  \"artifact_contract_ok\": " << (artifactValidation.ok ? "true" : "false") << ",\n";
    out << "  \"thread_sweep_rc\": " << threadRc << ",\n";
    out << "  \"backend_experiments_rc\": " << backendRc << ",\n";
    out << "  \"checklist\": [\n";
    out << "    {\"name\":\"scene pack validates\",\"status\":\"" << (scene_fail == 0 ? "pass" : "fail") << "\"},\n";
    out << "    {\"name\":\"CPU scalar render passes\",\"status\":\"" << (runRc == 0 ? "pass" : "fail") << "\"},\n";
    out << "    {\"name\":\"benchmark artifacts validate\",\"status\":\"" << (artifactValidation.ok ? "pass" : "fail") << "\"},\n";
    out << "    {\"name\":\"CPU threaded render passes\",\"status\":\"" << (threadRc == 0 ? "pass" : "fail") << "\"},\n";
    out << "    {\"name\":\"backend experiments skip unavailable paths cleanly\",\"status\":\"" << (backendRc == 0 ? "pass" : "fail") << "\"},\n";
    out << "    {\"name\":\"UI G70 release checks\",\"status\":\"external\",\"reason\":\"covered by release_gate_check script UI smoke step when Qt smoke tool is present\"}\n";
    out << "  ]\n";
    out << "}\n";
  }
  std::cout << "results: " << outPath.string() << "\n";
  return (scene_fail == 0 && runRc == 0 && artifactValidation.ok && threadRc == 0 && backendRc == 0) ? 0 : 2;
}

int RunExperimentsCommand(const std::vector<std::string_view>& args) {
  std::string scenePack = "core";
  std::vector<std::string> includes;
  std::string output = "artifacts/benchmarks/experiments";
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--scene-pack") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --scene-pack value\n";
        return 1;
      }
      scenePack = std::string(args[++i]);
    } else if (args[i] == "--include") {
      if (i + 1 >= args.size()) {
        std::cerr << "missing --include value\n";
        return 1;
      }
      includes.push_back(std::string(args[++i]));
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
  auto lower = [](const std::string& value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lowered;
  };
  bool hasCpu = includes.empty();
  bool hasGpu = false;
  for (auto& include : includes) {
    const auto lowered = lower(include);
    if (lowered == "cpu" || lowered == "cpu-scalar" || lowered == "scalar") {
      hasCpu = true;
    }
    if (lowered == "gpu" || lowered == "gpu-compute" || lowered == "gpu-paths" || lowered == "vulkan") {
      hasGpu = true;
    }
  }
  if (!hasCpu && !hasGpu) {
    hasCpu = true;
  }

  Path inputDir = (scenePack == "core") ? Path("assets/scenes") : Path(scenePack);
  std::error_code ec;
  if (!std::filesystem::exists(inputDir, ec) || ec) {
    std::cerr << "missing scene pack path: " << inputDir.string() << "\n";
    return 2;
  }
  if (scenePack.empty()) {
    std::cerr << "missing --scene-pack\n";
    return 1;
  }

  std::vector<Path> scenes;
  for (std::filesystem::directory_iterator it(inputDir, std::filesystem::directory_options::skip_permission_denied, ec),
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
    scenes.push_back(entry.path());
  }
  if (scenes.empty()) {
    std::cerr << "no scenes in pack: " << scenePack << "\n";
    return 2;
  }

  std::vector<std::pair<std::string, std::string>> runs;
  if (hasCpu) {
    runs.push_back({"cpu", "cpu-scalar"});
  }
  if (hasGpu) {
    runs.push_back({"vulkan", "gpu-compute"});
  }

  std::size_t passed = 0;
  std::size_t failed = 0;
  for (const auto& scene : scenes) {
    for (const auto& run : runs) {
      std::vector<std::string> sceneArgs = {"ptbench", "run", "--scene", scene.string(), "--backend", run.first,
                                           "--renderer-path", run.second, "--resolution", "128x128", "--spp", "2", "--output",
                                           (Path(output) / scene.stem() / run.second).string()};
      std::vector<std::string_view> cargs;
      for (const auto& item : sceneArgs) {
        cargs.emplace_back(item);
      }
      const int rc = RunCommand(cargs);
      if (rc == 0) {
        ++passed;
      } else {
        ++failed;
      }
    }
  }
  std::cout << "run-experiments summary: passed=" << passed << " failed=" << failed << "\n";
  return failed == 0 ? 0 : 2;
}

}  // namespace vkpt::benchmark::ptbench
