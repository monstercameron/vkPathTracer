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

enum class DebugViewChannel : std::uint8_t {
  Beauty,
  Geometry,
  Material,
  PathState,
  Acceleration,
  Sdf,
  Lighting,
  PostProcess,
  Validation,
};

struct DebugViewDescriptor {
  DebugViewId id = DebugViewId::Unknown;
  std::string view_id;
  std::string display_name;
  bool available = false;
  DebugBackendRequirement backend_requirement = DebugBackendRequirement::Any;
  std::string notes;
  bool declared = true;
  DebugViewChannel channel = DebugViewChannel::Beauty;
  bool accumulation_reset_required = false;
  std::string command_id;
  std::string output_buffer;
};

struct DebugViewCommandDescriptor {
  std::string command_id;
  DebugViewId debug_view = DebugViewId::Unknown;
  std::string view_id;
  std::string display_name;
  bool enabled = true;
  bool undoable = false;
  bool accumulation_reset_required = false;
  std::string command_payload_json;
};

const char* ToString(DebugViewId id);
const char* ToString(DebugBackendRequirement req);
const char* ToString(DebugViewChannel channel);
const std::vector<DebugViewDescriptor>& GetDebugViewRegistry();
bool IsDebugViewAvailable(DebugViewId id);
const DebugViewDescriptor* FindDebugView(std::string_view view_id);
std::vector<DebugViewCommandDescriptor> BuildDebugViewCommandDescriptors();
const DebugViewCommandDescriptor* FindDebugViewCommand(std::string_view command_id);
std::string MakeDebugViewCommandPayloadJson(const DebugViewDescriptor& descriptor);
std::string SerializeDebugViewRegistry();

}  // namespace vkpt::render
