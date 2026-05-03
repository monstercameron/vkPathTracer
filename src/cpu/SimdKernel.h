#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "cpu/CpuFeatures.h"

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

// Human-readable name for a SIMD mode.
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

// Select the best SIMD mode available for this build and this machine.
// Prefers: AVX512 > AVX2 > AVX > SSE4 (x86) | SVE > NEON (ARM).
// Falls back to Scalar if nothing else applies.
inline SimdMode SelectBestSimdMode(const CpuFeatureSet& features) {
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

}  // namespace vkpt::cpu
