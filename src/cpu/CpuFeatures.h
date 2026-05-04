#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vkpt::cpu {

enum class SimdBackend : std::uint8_t {
  Scalar = 0,
  ArmNeon,
  ArmVce,
  ArmSme,
  X86Sse,
  X86Avx,
  X86Avx2,
  X86Avx512,
  X86Amx,
};

struct SimdDispatchInfo {
  SimdBackend preferred = SimdBackend::Scalar;
  std::vector<SimdBackend> available;
};

std::string ToString(SimdBackend backend);

// Runtime-detected CPU SIMD feature flags.
struct CpuFeatureSet {
  // Architecture string: "aarch64", "x86_64", or "unknown"
  std::string architecture = "unknown";

  // x86 / x86_64 features (false on non-x86 builds)
  bool sse2    = false;
  bool sse4_1  = false;
  bool sse4_2  = false;
  bool avx     = false;
  bool avx2    = false;
  bool avx512f = false;
  bool avx512dq= false;
  bool avx512bw= false;
  bool avx512vl= false;
  bool fma     = false;

  // ARM features (false on non-ARM builds)
  bool neon        = false;
  bool vce         = false;
  bool sve         = false;
  bool sme         = false;
  bool sve2        = false;
  bool fp16        = false;
  bool dot_product = false;
};

// Query CPU features at runtime. Result is stable for the process lifetime.
CpuFeatureSet QueryCpuFeatures();
SimdDispatchInfo BuildSimdDispatchInfo(const CpuFeatureSet& features);

// Serialize feature set as a JSON object string.
std::string SerializeCpuFeatures(const CpuFeatureSet& features);

}  // namespace vkpt::cpu
