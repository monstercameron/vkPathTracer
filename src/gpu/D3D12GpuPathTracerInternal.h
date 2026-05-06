#pragma once

#ifdef PT_ENABLE_D3D12

#include "gpu/D3D12GpuPathTracer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::gpu {

inline constexpr UINT kPathTraceRoot32BitValues =
    static_cast<UINT>(sizeof(PathTraceConstants) / sizeof(uint32_t));
inline constexpr uint32_t kGpuSdfStrideFloats = 16u;
/// Packed triangle record consumed by compute and DXR shaders: v0, edges,
/// material/double-sided flags, and per-vertex UVs.
inline constexpr uint32_t kGpuTriDataStrideFloats = 18u;
inline constexpr uint32_t kDxrStaticInstanceId = 0x00ffffffu;
inline constexpr uint32_t kInvalidTextureIndex = 0xffffffffu;
inline constexpr uint32_t kMaxTextureDimension = 512u;
/// Instance buffer stride in uint32_t words, including transform payload slots.
inline constexpr uint32_t kGpuInstanceStrideU32 = 24u;

inline D3D12_HEAP_PROPERTIES MakeHeapProperties(D3D12_HEAP_TYPE type) noexcept {
  D3D12_HEAP_PROPERTIES props{};
  props.Type = type;
  props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  props.CreationNodeMask = 1u;
  props.VisibleNodeMask = 1u;
  return props;
}

extern uint64_t g_d3d12RenderBatchCalls;
extern uint64_t g_d3d12SceneUploadCalls;

struct LoadedTextureRgba8 {
  uint32_t width = 1u;
  uint32_t height = 1u;
  std::vector<uint32_t> texels{0xffffffffu};
};

void LogInfo(const std::string& msg);
void LogError(const std::string& msg);
void LogDebug(const std::string& msg);
std::string WStringToUtf8(const wchar_t* src);
std::string FormatHr(HRESULT hr);
bool LoadTextureRgba8(std::string_view uri,
                      uint32_t maxDimension,
                      LoadedTextureRgba8& out,
                      std::string* error);

template <typename T>
std::string FormatFirstN(const T* values, size_t count, size_t max_values = 4u) {
  if (!values || count == 0u) {
    return "[]";
  }
  const size_t n = std::min(count, max_values);
  std::ostringstream ss;
  ss << "[";
  for (size_t i = 0; i < n; ++i) {
    if (i != 0u) {
      ss << ",";
    }
    ss << values[i];
  }
  if (count > n) {
    ss << ",...";
  }
  ss << "]";
  return ss.str();
}

uint32_t ParseEnvU32(const char* name, uint32_t fallback, uint32_t minValue, uint32_t maxValue);
std::string ReadEnvString(const char* name);
bool ParseEnvBool(const char* name, bool fallback);
bool D3D12VerboseLoggingEnabled();
std::string SelectDxrBuildMode();
uint32_t SelectBvhLeafSize();
uint32_t SelectBvhBucketCount();
std::string SelectBvhSplitMode();
std::string SelectShaderTraversalMode();
const char* ShaderTraversalDefine(const std::string& mode);
bool SelectPackedTriangleBuffer();
D3D12_COMMAND_LIST_TYPE SelectComputeCommandListType();
const char* CommandListTypeName(D3D12_COMMAND_LIST_TYPE type);
const char* BoolDefine(bool value);
D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS DxrBuildPreferenceFlags(const std::string& mode);
D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS AddDxrBuildFlags(
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS lhs,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS rhs);
uint32_t SelectRaysPerPixelPerDispatch(const vkpt::pathtracer::RenderSettings& settings);
uint32_t SelectReadbackInterval(const vkpt::pathtracer::RenderSettings& settings);
uint64_t SaturatingMulU64(uint64_t lhs, uint64_t rhs);
uint64_t EstimateLogicalRaysPerD3D12Sample(const vkpt::pathtracer::RenderSettings& settings,
                                           const vkpt::pathtracer::RTSceneData& scene,
                                           bool usingDxrDispatch);
D3D12_UNORDERED_ACCESS_VIEW_DESC MakeRawBufferUavDesc(UINT64 byteSize);
D3D12TemporalCameraState MakeTemporalCameraState(const PathTraceConstants& pc);
/// Writes the previous camera block used by temporal reprojection shaders.
void FillPreviousCameraConstants(PathTraceConstants& pc, const D3D12TemporalCameraState& state);
/// Creates a full-resource transition barrier for buffer resources.
D3D12_RESOURCE_BARRIER MakeTransitionBarrier(ID3D12Resource* resource,
                                             D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after);
uint32_t FloatBits(float value);
float UintBitsToFloat(uint32_t value);
D3D12_RAYTRACING_INSTANCE_DESC MakeDxrInstanceDesc(
    uint32_t instanceId,
    D3D12_GPU_VIRTUAL_ADDRESS blasAddress,
    const vkpt::pathtracer::Vec3& translation,
    vkpt::pathtracer::Quat4 rotation,
    const vkpt::pathtracer::Vec3& scale);
void StoreEmptyBvh(std::vector<float>& gpu_bvh);

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
