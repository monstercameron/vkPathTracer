#include "benchmark/BenchmarkRuntime.h"
#include "benchmark/BenchmarkRuntimeInternal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "benchmark/BenchmarkSchema.h"
#include "cpu/CpuFeatures.h"
#include "cpu/PacketRay.h"
#include "cpu/SimdKernel.h"
#include "cpu/SimdKernelScalar.h"
#include "cpu/SimdKernelNeon.h"
#include "cpu/SimdKernelSve.h"
#include "cpu/SimdKernelAvx2.h"
#include "cpu/SimdKernelAvx512.h"

namespace vkpt::benchmark::ptbench {

using Path = std::filesystem::path;
// F09: SIMD sweep — measures raw ray/triangle intersection throughput for each available kernel.
std::vector<uint32_t> ParseWorkerList(const std::string& text) {
  std::vector<uint32_t> values;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto comma = text.find(',', start);
    const auto token = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!token.empty()) {
      std::uint32_t value = 0;
      if (!ParseUnsigned(token, value)) {
        return {};
      }
      if (value > 0u) {
        values.push_back(value);
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

int ThreadSweepCommand(const std::vector<std::string_view>& args) {
  std::string scene;
  uint32_t spp = 2;
  std::string resolution = "128x128";
  std::string output = "artifacts/thread_sweep";
  std::vector<uint32_t> workers;

  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--scene" && i + 1 < args.size()) {
      scene = std::string(args[++i]);
    } else if (args[i] == "--workers" && i + 1 < args.size()) {
      workers = ParseWorkerList(std::string(args[++i]));
      if (workers.empty()) {
        std::cerr << "invalid --workers list\n";
        return 1;
      }
    } else if (args[i] == "--spp" && i + 1 < args.size()) {
      if (!ParseUnsigned(args[++i], spp) || spp == 0u) {
        std::cerr << "invalid --spp\n";
        return 1;
      }
    } else if (args[i] == "--resolution" && i + 1 < args.size()) {
      resolution = std::string(args[++i]);
    } else if (args[i] == "--output" && i + 1 < args.size()) {
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  if (scene.empty()) {
    std::cerr << "thread-sweep requires --scene\n";
    return 1;
  }
  uint32_t width = 0;
  uint32_t height = 0;
  if (!ParseResolution(resolution, width, height) || width == 0u || height == 0u) {
    std::cerr << "invalid --resolution\n";
    return 1;
  }

  const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
  if (workers.empty()) {
    workers = {1u, 2u, 4u, 8u};
  }

  struct Row {
    uint32_t workers = 0;
    std::string status;
    std::string reason;
    double samples_per_sec = 0.0;
    double paths_per_sec = 0.0;
    double normalized_score = 0.0;
    double speedup_vs_one_worker = 0.0;
    std::string artifact_dir;
  };
  std::vector<Row> rows;
  double oneWorkerSamples = 0.0;

  // Each sweep entry is a full benchmark run with its own artifact directory;
  // skipped rows remain in the JSON so dashboards can show unsupported counts.
  for (const auto workerCount : workers) {
    Row row;
    row.workers = workerCount;
    row.artifact_dir = (Path(output) / ("workers_" + std::to_string(workerCount))).string();
    if (workerCount > hardware) {
      row.status = "skipped";
      row.reason = "worker count exceeds hardware concurrency";
      rows.push_back(std::move(row));
      continue;
    }

    vkpt::benchmark::BenchmarkRunDesc desc;
    desc.scene_path = scene;
    desc.backend = "cpu";
    desc.renderer_path = "cpu-tiled";
    desc.resolution.width = width;
    desc.resolution.height = height;
    desc.samples_per_pixel = spp;
    desc.duration = 0.0;
    desc.warmup_frames = 0;
    desc.seed = 0xBADC0FFEEull;
    desc.output_directory = row.artifact_dir;
    desc.tolerance_policy = "abs=0.001";
    desc.max_depth = 6;
    desc.worker_count = workerCount;
    desc.tile_height = 16;

    const auto result = RunCliBenchmarkOnce(desc);
    if (!result) {
      row.status = "failed";
      row.reason = "benchmark run failed";
    } else {
      row.status = "ok";
      row.samples_per_sec = result.value().throughput.samples_per_sec;
      row.paths_per_sec = result.value().throughput.paths_per_sec;
      row.normalized_score = result.value().score.normalized_score;
      if (workerCount == 1u) {
        oneWorkerSamples = std::max(1.0, row.samples_per_sec);
      }
      if (oneWorkerSamples > 0.0) {
        row.speedup_vs_one_worker = row.samples_per_sec / oneWorkerSamples;
      }
    }
    rows.push_back(std::move(row));
  }

  EnsureDirectory(Path(output));
  const Path outPath = Path(output) / "thread_sweep.json";
  std::ofstream out(outPath);
  if (out.is_open()) {
    out << "{\n";
    out << "  \"scene\":\"" << EscapeJson(scene) << "\",\n";
    out << "  \"hardware_threads\":" << hardware << ",\n";
    out << "  \"rows\":[\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& row = rows[i];
      out << "    {\"workers\":" << row.workers
          << ",\"status\":\"" << EscapeJson(row.status) << "\""
          << ",\"reason\":\"" << EscapeJson(row.reason) << "\""
          << ",\"samples_per_sec\":" << std::fixed << std::setprecision(4) << row.samples_per_sec
          << ",\"paths_per_sec\":" << std::fixed << std::setprecision(4) << row.paths_per_sec
          << ",\"normalized_score\":" << std::fixed << std::setprecision(4) << row.normalized_score
          << ",\"speedup_vs_one_worker\":" << std::fixed << std::setprecision(4) << row.speedup_vs_one_worker
          << ",\"artifact_dir\":\"" << EscapeJson(row.artifact_dir) << "\"}";
      if (i + 1 < rows.size()) out << ",";
      out << "\n";
    }
    out << "  ]\n}\n";
  }
  std::cout << "results: " << outPath.string() << "\n";
  const bool failed = std::any_of(rows.begin(), rows.end(), [](const Row& row) { return row.status == "failed"; });
  return failed ? 2 : 0;
}

int SimdSweepCommand(const std::vector<std::string_view>& args) {
  uint64_t rayCount   = 1'000'000ULL;
  uint64_t triCount   = 1024ULL;
  std::string output  = "artifacts/simd_sweep";

  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--rays" && i + 1 < args.size()) {
      if (!ParseUnsigned64(args[++i], rayCount) || rayCount == 0u) {
        std::cerr << "invalid --rays\n";
        return 1;
      }
    } else if (args[i] == "--triangles" && i + 1 < args.size()) {
      if (!ParseUnsigned64(args[++i], triCount) || triCount == 0u) {
        std::cerr << "invalid --triangles\n";
        return 1;
      }
    } else if (args[i] == "--output" && i + 1 < args.size()) {
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }

  // Generate pseudo-random triangles and rays
  auto lcg = [](uint64_t& s) -> float {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<float>((s >> 33) & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
  };

  std::vector<vkpt::cpu::TriangleSOA> triangles(triCount);
  uint64_t seed = 0xDEADBEEF12345678ULL;
  for (auto& tri : triangles) {
    const float v0x = lcg(seed) * 10.0f - 5.0f;
    const float v0y = lcg(seed) * 10.0f - 5.0f;
    const float v0z = lcg(seed) * 10.0f - 5.0f;
    tri.v0x = v0x; tri.v0y = v0y; tri.v0z = v0z;
    tri.e1x = lcg(seed) * 2.0f - 1.0f;
    tri.e1y = lcg(seed) * 2.0f - 1.0f;
    tri.e1z = lcg(seed) * 2.0f - 1.0f;
    tri.e2x = lcg(seed) * 2.0f - 1.0f;
    tri.e2y = lcg(seed) * 2.0f - 1.0f;
    tri.e2z = lcg(seed) * 2.0f - 1.0f;
    tri.material_index = 0;
  }

  // Build a ray packet (reuse same packet for the benchmark, like a hot cache)
  constexpr uint32_t kPacketW = 4u;
  vkpt::cpu::RayPacket packet{};
  packet.count = kPacketW;
  seed = 0xCAFEBABECAFEBABEULL;
  for (uint32_t i = 0; i < kPacketW; ++i) {
    packet.origin_x[i] = lcg(seed) * 2.0f - 1.0f;
    packet.origin_y[i] = lcg(seed) * 2.0f - 1.0f;
    packet.origin_z[i] = (lcg(seed) + 1.0f) * 5.0f;
    const float dx = lcg(seed) * 0.1f;
    const float dy = lcg(seed) * 0.1f;
    const float dz = -1.0f;
    const float inv_len = 1.0f / std::sqrt(dx*dx + dy*dy + dz*dz);
    packet.dir_x[i] = dx * inv_len;
    packet.dir_y[i] = dy * inv_len;
    packet.dir_z[i] = dz * inv_len;
  }

  struct SweepResult {
    std::string mode_name;
    uint64_t total_rays = 0;
    double mrays_per_sec = 0.0;
    bool available = false;
    std::string status = "skipped";
    std::string skip_reason;
    double speedup_vs_scalar = 0.0;
  };

  std::vector<SweepResult> results;

  auto bench_mode = [&](const char* name, bool available, auto fn) {
    SweepResult r;
    r.mode_name = name;
    r.available = available;
    if (!available) {
      r.status = "skipped";
      r.skip_reason = "not supported by this CPU or build";
      results.push_back(r);
      return;
    }
    r.status = "ok";
    vkpt::cpu::HitPacket hits{};
    vkpt::cpu::reset_hit_packet(hits, kPacketW);
    // Warm-up
    for (uint64_t t = 0; t < 1000ULL; ++t) fn(packet, triangles[t % triCount], hits);
    vkpt::cpu::reset_hit_packet(hits, kPacketW);
    const auto t0 = std::chrono::high_resolution_clock::now();
    const uint64_t total_pairs = rayCount / kPacketW;
    for (uint64_t t = 0; t < total_pairs; ++t) fn(packet, triangles[t % triCount], hits);
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.total_rays = total_pairs * kPacketW;
    r.mrays_per_sec = (ms > 0.0) ? (static_cast<double>(r.total_rays) / ms / 1000.0) : 0.0;
    results.push_back(r);
    std::cout << "  " << name << ": " << std::fixed << std::setprecision(2) << r.mrays_per_sec << " Mrays/s\n";
  };

  const auto cpuFeatures = vkpt::cpu::QueryCpuFeatures();
  const auto bestMode    = vkpt::cpu::SelectBestSimdMode(cpuFeatures);
  std::cout << "simd-sweep: " << rayCount << " rays x " << triCount << " triangles\n";
  std::cout << "cpu: " << cpuFeatures.architecture << ", best_mode=" << vkpt::cpu::SimdModeName(bestMode) << "\n";

  bench_mode("scalar", true,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_scalar(p, t, h);
    });

#if defined(__ARM_NEON)
  bench_mode("neon", cpuFeatures.neon,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_neon(p, t, h);
    });
#else
  results.push_back({"neon", 0, 0.0, false, "skipped", "not compiled for ARM NEON", 0.0});
#endif

#if defined(__ARM_FEATURE_SVE)
  bench_mode("sve", cpuFeatures.sve,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_sve(p, t, h);
    });
