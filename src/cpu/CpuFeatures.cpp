#include "cpu/CpuFeatures.h"

#include <mutex>
#include <sstream>

#include "core/log/Log.h"

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

namespace {

std::once_flag g_simdSelectedLogOnce;

#if defined(VKPT_ARCH_X86)
uint64_t xgetbv0() {
#if defined(_MSC_VER)
  return _xgetbv(0);
#elif defined(__clang__) || defined(__GNUC__)
  uint32_t eax = 0;
  uint32_t edx = 0;
  __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
  return (static_cast<uint64_t>(edx) << 32u) | eax;
#else
  return 0;
#endif
}
#endif

SimdBackend SelectCompiledTelemetryBackend(const CpuFeatureSet& features) {
  (void)features;
#if defined(VKPT_ARCH_ARM64)
  if (features.neon) {
    return SimdBackend::ArmNeon;
  }
#elif defined(VKPT_ARCH_X86)
#if defined(__AVX512F__)
  if (features.avx512f) {
    return SimdBackend::X86Avx512;
  }
#endif
#if defined(__AVX2__)
  if (features.avx2) {
    return SimdBackend::X86Avx2;
  }
#endif
#endif
  return SimdBackend::Scalar;
}

const char* TelemetryKernelName(SimdBackend backend) {
  switch (backend) {
    case SimdBackend::ArmNeon:
      return "neon";
    case SimdBackend::X86Avx2:
      return "avx2";
    case SimdBackend::X86Avx512:
      return "avx512";
    case SimdBackend::Scalar:
    default:
      return "scalar";
  }
}

const char* SimdSelectionReason(SimdBackend backend) {
  return backend == SimdBackend::Scalar ? "fallback" : "cpu_feature";
}

}  // namespace

std::string ToString(SimdBackend backend) {
  switch (backend) {
    case SimdBackend::Scalar:    return "scalar";
    case SimdBackend::ArmNeon:   return "arm-neon";
    case SimdBackend::ArmVce:    return "arm-vce";
    case SimdBackend::ArmSme:   return "arm-sme";
    case SimdBackend::X86Sse:    return "x86-sse";
    case SimdBackend::X86Avx:   return "x86-avx";
    case SimdBackend::X86Avx2:  return "x86-avx2";
    case SimdBackend::X86Avx512: return "x86-avx512";
    case SimdBackend::X86Amx:   return "x86-amx";
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

  // Probe CPUID leaf 1 for SSE/AVX baseline. AVX requires both CPU support
  // and OS-managed XMM/YMM state, so gate it through XGETBV.
  bool os_avx = false;
  bool os_avx512 = false;
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
      const bool cpu_avx = ((ecx >> 28) & 1u) != 0u;
      const bool osxsave = ((ecx >> 27) & 1u) != 0u;
      const uint64_t xcr0 = osxsave ? xgetbv0() : 0u;
      os_avx = (xcr0 & 0x6u) == 0x6u;
      os_avx512 = (xcr0 & 0xe6u) == 0xe6u;
      f.avx = cpu_avx && os_avx;
      f.fma = f.avx && (((ecx >> 12) & 1u) != 0u);
    }
  }

  // Probe CPUID leaf 7 subleaf 0 for AVX2 / AVX-512. AVX-512 is exposed only
  // when opmask and ZMM state are enabled by the OS.
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
      f.avx2     = f.avx && (((ebx >>  5) & 1u) != 0u);
      f.avx512f  = os_avx512 && (((ebx >> 16) & 1u) != 0u);
      f.avx512dq = os_avx512 && (((ebx >> 17) & 1u) != 0u);
      f.avx512bw = os_avx512 && (((ebx >> 30) & 1u) != 0u);
      f.avx512vl = os_avx512 && (((ebx >> 31) & 1u) != 0u);
    }
  }

#else
  f.architecture = "unknown";
#endif

  return f;
}

std::string SelectedSimdKernelName(const CpuFeatureSet& features) {
  return TelemetryKernelName(SelectCompiledTelemetryBackend(features));
}

SimdDispatchInfo BuildSimdDispatchInfo(const CpuFeatureSet& f) {
  const SimdBackend selectedBackend = SelectCompiledTelemetryBackend(f);
  std::call_once(g_simdSelectedLogOnce, [&]() {
    VKP_LOG(Info,
            "cpu",
            "simd_selected",
            "kernel",
            TelemetryKernelName(selectedBackend),
            "reason",
            SimdSelectionReason(selectedBackend));
  });

  SimdDispatchInfo out;
  out.available.push_back(SimdBackend::Scalar);

  if (f.architecture == "aarch64") {
    // Prefer wider or more specialized ARM modes, but keep all supported
    // backends in the available list for diagnostics and manual selection.
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
    // Prefer the widest compiled x86 backend supported by both CPU and OS.
    if (f.sse2)  { out.available.push_back(SimdBackend::X86Sse); }
    if (f.avx)   { out.available.push_back(SimdBackend::X86Avx); }
    if (f.avx2)  { out.available.push_back(SimdBackend::X86Avx2); }
    if (f.avx512f) { out.available.push_back(SimdBackend::X86Avx512); }

    if (f.avx512f)       { out.preferred = SimdBackend::X86Avx512; }
    else if (f.avx2)     { out.preferred = SimdBackend::X86Avx2; }
    else if (f.avx)      { out.preferred = SimdBackend::X86Avx; }
    else if (f.sse2)     { out.preferred = SimdBackend::X86Sse; }
    else                 { out.preferred = SimdBackend::Scalar; }
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
