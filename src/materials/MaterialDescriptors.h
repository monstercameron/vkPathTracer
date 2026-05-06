#pragma once
#include <array>
#include <cstdint>
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
  Wood,
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

enum class AlphaMode : std::uint8_t {
  Opaque,
  Mask,
  Blend,
};

struct MaterialTextureBinding {
  std::string slot;
  std::string texture_asset_urn;
  std::string sampler_id;
  std::string channel_semantic;
};

struct MaterialSamplerBinding {
  std::string sampler_id = "default_linear_repeat";
  std::string wrap_u = "repeat";
  std::string wrap_v = "repeat";
  std::string min_filter = "linear_mipmap_linear";
  std::string mag_filter = "linear";
  float anisotropy = 1.0f;
};

struct MaterialDesc {
  // Runtime-facing material payload; descriptor presets fill this before scene conversion packs RTMaterial.
  MaterialFamily family = MaterialFamily::Unknown;
  std::string id;
  std::string display_name;
  std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
  float roughness = 1.0f;
  float metallic = 0.0f;
  float ior = 1.5f;
  float transmission = 0.0f;
  std::array<float, 3> emission{0.0f, 0.0f, 0.0f};
  float emission_intensity = 0.0f;
  std::string normal_map;
  AlphaMode alpha_mode = AlphaMode::Opaque;
  float alpha_cutoff = 0.5f;
  float clearcoat = 0.0f;
  float sheen = 0.0f;
  float anisotropy = 0.0f;
  std::vector<MaterialTextureBinding> texture_bindings;
  std::vector<MaterialSamplerBinding> sampler_bindings;
  std::vector<std::string> compatibility_notes;
  std::string fallback_material_id = "diffuse";
  bool benchmark_approved = false;
};

struct MaterialValidationResult {
  bool valid = true;
  std::vector<std::string> warnings;
  std::string fallback_material_id = "diffuse";
};

class IMaterialRegistry {
 public:
  virtual ~IMaterialRegistry() = default;
  virtual bool register_family(MaterialDescriptor descriptor) = 0;
  virtual bool register_preset(MaterialDesc desc) = 0;
  [[nodiscard]] virtual MaterialValidationResult validate_material(const MaterialDesc& desc) const = 0;
  [[nodiscard]] virtual const MaterialDesc& resolve_fallback(std::string_view requested_id) const = 0;
  [[nodiscard]] virtual bool query_benchmark_approval(std::string_view material_id) const = 0;
  [[nodiscard]] virtual const MaterialDesc* find_preset(std::string_view material_id) const = 0;
  [[nodiscard]] virtual std::string dump_registry() const = 0;
};

class MaterialRegistry final : public IMaterialRegistry {
 public:
  // Owns both family descriptors and concrete presets so validation can select fallbacks consistently.
  MaterialRegistry();

  bool register_family(MaterialDescriptor descriptor) override;
  bool register_preset(MaterialDesc desc) override;
  [[nodiscard]] MaterialValidationResult validate_material(const MaterialDesc& desc) const override;
  [[nodiscard]] const MaterialDesc& resolve_fallback(std::string_view requested_id) const override;
  [[nodiscard]] bool query_benchmark_approval(std::string_view material_id) const override;
  [[nodiscard]] const MaterialDesc* find_preset(std::string_view material_id) const override;
  [[nodiscard]] std::string dump_registry() const override;

  [[nodiscard]] const std::vector<MaterialDescriptor>& families() const;
  [[nodiscard]] const std::vector<MaterialDesc>& presets() const;

 private:
  std::vector<MaterialDescriptor> m_families;
  std::vector<MaterialDesc> m_presets;
  std::string m_fallbackId = "diffuse";
};

const char* ToString(MaterialFamily family);
const char* ToString(ImplementationStatus status);
const char* ToString(AlphaMode mode);

// Returns all registered material descriptors across all packs.
const std::vector<MaterialDescriptor>& GetMaterialRegistry();

// Returns only materials with benchmark_approved == true.
std::vector<const MaterialDescriptor*> GetBenchmarkApprovedMaterials();

// Returns the fallback material (Diffuse/Lambert).
const MaterialDescriptor& GetFallbackMaterial();

// Look up a descriptor by id string. Returns nullptr if not found.
const MaterialDescriptor* FindMaterial(std::string_view id);

std::string SerializeMaterialRegistry();
std::string SerializeMaterialDesc(const MaterialDesc& desc);
std::string HashMaterialDesc(const MaterialDesc& desc);
MaterialDesc MakeMaterialDescFromDescriptor(const MaterialDescriptor& descriptor);
IMaterialRegistry& GetDefaultMaterialRegistry();

}  // namespace vkpt::materials