#else
  results.push_back({"sve", 0, 0.0, false, "skipped", "not compiled for ARM SVE", 0.0});
#endif

#if defined(__AVX2__)
  bench_mode("avx2", cpuFeatures.avx2,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_avx2_full(p, t, h);
    });
#else
  results.push_back({"avx2", 0, 0.0, false, "skipped", "not compiled for AVX2", 0.0});
#endif

#if defined(__AVX512F__)
  bench_mode("avx512", cpuFeatures.avx512f,
    [](const vkpt::cpu::RayPacket& p, const vkpt::cpu::TriangleSOA& t, vkpt::cpu::HitPacket& h) {
      vkpt::cpu::intersect_triangle_packet_avx512(p, t, h);
    });
#else
  results.push_back({"avx512", 0, 0.0, false, "skipped", "not compiled for AVX-512", 0.0});
#endif

  // Find best available
  std::string best_name = "scalar";
  double best_mrays = 0.0;
  double scalar_mrays = 0.0;
  for (const auto& r : results) {
    if (r.available && r.mode_name == "scalar") {
      scalar_mrays = std::max(1.0e-9, r.mrays_per_sec);
    }
  }
  for (auto& r : results) {
    if (r.available && scalar_mrays > 0.0) {
      r.speedup_vs_scalar = r.mrays_per_sec / scalar_mrays;
    }
  }
  for (const auto& r : results) {
    if (r.available && r.mrays_per_sec > best_mrays) {
      best_mrays = r.mrays_per_sec;
      best_name = r.mode_name;
    }
  }
  std::cout << "best: " << best_name << " (" << std::fixed << std::setprecision(2) << best_mrays << " Mrays/s)\n";

  // Write JSON output
  EnsureDirectory(Path(output));
  const Path outPath = Path(output) / "simd_sweep.json";
  std::ofstream jf(outPath);
  if (jf.is_open()) {
    jf << "{\n";
    jf << "  \"architecture\":\"" << cpuFeatures.architecture << "\",\n";
    jf << "  \"best_mode\":\"" << best_name << "\",\n";
    jf << "  \"results\":[\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
      const auto& r = results[i];
      jf << "    {\"mode\":\"" << r.mode_name << "\",\"available\":" << (r.available ? "true" : "false")
         << ",\"status\":\"" << EscapeJson(r.status) << "\""
         << ",\"skip_reason\":\"" << EscapeJson(r.skip_reason) << "\""
         << ",\"mrays_per_sec\":" << std::fixed << std::setprecision(4) << r.mrays_per_sec
         << ",\"speedup_vs_scalar\":" << std::fixed << std::setprecision(4) << r.speedup_vs_scalar << "}";
      if (i + 1 < results.size()) jf << ",";
      jf << "\n";
    }
    jf << "  ]\n}\n";
    std::cout << "results: " << outPath.string() << "\n";
  }
  return 0;
}

