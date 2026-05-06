#include "benchmark/BenchmarkRuntimeInternal.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vkpt::benchmark::ptbench {

void ApplyRunDesc(const vkpt::benchmark::BenchmarkRunDesc& desc, RunOptions& out) {
  out.scenePath = desc.scene_path;
  out.backend = desc.backend;
  out.rendererPath = desc.renderer_path;
  out.width = desc.resolution.width;
  out.height = desc.resolution.height;
  out.spp = desc.samples_per_pixel;
  out.duration = desc.duration;
  out.warmupFrames = desc.warmup_frames;
  out.seed = desc.seed;
  out.output = desc.output_directory;
  out.referenceImage = desc.reference_image;
  out.tolerance = desc.tolerance_policy;
  out.maxDepth = desc.max_depth;
  out.workers = desc.worker_count;
  out.tileHeight = desc.tile_height;
  out.deterministic = desc.deterministic;
}

vkpt::benchmark::BenchmarkRunDesc ToRunDesc(const RunOptions& opts) {
  vkpt::benchmark::BenchmarkRunDesc desc;
  desc.scene_path = opts.scenePath;
  desc.backend = opts.backend;
  desc.renderer_path = opts.rendererPath;
  desc.resolution.width = opts.width;
  desc.resolution.height = opts.height;
  desc.samples_per_pixel = opts.spp;
  desc.duration = opts.duration;
  desc.warmup_frames = opts.warmupFrames;
  desc.seed = opts.seed;
  desc.output_directory = opts.output;
  desc.reference_image = opts.referenceImage;
  desc.tolerance_policy = opts.tolerance;
  desc.max_depth = opts.maxDepth;
  desc.worker_count = opts.workers;
  desc.tile_height = opts.tileHeight;
  desc.deterministic = opts.deterministic;
  return desc;
}

bool ParseRunArgs(const std::vector<std::string_view>& args, RunOptions& out, std::string* error) {
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--desc") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --desc value";
        return false;
      }
      out.descPath = std::string(args[++i]);
      const auto desc = vkpt::benchmark::LoadBenchmarkRunDescFromFile(out.descPath);
      if (!desc) {
        if (error) *error = "failed to load benchmark descriptor: " + out.descPath;
        return false;
      }
      ApplyRunDesc(desc.value(), out);
    }
  }

  for (std::size_t i = 2; i < args.size(); ++i) {
    const auto token = args[i];
    if (token == "--desc") {
      ++i;
    } else if (token == "--echo-desc") {
      out.echoDesc = true;
    } else if (token == "--scene") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --scene value";
        return false;
      }
      out.scenePath = std::string(args[++i]);
    } else if (token == "--backend") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --backend value";
        return false;
      }
      out.backend = std::string(args[++i]);
    } else if (token == "--renderer-path") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --renderer-path value";
        return false;
      }
      out.rendererPath = std::string(args[++i]);
    } else if (token == "--output") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --output value";
        return false;
      }
      out.output = std::string(args[++i]);
    } else if (token == "--reference-image") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --reference-image value";
        return false;
      }
      out.referenceImage = std::string(args[++i]);
    } else if (token == "--tolerance-policy") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --tolerance-policy value";
        return false;
      }
      out.tolerance = std::string(args[++i]);
    } else if (token == "--resolution") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --resolution value";
        return false;
      }
      std::uint32_t w = 0;
      std::uint32_t h = 0;
      if (!ParseResolution(args[++i], w, h) || w == 0 || h == 0) {
        if (error) *error = "invalid --resolution";
        return false;
      }
      out.width = w;
      out.height = h;
    } else if (token == "--spp") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --spp value";
        return false;
      }
      if (!ParseUnsigned(args[++i], out.spp) || out.spp == 0) {
        if (error) *error = "invalid --spp";
        return false;
      }
    } else if (token == "--seed") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --seed value";
        return false;
      }
      if (!ParseUnsigned64(args[++i], out.seed)) {
        if (error) *error = "invalid --seed";
        return false;
      }
    } else if (token == "--max-depth") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --max-depth value";
        return false;
      }
      if (!ParseUnsigned(args[++i], out.maxDepth) || out.maxDepth == 0) {
        if (error) *error = "invalid --max-depth";
        return false;
      }
    } else if (token == "--duration") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --duration value";
        return false;
      }
      const auto value = args[++i];
      auto numeric = value;
      if (!numeric.empty() && numeric.front() == '+') {
        numeric.remove_prefix(1);
      }
      const auto parsed = std::from_chars(numeric.data(), numeric.data() + numeric.size(), out.duration);
      if (numeric.empty() ||
          parsed.ec != std::errc{} ||
          parsed.ptr != numeric.data() + numeric.size() ||
          !std::isfinite(out.duration)) {
        if (error) *error = "invalid --duration";
        return false;
      }
    } else if (token == "--warmup-frames") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --warmup-frames value";
        return false;
      }
      if (!ParseUnsigned(args[++i], out.warmupFrames)) {
        if (error) *error = "invalid --warmup-frames";
        return false;
      }
    } else if (token == "--json") {
      out.json = true;
    } else if (token == "--workers") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --workers value";
        return false;
      }
      if (!ParseUnsigned(args[++i], out.workers)) {
        if (error) *error = "invalid --workers";
        return false;
      }
    } else if (token == "--tile-size") {
      if (i + 1 >= args.size()) {
        if (error) *error = "missing --tile-size value";
        return false;
      }
      if (!ParseUnsigned(args[++i], out.tileHeight) || out.tileHeight == 0) {
        if (error) *error = "invalid --tile-size (must be row count > 0)";
        return false;
      }
    } else if (token == "--deterministic") {
      out.deterministic = true;
    } else {
      if (error) {
        *error = "unknown option: " + std::string(token);
      }
      return false;
    }
  }
  return true;
}


}  // namespace vkpt::benchmark::ptbench
