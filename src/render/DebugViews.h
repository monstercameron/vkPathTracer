#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::render {

enum class DebugViewId : std::uint8_t {
  Beauty,
  Albedo,
  Normals,
  Depth,
  WorldPosition,
  MaterialId,
  ObjectId,
  UV,
  Roughness,
  Metallic,
  Emission,
  Throughput,
  SampleCount,
  Variance,
  BvhDepth,
  SdfDistance,
  LightContribution,
  DenoisedOutput,
  DifferenceHeatmap,
  NanInfHighlight,
  Unknown,
};

enum class DebugBackendRequirement : std::uint8_t {
  Any,       // works on any backend
  CpuOnly,   // requires CPU path
  GpuOnly,   // requires GPU compute
  RtOnly,    // requires hardware RT
};

struct DebugViewDescriptor {
  DebugViewId id = DebugViewId::Unknown;
  std::string view_id;
  std::string display_name;
  bool available = false;
  DebugBackendRequirement backend_requirement = DebugBackendRequirement::Any;
  std::string notes;
};

const char* ToString(DebugViewId id);
const char* ToString(DebugBackendRequirement req);
const std::vector<DebugViewDescriptor>& GetDebugViewRegistry();
bool IsDebugViewAvailable(DebugViewId id);
const DebugViewDescriptor* FindDebugView(std::string_view view_id);
std::string SerializeDebugViewRegistry();

}  // namespace vkpt::render
