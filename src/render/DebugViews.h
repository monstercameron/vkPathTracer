#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::render {

/// Stable ids for debug outputs that may be backed by CPU, GPU, or validation paths.
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

/// Backend capability required before a debug view can be considered runnable.
enum class DebugBackendRequirement : std::uint8_t {
  Any,       // works on any backend
  CpuOnly,   // requires CPU path
  GpuOnly,   // requires GPU compute
  RtOnly,    // requires hardware RT
};

/// UI/diagnostic grouping for debug views.
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

/// Registry entry describing one debug output and its command routing metadata.
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

/// Command descriptor exported to UI/action layers for selecting a debug view.
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
/// Return the process-wide debug-view registry.
const std::vector<DebugViewDescriptor>& GetDebugViewRegistry();
/// Return true when the view is declared and currently implemented.
bool IsDebugViewAvailable(DebugViewId id);
/// Find a debug view by its stable string id, such as "normals".
const DebugViewDescriptor* FindDebugView(std::string_view view_id);
/// Build command descriptors from the registry for menu/action binding.
std::vector<DebugViewCommandDescriptor> BuildDebugViewCommandDescriptors();
/// Find a generated command descriptor by command id.
const DebugViewCommandDescriptor* FindDebugViewCommand(std::string_view command_id);
/// Serialize the command payload used by the app command bus.
std::string MakeDebugViewCommandPayloadJson(const DebugViewDescriptor& descriptor);
/// Serialize the debug-view registry as compact JSON for diagnostics.
std::string SerializeDebugViewRegistry();

}  // namespace vkpt::render
