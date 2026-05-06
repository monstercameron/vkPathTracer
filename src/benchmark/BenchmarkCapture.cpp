#include "benchmark/BenchmarkRuntime.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "benchmark/BenchmarkRuntimeInternal.h"

namespace vkpt::benchmark::ptbench {

namespace {

std::string TrimText(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1u));
}

}  // namespace

std::string ReadProcessEnvRaw(const char* name) {
  std::string valueText;
#if defined(_WIN32)
  char* valueBuffer = nullptr;
  size_t valueLength = 0u;
  if (_dupenv_s(&valueBuffer, &valueLength, name) == 0 && valueBuffer != nullptr) {
    valueText.assign(valueBuffer, valueLength > 0u ? valueLength - 1u : 0u);
    std::free(valueBuffer);
  }
#else
  if (const char* value = std::getenv(name)) {
    valueText = value;
  }
#endif
  return valueText;
}

bool ReadProcessEnvBool(const char* name) {
  std::string value = ReadProcessEnvRaw(name);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::string ReadProcessEnvOr(const char* name, const char* fallback) {
  const std::string value = ReadProcessEnvRaw(name);
  return value.empty() ? std::string(fallback) : value;
}

void SetProcessEnvVar(const char* name, const std::string& value) {
  if (value.empty()) {
    return;
  }
#if defined(_WIN32)
  (void)_putenv_s(name, value.c_str());
#else
  (void)setenv(name, value.c_str(), 1);
#endif
}

std::vector<std::string> BuildPixAutorunArgs(const std::filesystem::path& configPath) {
  std::string scene = ReadProcessEnvOr("PTBENCH_SCENE", "assets/scenes/cornell_native.json");
  std::string backend = ReadProcessEnvOr("PTBENCH_BACKEND", "d3d12");
  std::string rendererPath = ReadProcessEnvOr("PTBENCH_RENDERER_PATH", "d3d12-compute");
  std::string resolution = ReadProcessEnvOr("PTBENCH_RESOLUTION", "512x512");
  std::string spp = ReadProcessEnvOr("PTBENCH_SPP", "8");
  std::string maxDepth = ReadProcessEnvOr("PTBENCH_MAX_DEPTH", "6");
  std::string output = ReadProcessEnvOr("PTBENCH_OUTPUT", "artifacts/pix_probe/autocapture/bench");
  std::string d3d12RaysPerPixel = ReadProcessEnvRaw("PT_D3D12_RAYS_PER_PIXEL");
  std::string d3d12ShaderTraversal = ReadProcessEnvRaw("PT_D3D12_SHADER_TRAVERSAL");
  std::string d3d12PackedTriangles = ReadProcessEnvRaw("PT_D3D12_PACKED_TRIANGLES");
  std::string d3d12ReadbackInterval = ReadProcessEnvRaw("PT_D3D12_READBACK_INTERVAL");
  std::string d3d12CommandQueue = ReadProcessEnvRaw("PT_D3D12_COMMAND_QUEUE");
  std::string pixProgrammaticCapture = ReadProcessEnvRaw("PTBENCH_PIX_PROGRAMMATIC_CAPTURE");
  std::string pixCapturePath = ReadProcessEnvRaw("PTBENCH_PIX_CAPTURE_PATH");

  if (!configPath.empty()) {
    std::ifstream in(configPath);
    std::string line;
    while (std::getline(in, line)) {
      const auto comment = line.find('#');
      if (comment != std::string::npos) {
        line = line.substr(0, comment);
      }
      const auto eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      const std::string key = TrimText(std::string_view(line).substr(0, eq));
      const std::string value = TrimText(std::string_view(line).substr(eq + 1u));
      if (value.empty()) {
        continue;
      }
      if (key == "scene") scene = value;
      else if (key == "backend") backend = value;
      else if (key == "renderer_path") rendererPath = value;
      else if (key == "resolution") resolution = value;
      else if (key == "spp") spp = value;
      else if (key == "max_depth") maxDepth = value;
      else if (key == "output") output = value;
      else if (key == "d3d12_rays_per_pixel") d3d12RaysPerPixel = value;
      else if (key == "d3d12_shader_traversal") d3d12ShaderTraversal = value;
      else if (key == "d3d12_packed_triangles") d3d12PackedTriangles = value;
      else if (key == "d3d12_readback_interval") d3d12ReadbackInterval = value;
      else if (key == "d3d12_command_queue") d3d12CommandQueue = value;
      else if (key == "pix_programmatic_capture") pixProgrammaticCapture = value;
      else if (key == "pix_capture_path") pixCapturePath = value;
    }
  }

  SetProcessEnvVar("PT_D3D12_RAYS_PER_PIXEL", d3d12RaysPerPixel);
  SetProcessEnvVar("PT_D3D12_SHADER_TRAVERSAL", d3d12ShaderTraversal);
  SetProcessEnvVar("PT_D3D12_PACKED_TRIANGLES", d3d12PackedTriangles);
  SetProcessEnvVar("PT_D3D12_READBACK_INTERVAL", d3d12ReadbackInterval);
  SetProcessEnvVar("PT_D3D12_COMMAND_QUEUE", d3d12CommandQueue);
  SetProcessEnvVar("PTBENCH_PIX_PROGRAMMATIC_CAPTURE", pixProgrammaticCapture);
  SetProcessEnvVar("PTBENCH_PIX_CAPTURE_PATH", pixCapturePath);

  std::vector<std::string> args;
  args.reserve(18);
  args.push_back("ptbench");
  args.push_back("run");
  args.push_back("--scene");
  args.push_back(scene);
  args.push_back("--backend");
  args.push_back(backend);
  args.push_back("--renderer-path");
  args.push_back(rendererPath);
  args.push_back("--resolution");
  args.push_back(resolution);
  args.push_back("--spp");
  args.push_back(spp);
  args.push_back("--max-depth");
  args.push_back(maxDepth);
  args.push_back("--output");
  args.push_back(output);
  args.push_back("--json");
  return args;
}

std::vector<std::string_view> MakeArgViews(const std::vector<std::string>& args) {
  std::vector<std::string_view> views;
  views.reserve(args.size());
  for (const auto& arg : args) {
    views.emplace_back(arg);
  }
  return views;
}

}  // namespace vkpt::benchmark::ptbench
