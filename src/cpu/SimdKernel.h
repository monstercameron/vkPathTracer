#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "cpu/CpuFeatures.h"
#include "cpu/PacketRay.h"

namespace vkpt::cpu {

// Available SIMD execution modes.
// Build-time guards control which are compiled; runtime selection picks the best supported.
enum class SimdMode : uint8_t {
  Scalar   = 0,  // Portable scalar fallback (always available)
  NEON     = 1,  // ARM Advanced SIMD / NEON (AArch64, 128-bit, 4xf32)
  SVE      = 2,  // ARM Scalable Vector Extension (variable width)
  AVX      = 3,  // x86 AVX (256-bit, but only 128-bit float lanes used here)
  AVX2     = 4,  // x86 AVX2 (256-bit, 8xf32)
  AVX512   = 5,  // x86 AVX-512F (512-bit, 16xf32)
};

/// Human-readable name for a SIMD mode.
inline const char* SimdModeName(SimdMode m) {
  switch (m) {
    case SimdMode::Scalar: return "scalar";
    case SimdMode::NEON:   return "neon";
    case SimdMode::SVE:    return "sve";
    case SimdMode::AVX:    return "avx";
    case SimdMode::AVX2:   return "avx2";
    case SimdMode::AVX512: return "avx512";
    default:               return "unknown";
  }
}

/// Select the best SIMD mode available for this build and this machine.
///
/// Compile-time guards ensure the returned mode has code in this binary, while
/// CpuFeatureSet ensures the current CPU/OS can execute that code.
inline SimdMode SelectBestSimdMode(const CpuFeatureSet& features) {
  (void)features;
#if defined(__aarch64__) || defined(_M_ARM64)
  #if defined(__ARM_FEATURE_SVE)
    if (features.sve) return SimdMode::SVE;
  #endif
  if (features.neon) return SimdMode::NEON;
#elif defined(__x86_64__) || defined(_M_X64)
  #if defined(__AVX512F__)
    if (features.avx512f) return SimdMode::AVX512;
  #endif
  #if defined(__AVX2__)
    if (features.avx2) return SimdMode::AVX2;
  #endif
  #if defined(__AVX__)
    if (features.avx) return SimdMode::AVX;
  #endif
#endif
  return SimdMode::Scalar;
}

enum class PacketLanePolicy : uint8_t {
  ActivePrefix = 0,
  ZeroPaddedTail,
  ScalarTail,
  PreserveLaneOrder,
};

/// Static properties for one SIMD packet kernel implementation.
struct SimdKernelInfo {
  SimdMode mode = SimdMode::Scalar;
  uint32_t lane_width = 1u;
  PacketLanePolicy lane_policy = PacketLanePolicy::ActivePrefix;
  bool compiled = true;
  bool runtime_supported = true;
  std::string_view name = "scalar";
};

/// Interface for packet ray/triangle intersection kernels.
///
/// Implementations test one triangle against all active lanes in a RayPacket
/// and update HitPacket only when the new hit is closer than the current lane t.
class ISimdKernel {
 public:
  virtual ~ISimdKernel() = default;

  virtual SimdKernelInfo info() const = 0;
  virtual uint32_t intersect_triangle_packet(const RayPacket& packet,
                                             const TriangleSOA& triangle,
                                             HitPacket& hits) const = 0;
};

}  // namespace vkpt::cpu
