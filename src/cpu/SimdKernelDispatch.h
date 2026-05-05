#pragma once

#include "cpu/SimdKernel.h"
#include "cpu/SimdKernelScalar.h"

#if defined(__AVX__)
#include "cpu/SimdKernelAvx.h"
#endif
#if defined(__AVX2__)
#include "cpu/SimdKernelAvx2.h"
#endif
#if defined(__AVX512F__)
#include "cpu/SimdKernelAvx512.h"
#endif
#if defined(__ARM_NEON)
#include "cpu/SimdKernelNeon.h"
#endif
#if defined(__ARM_FEATURE_SVE)
#include "cpu/SimdKernelSve.h"
#endif

namespace vkpt::cpu {

inline const ISimdKernel& ScalarKernelInstance() {
  static const ScalarSimdKernel kernel;
  return kernel;
}

inline const ISimdKernel& KernelForMode(SimdMode mode) {
  switch (mode) {
#if defined(__AVX512F__)
    case SimdMode::AVX512: {
      static const Avx512SimdKernel kernel;
      return kernel;
    }
#endif
#if defined(__AVX2__)
    case SimdMode::AVX2: {
      static const Avx2SimdKernel kernel;
      return kernel;
    }
#endif
#if defined(__AVX__)
    case SimdMode::AVX: {
      static const AvxSimdKernel kernel;
      return kernel;
    }
#endif
#if defined(__ARM_FEATURE_SVE)
    case SimdMode::SVE: {
      static const SveSimdKernel kernel;
      return kernel;
    }
#endif
#if defined(__ARM_NEON)
    case SimdMode::NEON: {
      static const NeonSimdKernel kernel;
      return kernel;
    }
#endif
    default:
      return ScalarKernelInstance();
  }
}

inline const ISimdKernel& SelectSimdKernel(const CpuFeatureSet& features) {
  return KernelForMode(SelectBestSimdMode(features));
}

}  // namespace vkpt::cpu
