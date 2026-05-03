#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::shaders {

enum class SdfFeature : std::uint8_t {
  Sphere,
  Box,
  RoundedBox,
  Cylinder,
  Cone,
  Frustum,
  Capsule,
  Ellipsoid,
  Torus,
  Disk,
  Plane,
  Triangle,
  Wedge,
  Prism,
  Metaballs,
  MandelbulbFractal,
  CsgUnion,
  CsgIntersection,
  CsgSubtraction,
  SmoothBlend,
  NoiseDeformation,
  Unknown,
};

struct SdfFeatureDescriptor {
  SdfFeature feature = SdfFeature::Unknown;
  std::string id;
  std::string display_name;
  bool implemented = false;
  std::string notes;
};

const char* ToString(SdfFeature feature);
const std::vector<SdfFeatureDescriptor>& GetSdfFeatureInventory();
std::string SerializeSdfFeatureInventory();

}  // namespace vkpt::shaders
