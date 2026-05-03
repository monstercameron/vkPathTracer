#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::materials {

enum class MaterialFamily : std::uint8_t {
  // Pack 1 — benchmark core
  Diffuse,
  Emissive,
  Mirror,
  Specular,
  Glossy,
  GgxRoughConductor,
  GgxRoughDielectric,
  MetallicPbr,
  DielectricGlass,
  Clearcoat,
  NormalMappedPbr,
  AlphaMask,
  EnvironmentEmissive,
  // Pack 2 — experimental
  Velvet,
  Charcoal,
  Rubber,
  XRay,
  SubsurfaceApprox,
  SpectralGlassApprox,
  ProceduralMaterial,
  SdfFractalMaterial,
  VolumetricShafts,
  CausticsInspiredResponse,
  ThinFilmIridescent,
  Retroreflector,
  VoronoiCracks,
  DiffractionGrating,
  AnisotropicGgx,
  BlackbodyEmission,
  FirePlasma,
  ToonSurface,
  BokehMotionBlurStress,
  // Pack 3 — backlog
  Plastic,
  FabricCloth,
  PorcelainCeramic,
  Paint,
  CarPaint,
  WetSurface,
  FrostedGlass,
  DirtyGlass,
  CorrosionOxidation,
  Stone,
  Concrete,
  Plaster,
  WaterFluidSurface,
  IceCrystal,
  StylizedDiffuse,
  // Advanced pack — deferred
  Skin,
  Wax,
  MarbleScattering,
  HairFurLobes,
  EnergyConservingLayered,
  VolumetricMedium,
  Mud,
  Sand,
  TerraEarth,
  BrushedMetal,
  GroundMetal,
  FireSparkleEmission,
  LightEmittingTextile,
  HolographicCoating,
  Paper,
  Cardboard,
  Resin,
  Epoxy,
  Gemstone,
  Smoke,
  ChromaticDust,
  PearlLustre,
  FrostedAcrylic,
  TranslucentPolymer,
  RustProgression,
  Unknown,
};

enum class ImplementationStatus : std::uint8_t {
  Implemented,    // ready for benchmark
  Experimental,   // descriptor exists, partial/prototype implementation
  Backlog,        // descriptor exists, not yet implemented
  Deferred,       // planned for future milestone
};

struct MaterialDescriptor {
  MaterialFamily family = MaterialFamily::Unknown;
  std::string id;
  std::string display_name;
  ImplementationStatus status = ImplementationStatus::Deferred;
  bool benchmark_approved = false;
  std::string notes;
};

const char* ToString(MaterialFamily family);
const char* ToString(ImplementationStatus status);

// Returns all registered material descriptors across all packs.
const std::vector<MaterialDescriptor>& GetMaterialRegistry();

// Returns only materials with benchmark_approved == true.
std::vector<const MaterialDescriptor*> GetBenchmarkApprovedMaterials();

// Returns the fallback material (Diffuse/Lambert).
const MaterialDescriptor& GetFallbackMaterial();

// Look up a descriptor by id string. Returns nullptr if not found.
const MaterialDescriptor* FindMaterial(std::string_view id);

std::string SerializeMaterialRegistry();

}  // namespace vkpt::materials