// F10: Tile-size sweep — runs cpu-tiled with different tile heights and measures throughput.
int TileSweepCommand(const std::vector<std::string_view>& args) {
  std::string scene;
  uint32_t workers = 0;
  uint32_t spp = 4;
  std::string resolution = "128x128";
  std::string output = "artifacts/tile_sweep";

  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--scene" && i + 1 < args.size()) {
      scene = std::string(args[++i]);
    } else if (args[i] == "--workers" && i + 1 < args.size()) {
      if (!ParseUnsigned(args[++i], workers)) {
        std::cerr << "invalid --workers\n";
        return 1;
      }
    } else if (args[i] == "--spp" && i + 1 < args.size()) {
      if (!ParseUnsigned(args[++i], spp) || spp == 0u) {
        std::cerr << "invalid --spp\n";
        return 1;
      }
    } else if (args[i] == "--resolution" && i + 1 < args.size()) {
      resolution = std::string(args[++i]);
    } else if (args[i] == "--output" && i + 1 < args.size()) {
      output = std::string(args[++i]);
    } else {
      std::cerr << "unknown option: " << args[i] << "\n";
      return 1;
    }
  }
  if (scene.empty()) {
    std::cerr << "tile-sweep requires --scene\n";
    return 1;
  }

  const uint32_t kTileSizes[] = {8, 16, 32, 64};
  struct TileResult {
    uint32_t tile_height;
    double samples_per_sec;
    double normalized_score;
    double render_ms;
    bool ok;
    std::string status;
    std::string reason;
  };
  std::vector<TileResult> results;

  std::cout << "tile-sweep: " << scene << " workers=" << workers << " spp=" << spp << "\n";

  for (const uint32_t tile_h : kTileSizes) {
    const Path tileOut = Path(output) / (std::string("tile") + std::to_string(tile_h));
    const std::vector<std::string> call_args = {
      "ptbench", "run",
      "--scene", scene,
      "--backend", "cpu",
      "--renderer-path", "cpu-tiled",
      "--resolution", resolution,
      "--spp", std::to_string(spp),
      "--workers", std::to_string(workers),
      "--tile-size", std::to_string(tile_h),
      "--output", tileOut.string()
    };
    std::vector<std::string_view> cargs;
    for (const auto& a : call_args) cargs.emplace_back(a);

    const auto t0 = std::chrono::high_resolution_clock::now();
    const int rc = RunCommand(cargs);
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    TileResult tr{};
    tr.tile_height = tile_h;
    tr.ok = (rc == 0);
    tr.status = tr.ok ? "ok" : "failed";
    tr.reason = tr.ok ? "" : "benchmark run failed";
    tr.render_ms = elapsed_ms;

    if (tr.ok) {
      const Path res_path = tileOut / "results.json";
      const auto parsed = vkpt::benchmark::LoadBenchmarkResultFromFile(res_path.string());
      if (parsed) {
        tr.samples_per_sec = parsed.value().throughput.samples_per_sec;
        tr.normalized_score = parsed.value().score.normalized_score;
      } else {
        tr.ok = false;
        tr.status = "failed";
        tr.reason = "results.json failed schema validation";
      }
    }
    results.push_back(tr);
    std::cout << "  tile_height=" << tile_h << ": ";
    if (tr.ok) {
      std::cout << std::fixed << std::setprecision(2) << tr.samples_per_sec / 1e6 << " Msamples/s";
    } else {
      std::cout << "FAILED";
    }
    std::cout << "\n";
  }

  // Find best
  uint32_t best_tile = 16;
  double best_sps = 0.0;
  for (const auto& r : results) {
    if (r.ok && r.samples_per_sec > best_sps) {
      best_sps = r.samples_per_sec;
      best_tile = r.tile_height;
    }
  }
  std::cout << "best tile_height: " << best_tile << " (" << std::fixed << std::setprecision(2) << best_sps / 1e6 << " Msamples/s)\n";

  // Write JSON
  EnsureDirectory(Path(output));
  const Path outPath = Path(output) / "tile_sweep.json";
  std::ofstream jf(outPath);
  if (jf.is_open()) {
    jf << "{\n";
    jf << "  \"scene\":\"" << EscapeJson(scene) << "\",\n";
    jf << "  \"best_tile_height\":" << best_tile << ",\n";
    jf << "  \"results\":[\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
      const auto& r = results[i];
      jf << "    {\"tile_height\":" << r.tile_height
         << ",\"ok\":" << (r.ok ? "true" : "false")
         << ",\"status\":\"" << EscapeJson(r.status) << "\""
         << ",\"reason\":\"" << EscapeJson(r.reason) << "\""
         << ",\"tile_shape\":\"full-width-row-band\""
         << ",\"samples_per_sec\":" << std::fixed << std::setprecision(2) << r.samples_per_sec
         << ",\"normalized_score\":" << std::fixed << std::setprecision(2) << r.normalized_score
         << ",\"render_ms\":" << std::fixed << std::setprecision(2) << r.render_ms
         << "}";
      if (i + 1 < results.size()) jf << ",";
      jf << "\n";
    }
    jf << "  ]\n}\n";
    std::cout << "results: " << outPath.string() << "\n";
  }
  return 0;
}
}  // namespace vkpt::benchmark::ptbench
