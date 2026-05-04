#include "cpu/CpuFeatures.h"

#include <sstream>

// Architecture detection
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #define VKPT_ARCH_X86 1
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
  #define VKPT_ARCH_ARM64 1
#endif

#if defined(VKPT_ARCH_X86)
  #if defined(__clang__) || defined(__GNUC__)
    #include <cpuid.h>
    // __get_cpuid(leaf, &eax, &ebx, &ecx, &edx) -> bool
    // __get_cpuid_count(leaf, subleaf, &eax, &ebx, &ecx, &edx) -> bool
  #elif defined(_MSC_VER)
    #include <intrin.h>
    // __cpuid(info, leaf), __cpuidex(info, leaf, subleaf)
  #endif
#endif

namespace vkpt::cpu {

std::string ToString(SimdBackend backend) {
  switch (backend) {
    case SimdBackend::Scalar: return "scalar";
    case SimdBackend::ArmNeon: return "arm-neon";
    case SimdBackend::ArmVce: return "arm-vce";
    case SimdBackend::ArmSme: return "arm-sme";
    case SimdBackend::X86Sse: return "x86-sse";
    case SimdBackend::X86Avx: return "x86-avx";
    case SimdBackend::X86Amx: return "x86-amx";
    default: return "unknown";
  }
}

CpuFeatureSet QueryCpuFeatures() {
  CpuFeatureSet f;

#if defined(VKPT_ARCH_ARM64)
  f.architecture = "aarch64";

  // ARMv8 (AArch64) always includes NEON (Advanced SIMD).
  f.neon = true;

#if defined(__ARM_FEATURE_SVE)
  f.sve = true;
#endif
#if defined(__ARM_FEATURE_SME)
  f.sme = true;
#endif
#if defined(__ARM_FEATURE_SVE2)
  f.sve2 = true;
#endif
#if defined(__ARM_FEATURE_FP16_SCALAR_ARITHMETIC) || defined(__ARM_FP16_FORMAT_IEEE)
  f.fp16 = true;
#endif
#if defined(__ARM_FEATURE_DOTPROD)
  f.dot_product = true;
#endif

  // There is no stable portable compiler macro for "VCE" yet.
  // For now we treat VCE as available when vector-length-agnostic SVE paths are available.
  f.vce = f.sve || f.sve2;

#elif defined(VKPT_ARCH_X86)
  f.architecture = "x86_64";

  // Probe CPUID leaf 1 for SSE/AVX baseline
  {
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
#if defined(__clang__) || defined(__GNUC__)
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
#else
    int cpuInfo[4] = {};
    __cpuid(cpuInfo, 1);
    eax = cpuInfo[0]; ebx = cpuInfo[1]; ecx = cpuInfo[2]; edx = cpuInfo[3];
    if (true) {
#endif
      f.sse2   = (edx >> 26) & 1u;
      f.sse4_1 = (ecx >> 19) & 1u;
      f.sse4_2 = (ecx >> 20) & 1u;
      f.avx    = (ecx >> 28) & 1u;
      f.fma    = (ecx >> 12) & 1u;
    }
  }

  // Probe CPUID leaf 7 subleaf 0 for AVX2 / AVX-512
  {
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
#if defined(__clang__) || defined(__GNUC__)
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
#else
    int cpuInfo[4] = {};
    __cpuidex(cpuInfo, 7, 0);
    eax = cpuInfo[0]; ebx = cpuInfo[1]; ecx = cpuInfo[2]; edx = cpuInfo[3];
    if (true) {
#endif
      f.avx2     = (ebx >>  5) & 1u;
      f.avx512f  = (ebx >> 16) & 1u;
      f.avx512dq = (ebx >> 17) & 1u;
      f.avx512bw = (ebx >> 30) & 1u;
      f.avx512vl = (ebx >> 31) & 1u;
    }
  }

#else
  f.architecture = "unknown";
#endif

  return f;
}

SimdDispatchInfo BuildSimdDispatchInfo(const CpuFeatureSet& f) {
  SimdDispatchInfo out;
  out.available.push_back(SimdBackend::Scalar);

  if (f.architecture == "aarch64") {
    if (f.neon) {
      out.available.push_back(SimdBackend::ArmNeon);
    }
    if (f.vce) {
      out.available.push_back(SimdBackend::ArmVce);
    }
    if (f.sme) {
      out.available.push_back(SimdBackend::ArmSme);
    }

    if (f.sme) {
      out.preferred = SimdBackend::ArmSme;
    } else if (f.vce) {
      out.preferred = SimdBackend::ArmVce;
    } else if (f.neon) {
      out.preferred = SimdBackend::ArmNeon;
    } else {
      out.preferred = SimdBackend::Scalar;
    }
    return out;
  }

  if (f.architecture == "x86_64") {
    if (f.sse2) {
      out.available.push_back(SimdBackend::X86Sse);
    }
    if (f.avx || f.avx2 || f.avx512f) {
      out.available.push_back(SimdBackend::X86Avx);
    }

    // Placeholder until AMX runtime probing and kernels are added.
    const bool hasAmx = false;
    if (hasAmx) {
      out.available.push_back(SimdBackend::X86Amx);
    }

    if (hasAmx) {
      out.preferred = SimdBackend::X86Amx;
    } else if (f.avx || f.avx2 || f.avx512f) {
      out.preferred = SimdBackend::X86Avx;
    } else if (f.sse2) {
      out.preferred = SimdBackend::X86Sse;
    } else {
      out.preferred = SimdBackend::Scalar;
    }
    return out;
  }

  out.preferred = SimdBackend::Scalar;
  return out;
}

std::string SerializeCpuFeatures(const CpuFeatureSet& f) {
  std::ostringstream oss;
  auto b = [](bool v) -> const char* { return v ? "true" : "false"; };
  oss << "{\n";
  oss << "  \"architecture\":\"" << f.architecture << "\",\n";
  oss << "  \"x86\":{";
  oss <<   "\"sse2\":"    << b(f.sse2)    << ",";
  oss <<   "\"sse4_1\":"  << b(f.sse4_1)  << ",";
  oss <<   "\"sse4_2\":"  << b(f.sse4_2)  << ",";
  oss <<   "\"avx\":"     << b(f.avx)     << ",";
  oss <<   "\"avx2\":"    << b(f.avx2)    << ",";
  oss <<   "\"avx512f\":" << b(f.avx512f) << ",";
  oss <<   "\"avx512dq\":"<< b(f.avx512dq)<< ",";
  oss <<   "\"avx512bw\":"<< b(f.avx512bw)<< ",";
  oss <<   "\"avx512vl\":"<< b(f.avx512vl)<< ",";
  oss <<   "\"fma\":"     << b(f.fma)     << "},\n";
  oss << "  \"arm\":{";
  oss <<   "\"neon\":"        << b(f.neon)        << ",";
  oss <<   "\"vce\":"         << b(f.vce)         << ",";
  oss <<   "\"sve\":"         << b(f.sve)         << ",";
  oss <<   "\"sme\":"         << b(f.sme)         << ",";
  oss <<   "\"sve2\":"        << b(f.sve2)        << ",";
  oss <<   "\"fp16\":"        << b(f.fp16)        << ",";
  oss <<   "\"dot_product\":" << b(f.dot_product) << "}\n";
  oss << "}";
  return oss.str();
}

}  // namespace vkpt::cpu
