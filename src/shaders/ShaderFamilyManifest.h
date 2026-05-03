#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::shaders {

enum class ShaderFamily : std::uint8_t {
  RayGeneration,
  CameraSampling,
  BvhTraversal,
  SdfIntersection,
  TriangleIntersection,
  MaterialEvaluation,
  BsdfSampling,
  LightSampling,
  ShadowRayTesting,
  FilmAccumulation,
  Resolve,
  Denoise,
  DebugVisualization,
  EditorOverlay,
  Unknown,
};

enum class ShaderStage : std::uint8_t {
  Compute,
  RayGen,
  ClosestHit,
  AnyHit,
  Miss,
  Callable,
  Vertex,
  Fragment,
};

struct ShaderFamilyDescriptor {
  ShaderFamily family = ShaderFamily::Unknown;
  std::string id;
  std::string display_name;
  ShaderStage primary_stage = ShaderStage::Compute;
  bool implemented = false;
  std::string notes;
};

const char* ToString(ShaderFamily family);
const char* ToString(ShaderStage stage);
const std::vector<ShaderFamilyDescriptor>& GetShaderFamilyManifest();
std::string SerializeShaderFamilyManifest();

}  // namespace vkpt::shaders
