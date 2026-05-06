#include "materials/MaterialDescriptors.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace vkpt::materials {

namespace {

std::string EscapeJson(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

bool InRange01(float value) {
  return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

bool IsFinitePositive(float value) {
  return std::isfinite(value) && value > 0.0f;
}

std::uint64_t Fnv1a64(std::string_view text) {
  constexpr std::uint64_t kOffset = 1469598103934665603ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffset;
  for (const unsigned char c : text) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= kPrime;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << value;
  return out.str();
}

void AppendFloat(std::ostringstream& out, float value) {
  out << std::fixed << std::setprecision(6) << value;
}

}  // namespace

const char* ToString(MaterialFamily family) {
  switch (family) {
    case MaterialFamily::Diffuse:                return "Diffuse";
    case MaterialFamily::Emissive:               return "Emissive";
    case MaterialFamily::Mirror:                 return "Mirror";
    case MaterialFamily::Specular:               return "Specular";
    case MaterialFamily::Glossy:                 return "Glossy";
    case MaterialFamily::GgxRoughConductor:      return "GgxRoughConductor";
    case MaterialFamily::GgxRoughDielectric:     return "GgxRoughDielectric";
    case MaterialFamily::MetallicPbr:            return "MetallicPbr";
    case MaterialFamily::DielectricGlass:        return "DielectricGlass";
    case MaterialFamily::Clearcoat:              return "Clearcoat";
    case MaterialFamily::NormalMappedPbr:        return "NormalMappedPbr";
    case MaterialFamily::AlphaMask:              return "AlphaMask";
    case MaterialFamily::EnvironmentEmissive:    return "EnvironmentEmissive";
    case MaterialFamily::Velvet:                 return "Velvet";
    case MaterialFamily::Charcoal:               return "Charcoal";
    case MaterialFamily::Rubber:                 return "Rubber";
    case MaterialFamily::XRay:                   return "XRay";
    case MaterialFamily::SubsurfaceApprox:       return "SubsurfaceApprox";
    case MaterialFamily::SpectralGlassApprox:    return "SpectralGlassApprox";
    case MaterialFamily::ProceduralMaterial:     return "ProceduralMaterial";
    case MaterialFamily::SdfFractalMaterial:     return "SdfFractalMaterial";
    case MaterialFamily::VolumetricShafts:       return "VolumetricShafts";
    case MaterialFamily::CausticsInspiredResponse: return "CausticsInspiredResponse";
    case MaterialFamily::ThinFilmIridescent:     return "ThinFilmIridescent";
    case MaterialFamily::Retroreflector:         return "Retroreflector";
    case MaterialFamily::VoronoiCracks:          return "VoronoiCracks";
    case MaterialFamily::DiffractionGrating:     return "DiffractionGrating";
    case MaterialFamily::AnisotropicGgx:         return "AnisotropicGgx";
    case MaterialFamily::BlackbodyEmission:      return "BlackbodyEmission";
    case MaterialFamily::FirePlasma:             return "FirePlasma";
    case MaterialFamily::ToonSurface:            return "ToonSurface";
    case MaterialFamily::BokehMotionBlurStress:  return "BokehMotionBlurStress";
    case MaterialFamily::Plastic:                return "Plastic";
    case MaterialFamily::FabricCloth:            return "FabricCloth";
    case MaterialFamily::PorcelainCeramic:       return "PorcelainCeramic";
    case MaterialFamily::Paint:                  return "Paint";
    case MaterialFamily::CarPaint:               return "CarPaint";
    case MaterialFamily::WetSurface:             return "WetSurface";
    case MaterialFamily::FrostedGlass:           return "FrostedGlass";
    case MaterialFamily::DirtyGlass:             return "DirtyGlass";
    case MaterialFamily::CorrosionOxidation:     return "CorrosionOxidation";
    case MaterialFamily::Stone:                  return "Stone";
    case MaterialFamily::Concrete:               return "Concrete";
    case MaterialFamily::Plaster:                return "Plaster";
    case MaterialFamily::WaterFluidSurface:      return "WaterFluidSurface";
    case MaterialFamily::IceCrystal:             return "IceCrystal";
    case MaterialFamily::StylizedDiffuse:        return "StylizedDiffuse";
    case MaterialFamily::Skin:                   return "Skin";
    case MaterialFamily::Wax:                    return "Wax";
    case MaterialFamily::MarbleScattering:       return "MarbleScattering";
    case MaterialFamily::HairFurLobes:           return "HairFurLobes";
    case MaterialFamily::EnergyConservingLayered: return "EnergyConservingLayered";
    case MaterialFamily::VolumetricMedium:       return "VolumetricMedium";
    case MaterialFamily::Mud:                    return "Mud";
    case MaterialFamily::Sand:                   return "Sand";
    case MaterialFamily::TerraEarth:             return "TerraEarth";
    case MaterialFamily::BrushedMetal:           return "BrushedMetal";
    case MaterialFamily::GroundMetal:            return "GroundMetal";
    case MaterialFamily::FireSparkleEmission:    return "FireSparkleEmission";
    case MaterialFamily::LightEmittingTextile:   return "LightEmittingTextile";
    case MaterialFamily::HolographicCoating:     return "HolographicCoating";
    case MaterialFamily::Paper:                  return "Paper";
    case MaterialFamily::Cardboard:              return "Cardboard";
    case MaterialFamily::Resin:                  return "Resin";
    case MaterialFamily::Epoxy:                  return "Epoxy";
    case MaterialFamily::Gemstone:               return "Gemstone";
    case MaterialFamily::Smoke:                  return "Smoke";
    case MaterialFamily::ChromaticDust:          return "ChromaticDust";
    case MaterialFamily::PearlLustre:            return "PearlLustre";
    case MaterialFamily::FrostedAcrylic:         return "FrostedAcrylic";
    case MaterialFamily::TranslucentPolymer:     return "TranslucentPolymer";
    case MaterialFamily::RustProgression:        return "RustProgression";
    default:                                     return "Unknown";
  }
}

const char* ToString(ImplementationStatus status) {
  switch (status) {
    case ImplementationStatus::Implemented:  return "implemented";
    case ImplementationStatus::Experimental: return "experimental";
    case ImplementationStatus::Backlog:      return "backlog";
    case ImplementationStatus::Deferred:     return "deferred";
    default:                                 return "unknown";
  }
}

const char* ToString(AlphaMode mode) {
  switch (mode) {
    case AlphaMode::Opaque: return "opaque";
    case AlphaMode::Mask:   return "mask";
    case AlphaMode::Blend:  return "blend";
    default:                return "opaque";
  }
}

const std::vector<MaterialDescriptor>& GetMaterialRegistry() {
  static const std::vector<MaterialDescriptor> s_registry = [] {
    std::vector<MaterialDescriptor> r;

    // Material packs are append-only so serialized registry hashes stay stable across releases.
    // Pack 1: benchmark core (13 materials)
    r.push_back({MaterialFamily::Diffuse,             "diffuse",             "Diffuse (Lambert)",         ImplementationStatus::Implemented, true,  "Lambertian BRDF"});
    r.push_back({MaterialFamily::Emissive,            "emissive",            "Emissive",                  ImplementationStatus::Implemented, true,  "Area light emission"});
    r.push_back({MaterialFamily::Mirror,              "mirror",              "Mirror",                    ImplementationStatus::Implemented, true,  "Perfect specular reflection"});
    r.push_back({MaterialFamily::Specular,            "specular",            "Specular",                  ImplementationStatus::Implemented, true,  "Blinn-Phong specular"});
    r.push_back({MaterialFamily::Glossy,              "glossy",              "Glossy",                    ImplementationStatus::Implemented, true,  "GGX microfacet"});
    r.push_back({MaterialFamily::GgxRoughConductor,   "ggx_rough_conductor", "GGX Rough Conductor",       ImplementationStatus::Implemented, true,  "Metal with GGX NDF"});
    r.push_back({MaterialFamily::GgxRoughDielectric,  "ggx_rough_dielectric","GGX Rough Dielectric",      ImplementationStatus::Implemented, true,  "Glass-like surface with GGX"});
    r.push_back({MaterialFamily::MetallicPbr,         "metallic_pbr",        "Metallic PBR",              ImplementationStatus::Implemented, true,  "UE4-style metallic workflow"});
    r.push_back({MaterialFamily::DielectricGlass,     "dielectric_glass",    "Dielectric Glass",          ImplementationStatus::Implemented, true,  "Fresnel refraction/reflection"});
    r.push_back({MaterialFamily::Clearcoat,           "clearcoat",           "Clearcoat",                 ImplementationStatus::Implemented, false, "Two-layer coat over diffuse base"});
    r.push_back({MaterialFamily::NormalMappedPbr,     "normal_mapped_pbr",   "Normal Mapped PBR",         ImplementationStatus::Implemented, false, "PBR + tangent-space normal map"});
    r.push_back({MaterialFamily::AlphaMask,           "alpha_mask",          "Alpha Mask",                ImplementationStatus::Implemented, false, "Binary alpha cutout"});
    r.push_back({MaterialFamily::EnvironmentEmissive, "environment_emissive","Environment Emissive",      ImplementationStatus::Implemented, false, "Env-map as emitter"});

    // Pack 2: implemented extended surfaces (19 materials)
    r.push_back({MaterialFamily::Velvet,                    "velvet",                    "Velvet",                   ImplementationStatus::Implemented, false, "Retro-reflective cloth sheen"});
    r.push_back({MaterialFamily::Charcoal,                  "charcoal",                  "Charcoal",                 ImplementationStatus::Implemented, false, "Dark absorptive diffuse"});
    r.push_back({MaterialFamily::Rubber,                    "rubber",                    "Rubber",                   ImplementationStatus::Implemented, false, "Low-IOR smooth dielectric"});
    r.push_back({MaterialFamily::XRay,                      "xray",                      "X-Ray",                    ImplementationStatus::Implemented, false, "Silhouette-emphasizing shader"});
    r.push_back({MaterialFamily::SubsurfaceApprox,          "subsurface_approx",         "Subsurface Approx",        ImplementationStatus::Implemented, false, "Dipole SSS approximation"});
    r.push_back({MaterialFamily::SpectralGlassApprox,       "spectral_glass_approx",     "Spectral Glass Approx",    ImplementationStatus::Implemented, false, "RGB dispersion glass"});
    r.push_back({MaterialFamily::ProceduralMaterial,        "procedural_material",       "Procedural Material",      ImplementationStatus::Implemented, false, "SDF-driven procedural patterns"});
    r.push_back({MaterialFamily::SdfFractalMaterial,        "sdf_fractal_material",      "SDF Fractal Material",     ImplementationStatus::Implemented, false, "Mandelbulb-shaded surface"});
    r.push_back({MaterialFamily::VolumetricShafts,          "volumetric_shafts",         "Volumetric Shafts",        ImplementationStatus::Implemented, false, "Participating media light shafts"});
    r.push_back({MaterialFamily::CausticsInspiredResponse,  "caustics_inspired_response","Caustics-Inspired Response",ImplementationStatus::Implemented, false, "Approximate caustic highlight"});
    r.push_back({MaterialFamily::ThinFilmIridescent,        "thin_film_iridescent",      "Thin Film Iridescent",     ImplementationStatus::Implemented, false, "Oil-slick thin film interference"});
    r.push_back({MaterialFamily::Retroreflector,            "retroreflector",            "Retroreflector",           ImplementationStatus::Implemented, false, "Corner-cube-like return shading"});
    r.push_back({MaterialFamily::VoronoiCracks,             "voronoi_cracks",            "Voronoi Cracks",           ImplementationStatus::Implemented, false, "Procedural crack pattern"});
    r.push_back({MaterialFamily::DiffractionGrating,        "diffraction_grating",       "Diffraction Grating",      ImplementationStatus::Implemented, false, "CD/DVD spectral diffraction"});
    r.push_back({MaterialFamily::AnisotropicGgx,            "anisotropic_ggx",           "Anisotropic GGX",          ImplementationStatus::Implemented, false, "Brushed-metal anisotropic NDF"});
    r.push_back({MaterialFamily::BlackbodyEmission,         "blackbody_emission",        "Blackbody Emission",       ImplementationStatus::Implemented, false, "Temperature-driven emission"});
    r.push_back({MaterialFamily::FirePlasma,                "fire_plasma",               "Fire/Plasma",              ImplementationStatus::Implemented, false, "Animated volumetric fire"});
    r.push_back({MaterialFamily::ToonSurface,               "toon_surface",              "Toon Surface",             ImplementationStatus::Implemented, false, "Cel-shaded step ramp"});
    r.push_back({MaterialFamily::BokehMotionBlurStress,     "bokeh_motion_blur_stress",  "Bokeh/Motion Blur Stress", ImplementationStatus::Implemented, false, "High-specular bokeh emitter"});

    // Pack 3: implemented production surfaces (15 materials)
    r.push_back({MaterialFamily::Plastic,           "plastic",            "Plastic",             ImplementationStatus::Implemented, false, "Layered dielectric + diffuse"});
    r.push_back({MaterialFamily::FabricCloth,       "fabric_cloth",       "Fabric Cloth",        ImplementationStatus::Implemented, false, "Woven microstructure"});
    r.push_back({MaterialFamily::PorcelainCeramic,  "porcelain_ceramic",  "Porcelain/Ceramic",   ImplementationStatus::Implemented, false, "Smooth matte + specular glaze"});
    r.push_back({MaterialFamily::Paint,             "paint",              "Paint",               ImplementationStatus::Implemented, false, "Lambertian + thin clearcoat"});
    r.push_back({MaterialFamily::CarPaint,          "car_paint",          "Car Paint",           ImplementationStatus::Implemented, false, "Metallic flake car paint"});
    r.push_back({MaterialFamily::WetSurface,        "wet_surface",        "Wet Surface",         ImplementationStatus::Implemented, false, "Extra specular from water film"});
    r.push_back({MaterialFamily::FrostedGlass,      "frosted_glass",      "Frosted Glass",       ImplementationStatus::Implemented, false, "Rough dielectric"});
    r.push_back({MaterialFamily::DirtyGlass,        "dirty_glass",        "Dirty Glass",         ImplementationStatus::Implemented, false, "Smudge layer over glass"});
    r.push_back({MaterialFamily::CorrosionOxidation,"corrosion_oxidation","Corrosion/Oxidation", ImplementationStatus::Implemented, false, "Rust/patina procedural blend"});
    r.push_back({MaterialFamily::Stone,             "stone",              "Stone",               ImplementationStatus::Implemented, false, "Granular rough diffuse"});
    r.push_back({MaterialFamily::Concrete,          "concrete",           "Concrete",            ImplementationStatus::Implemented, false, "Grey diffuse with cracks"});
    r.push_back({MaterialFamily::Plaster,           "plaster",            "Plaster",             ImplementationStatus::Implemented, false, "Matte whitish diffuse"});
    r.push_back({MaterialFamily::WaterFluidSurface, "water_fluid_surface","Water Fluid Surface",  ImplementationStatus::Implemented, false, "Animated dielectric surface"});
    r.push_back({MaterialFamily::IceCrystal,        "ice_crystal",        "Ice Crystal",         ImplementationStatus::Implemented, false, "Birefringent translucent solid"});
    r.push_back({MaterialFamily::StylizedDiffuse,   "stylized_diffuse",   "Stylized Diffuse",    ImplementationStatus::Implemented, false, "Artist-driven BRDF control"});

    // Advanced pack: implemented specialized surfaces (25 materials)
    r.push_back({MaterialFamily::Skin,                    "skin",                    "Skin",                    ImplementationStatus::Implemented, false, "Multi-layer SSS skin"});
    r.push_back({MaterialFamily::Wax,                     "wax",                    "Wax",                     ImplementationStatus::Implemented, false, "Translucent wax SSS"});
    r.push_back({MaterialFamily::MarbleScattering,        "marble_scattering",       "Marble Scattering",       ImplementationStatus::Implemented, false, "Veined stone with SSS"});
    r.push_back({MaterialFamily::HairFurLobes,            "hair_fur_lobes",          "Hair/Fur Lobes",          ImplementationStatus::Implemented, false, "Marschner hair BCSDF"});
    r.push_back({MaterialFamily::EnergyConservingLayered, "energy_conserving_layered","Energy-Conserving Layered",ImplementationStatus::Implemented, false, "Multi-layer energy compensation"});
    r.push_back({MaterialFamily::VolumetricMedium,        "volumetric_medium",       "Volumetric Medium",       ImplementationStatus::Implemented, false, "Heterogeneous volume"});
    r.push_back({MaterialFamily::Mud,                     "mud",                     "Mud",                     ImplementationStatus::Implemented, false, "Wet diffuse clay"});
    r.push_back({MaterialFamily::Sand,                    "sand",                    "Sand",                    ImplementationStatus::Implemented, false, "Granular micro-facet"});
    r.push_back({MaterialFamily::TerraEarth,              "terra_earth",             "Terra/Earth",             ImplementationStatus::Implemented, false, "Mixed soil material"});
    r.push_back({MaterialFamily::BrushedMetal,            "brushed_metal",           "Brushed Metal",           ImplementationStatus::Implemented, false, "Anisotropic conductor"});
    r.push_back({MaterialFamily::GroundMetal,             "ground_metal",            "Ground Metal",            ImplementationStatus::Implemented, false, "Rough isotropic conductor"});
    r.push_back({MaterialFamily::FireSparkleEmission,     "fire_sparkle_emission",   "Fire Sparkle Emission",   ImplementationStatus::Implemented, false, "Stochastic emissive particles"});
    r.push_back({MaterialFamily::LightEmittingTextile,    "light_emitting_textile",  "Light-Emitting Textile",  ImplementationStatus::Implemented, false, "LED fiber cloth"});
    r.push_back({MaterialFamily::HolographicCoating,      "holographic_coating",     "Holographic Coating",     ImplementationStatus::Implemented, false, "Spectral grating film"});
    r.push_back({MaterialFamily::Paper,                   "paper",                   "Paper",                   ImplementationStatus::Implemented, false, "Translucent cellulose"});
    r.push_back({MaterialFamily::Cardboard,               "cardboard",               "Cardboard",               ImplementationStatus::Implemented, false, "Rough opaque layered paper"});
    r.push_back({MaterialFamily::Resin,                   "resin",                   "Resin",                   ImplementationStatus::Implemented, false, "Clear solid dielectric"});
    r.push_back({MaterialFamily::Epoxy,                   "epoxy",                   "Epoxy",                   ImplementationStatus::Implemented, false, "Amber-tinted solid resin"});
    r.push_back({MaterialFamily::Gemstone,                "gemstone",                "Gemstone",                ImplementationStatus::Implemented, false, "High-IOR spectral crystal"});
    r.push_back({MaterialFamily::Smoke,                   "smoke",                   "Smoke",                   ImplementationStatus::Implemented, false, "Isotropic scattering volume"});
    r.push_back({MaterialFamily::ChromaticDust,           "chromatic_dust",          "Chromatic Dust",          ImplementationStatus::Implemented, false, "Coloured particle cloud"});
    r.push_back({MaterialFamily::PearlLustre,             "pearl_lustre",            "Pearl Lustre",            ImplementationStatus::Implemented, false, "Multi-layer iridescent coating"});
    r.push_back({MaterialFamily::FrostedAcrylic,          "frosted_acrylic",         "Frosted Acrylic",         ImplementationStatus::Implemented, false, "Rough translucent polymer"});
    r.push_back({MaterialFamily::TranslucentPolymer,      "translucent_polymer",     "Translucent Polymer",     ImplementationStatus::Implemented, false, "Semi-transparent plastic"});
    r.push_back({MaterialFamily::RustProgression,         "rust_progression",        "Rust Progression",        ImplementationStatus::Implemented, false, "Animated corrosion stages"});

    return r;
  }();
  return s_registry;
}

std::vector<const MaterialDescriptor*> GetBenchmarkApprovedMaterials() {
  const auto& reg = GetMaterialRegistry();
  std::vector<const MaterialDescriptor*> out;
  for (const auto& d : reg) {
    if (d.benchmark_approved) out.push_back(&d);
  }
  return out;
}

const MaterialDescriptor& GetFallbackMaterial() {
  return GetMaterialRegistry()[0];  // Diffuse
}

const MaterialDescriptor* FindMaterial(std::string_view id) {
  for (const auto& d : GetMaterialRegistry()) {
    if (d.id == id) return &d;
  }
  return nullptr;
}

MaterialDesc MakeMaterialDescFromDescriptor(const MaterialDescriptor& descriptor) {
  MaterialDesc desc;
  desc.family = descriptor.family;
  desc.id = descriptor.id;
  desc.display_name = descriptor.display_name;
  desc.benchmark_approved = descriptor.benchmark_approved;
  desc.compatibility_notes.push_back(descriptor.notes);

  // Collapse family descriptors into renderer-safe scalar slots and texture/sampler bindings.
  switch (descriptor.family) {
    case MaterialFamily::Diffuse:
      desc.base_color = {0.75f, 0.75f, 0.75f, 1.0f};
      desc.roughness = 1.0f;
      break;
    case MaterialFamily::Emissive:
    case MaterialFamily::EnvironmentEmissive:
    case MaterialFamily::BlackbodyEmission:
    case MaterialFamily::FirePlasma:
    case MaterialFamily::FireSparkleEmission:
    case MaterialFamily::LightEmittingTextile:
      desc.base_color = {1.0f, 1.0f, 1.0f, 1.0f};
      desc.emission = {1.0f, 1.0f, 1.0f};
      desc.emission_intensity = 1.0f;
      break;
    case MaterialFamily::Mirror:
      desc.roughness = 0.0f;
      desc.metallic = 1.0f;
      break;
    case MaterialFamily::Specular:
    case MaterialFamily::Glossy:
    case MaterialFamily::NormalMappedPbr:
    case MaterialFamily::Plastic:
    case MaterialFamily::Rubber:
      desc.roughness = 0.25f;
      desc.ior = descriptor.family == MaterialFamily::Rubber ? 1.35f : 1.5f;
      break;
    case MaterialFamily::GgxRoughConductor:
    case MaterialFamily::MetallicPbr:
    case MaterialFamily::AnisotropicGgx:
    case MaterialFamily::BrushedMetal:
    case MaterialFamily::GroundMetal:
      desc.metallic = 1.0f;
      desc.roughness = 0.35f;
      break;
    case MaterialFamily::GgxRoughDielectric:
    case MaterialFamily::DielectricGlass:
    case MaterialFamily::SpectralGlassApprox:
    case MaterialFamily::FrostedGlass:
    case MaterialFamily::DirtyGlass:
    case MaterialFamily::WaterFluidSurface:
    case MaterialFamily::IceCrystal:
    case MaterialFamily::Resin:
    case MaterialFamily::Epoxy:
    case MaterialFamily::Gemstone:
    case MaterialFamily::FrostedAcrylic:
    case MaterialFamily::TranslucentPolymer:
      desc.ior = 1.5f;
      desc.transmission = 1.0f;
      desc.roughness = (descriptor.family == MaterialFamily::FrostedGlass ||
                        descriptor.family == MaterialFamily::FrostedAcrylic) ? 0.5f : 0.02f;
      break;
    case MaterialFamily::Clearcoat:
    case MaterialFamily::Paint:
    case MaterialFamily::PorcelainCeramic:
    case MaterialFamily::WetSurface:
    case MaterialFamily::CarPaint:
    case MaterialFamily::EnergyConservingLayered:
    case MaterialFamily::ThinFilmIridescent:
    case MaterialFamily::DiffractionGrating:
    case MaterialFamily::HolographicCoating:
    case MaterialFamily::Retroreflector:
    case MaterialFamily::CausticsInspiredResponse:
      desc.clearcoat = 1.0f;
      desc.roughness = 0.2f;
      break;
    case MaterialFamily::AlphaMask:
      desc.alpha_mode = AlphaMode::Mask;
      desc.alpha_cutoff = 0.5f;
      break;
    case MaterialFamily::Velvet:
    case MaterialFamily::FabricCloth:
    case MaterialFamily::HairFurLobes:
    case MaterialFamily::PearlLustre:
      desc.sheen = 0.6f;
      break;
    case MaterialFamily::SubsurfaceApprox:
    case MaterialFamily::Skin:
    case MaterialFamily::Wax:
    case MaterialFamily::MarbleScattering:
    case MaterialFamily::Paper:
      desc.roughness = 0.62f;
      desc.sheen = 0.2f;
      break;
    case MaterialFamily::VolumetricShafts:
    case MaterialFamily::VolumetricMedium:
    case MaterialFamily::Smoke:
    case MaterialFamily::ChromaticDust:
      desc.roughness = 1.0f;
      desc.base_color[3] = 0.45f;
      desc.transmission = 0.12f;
      break;
    default:
      break;
  }

  if (descriptor.family == MaterialFamily::NormalMappedPbr) {
    desc.normal_map = "normal";
    desc.texture_bindings.push_back({"normal", "", "default_linear_repeat", "normal"});
  }

  if (desc.sampler_bindings.empty()) {
    desc.sampler_bindings.push_back({});
  }
  return desc;
}

std::string SerializeMaterialDesc(const MaterialDesc& desc) {
  // Keep field order fixed because HashMaterialDesc uses this JSON as the canonical descriptor stream.
  std::ostringstream out;
  out << "{";
  out << "\"id\":\"" << EscapeJson(desc.id) << "\",";
  out << "\"display_name\":\"" << EscapeJson(desc.display_name) << "\",";
  out << "\"family\":\"" << ToString(desc.family) << "\",";
  out << "\"base_color\":[";
  for (std::size_t i = 0; i < desc.base_color.size(); ++i) {
    if (i > 0) out << ",";
    AppendFloat(out, desc.base_color[i]);
  }
  out << "],\"roughness\":";
  AppendFloat(out, desc.roughness);
  out << ",\"metallic\":";
  AppendFloat(out, desc.metallic);
  out << ",\"ior\":";
  AppendFloat(out, desc.ior);
  out << ",\"transmission\":";
  AppendFloat(out, desc.transmission);
  out << ",\"emission\":[";
  for (std::size_t i = 0; i < desc.emission.size(); ++i) {
    if (i > 0) out << ",";
    AppendFloat(out, desc.emission[i]);
  }
  out << "],\"emission_intensity\":";
  AppendFloat(out, desc.emission_intensity);
  out << ",\"normal_map\":\"" << EscapeJson(desc.normal_map) << "\",";
  out << "\"alpha_mode\":\"" << ToString(desc.alpha_mode) << "\",";
  out << "\"alpha_cutoff\":";
  AppendFloat(out, desc.alpha_cutoff);
  out << ",\"clearcoat\":";
  AppendFloat(out, desc.clearcoat);
  out << ",\"sheen\":";
  AppendFloat(out, desc.sheen);
  out << ",\"anisotropy\":";
  AppendFloat(out, desc.anisotropy);
  out << ",\"texture_bindings\":[";
  for (std::size_t i = 0; i < desc.texture_bindings.size(); ++i) {
    const auto& binding = desc.texture_bindings[i];
    if (i > 0) out << ",";
    out << "{";
    out << "\"slot\":\"" << EscapeJson(binding.slot) << "\",";
    out << "\"texture_asset_urn\":\"" << EscapeJson(binding.texture_asset_urn) << "\",";
    out << "\"sampler_id\":\"" << EscapeJson(binding.sampler_id) << "\",";
    out << "\"channel_semantic\":\"" << EscapeJson(binding.channel_semantic) << "\"";
    out << "}";
  }
  out << "],\"sampler_bindings\":[";
  for (std::size_t i = 0; i < desc.sampler_bindings.size(); ++i) {
    const auto& sampler = desc.sampler_bindings[i];
    if (i > 0) out << ",";
    out << "{";
    out << "\"sampler_id\":\"" << EscapeJson(sampler.sampler_id) << "\",";
    out << "\"wrap_u\":\"" << EscapeJson(sampler.wrap_u) << "\",";
    out << "\"wrap_v\":\"" << EscapeJson(sampler.wrap_v) << "\",";
    out << "\"min_filter\":\"" << EscapeJson(sampler.min_filter) << "\",";
    out << "\"mag_filter\":\"" << EscapeJson(sampler.mag_filter) << "\",";
    out << "\"anisotropy\":";
    AppendFloat(out, sampler.anisotropy);
    out << "}";
  }
  out << "],\"compatibility_notes\":[";
  for (std::size_t i = 0; i < desc.compatibility_notes.size(); ++i) {
    if (i > 0) out << ",";
    out << "\"" << EscapeJson(desc.compatibility_notes[i]) << "\"";
  }
  out << "],\"fallback_material_id\":\"" << EscapeJson(desc.fallback_material_id) << "\",";
  out << "\"benchmark_approved\":" << (desc.benchmark_approved ? "true" : "false");
  out << "}";
  return out.str();
}

std::string HashMaterialDesc(const MaterialDesc& desc) {
  return Hex64(Fnv1a64(SerializeMaterialDesc(desc)));
}

MaterialRegistry::MaterialRegistry() {
  m_families = GetMaterialRegistry();
  m_presets.reserve(m_families.size());
  for (const auto& descriptor : m_families) {
    m_presets.push_back(MakeMaterialDescFromDescriptor(descriptor));
  }
}

bool MaterialRegistry::register_family(MaterialDescriptor descriptor) {
  if (descriptor.id.empty() || descriptor.family == MaterialFamily::Unknown) {
    return false;
  }
  const auto it = std::find_if(m_families.begin(), m_families.end(),
                               [&](const MaterialDescriptor& existing) { return existing.id == descriptor.id; });
  if (it != m_families.end()) {
    return false;
  }
  m_families.push_back(std::move(descriptor));
  return true;
}

bool MaterialRegistry::register_preset(MaterialDesc desc) {
  if (desc.id.empty() || desc.family == MaterialFamily::Unknown) {
    return false;
  }
  const auto it = std::find_if(m_presets.begin(), m_presets.end(),
                               [&](const MaterialDesc& existing) { return existing.id == desc.id; });
  if (it != m_presets.end()) {
    return false;
  }
  if (desc.fallback_material_id.empty()) {
    desc.fallback_material_id = m_fallbackId;
  }
  m_presets.push_back(std::move(desc));
  return true;
}

MaterialValidationResult MaterialRegistry::validate_material(const MaterialDesc& desc) const {
  MaterialValidationResult result;
  result.fallback_material_id = desc.fallback_material_id.empty() ? m_fallbackId : desc.fallback_material_id;

  // Validation is deliberately permissive for missing textures but strict for numeric packing ranges.
  auto invalidate = [&](std::string message) {
    result.valid = false;
    result.warnings.push_back(std::move(message));
  };
  auto warn = [&](std::string message) {
    result.warnings.push_back(std::move(message));
  };

  if (desc.id.empty()) {
    invalidate("material id is empty");
  }
  const auto family_it = std::find_if(m_families.begin(), m_families.end(),
                                      [&](const MaterialDescriptor& family) { return family.family == desc.family; });
  if (family_it == m_families.end()) {
    invalidate("material family is not registered");
  }
  for (const auto value : desc.base_color) {
    if (!InRange01(value)) {
      invalidate("base_color components must be finite in [0,1]");
      break;
    }
  }
  if (!InRange01(desc.roughness)) invalidate("roughness must be finite in [0,1]");
  if (!InRange01(desc.metallic)) invalidate("metallic must be finite in [0,1]");
  if (!IsFinitePositive(desc.ior)) invalidate("ior must be finite and positive");
  if (!InRange01(desc.transmission)) invalidate("transmission must be finite in [0,1]");
  if (!std::isfinite(desc.emission_intensity) || desc.emission_intensity < 0.0f) {
    invalidate("emission_intensity must be finite and non-negative");
  }
  for (const auto value : desc.emission) {
    if (!std::isfinite(value) || value < 0.0f) {
      invalidate("emission components must be finite and non-negative");
      break;
    }
  }
  if (!InRange01(desc.alpha_cutoff)) invalidate("alpha_cutoff must be finite in [0,1]");
  if (!InRange01(desc.clearcoat)) invalidate("clearcoat must be finite in [0,1]");
  if (!InRange01(desc.sheen)) invalidate("sheen must be finite in [0,1]");
  if (!InRange01(desc.anisotropy)) invalidate("anisotropy must be finite in [0,1]");

  for (const auto& binding : desc.texture_bindings) {
    if (binding.slot.empty()) {
      invalidate("texture binding has empty slot");
    }
    if (binding.texture_asset_urn.empty()) {
      warn("texture binding '" + binding.slot + "' has no texture asset assigned");
    }
  }
  for (const auto& sampler : desc.sampler_bindings) {
    if (sampler.sampler_id.empty()) {
      invalidate("sampler binding has empty sampler_id");
    }
    if (!std::isfinite(sampler.anisotropy) || sampler.anisotropy < 1.0f || sampler.anisotropy > 16.0f) {
      invalidate("sampler anisotropy must be finite in [1,16]");
    }
  }
  if (!desc.normal_map.empty()) {
    const auto has_normal_binding = std::any_of(desc.texture_bindings.begin(), desc.texture_bindings.end(),
                                                [](const MaterialTextureBinding& binding) {
                                                  return binding.slot == "normal";
                                                });
    if (!has_normal_binding) {
      warn("normal_map is set but no normal texture binding is present");
    }
  }
  if (!result.valid) {
    result.warnings.push_back("invalid material should resolve to fallback '" + result.fallback_material_id + "'");
  }
  return result;
}

const MaterialDesc& MaterialRegistry::resolve_fallback(std::string_view requested_id) const {
  if (const auto* requested = find_preset(requested_id)) {
    const auto validation = validate_material(*requested);
    if (validation.valid) {
      return *requested;
    }
    if (const auto* fallback = find_preset(validation.fallback_material_id)) {
      return *fallback;
    }
  }
  if (const auto* fallback = find_preset(m_fallbackId)) {
    return *fallback;
  }
  return m_presets.front();
}

bool MaterialRegistry::query_benchmark_approval(std::string_view material_id) const {
  if (const auto* preset = find_preset(material_id)) {
    return preset->benchmark_approved;
  }
  if (const auto* descriptor = FindMaterial(material_id)) {
    return descriptor->benchmark_approved;
  }
  return false;
}

const MaterialDesc* MaterialRegistry::find_preset(std::string_view material_id) const {
  for (const auto& preset : m_presets) {
    if (preset.id == material_id) {
      return &preset;
    }
  }
  return nullptr;
}

std::string MaterialRegistry::dump_registry() const {
  std::ostringstream out;
  out << "{\"schema\":\"vkpt.material_registry.v2\",\"families\":[";
  for (std::size_t i = 0; i < m_families.size(); ++i) {
    if (i > 0) out << ",";
    const auto& family = m_families[i];
    out << "{";
    out << "\"family\":\"" << ToString(family.family) << "\",";
    out << "\"id\":\"" << EscapeJson(family.id) << "\",";
    out << "\"display_name\":\"" << EscapeJson(family.display_name) << "\",";
    out << "\"status\":\"" << ToString(family.status) << "\",";
    out << "\"benchmark_approved\":" << (family.benchmark_approved ? "true" : "false") << ",";
    out << "\"notes\":\"" << EscapeJson(family.notes) << "\"";
    out << "}";
  }
  out << "],\"presets\":[";
  for (std::size_t i = 0; i < m_presets.size(); ++i) {
    if (i > 0) out << ",";
    out << SerializeMaterialDesc(m_presets[i]);
  }
  out << "]}";
  return out.str();
}

const std::vector<MaterialDescriptor>& MaterialRegistry::families() const {
  return m_families;
}

const std::vector<MaterialDesc>& MaterialRegistry::presets() const {
  return m_presets;
}

IMaterialRegistry& GetDefaultMaterialRegistry() {
  static MaterialRegistry registry;
  return registry;
}

std::string SerializeMaterialRegistry() {
  const auto& reg = GetMaterialRegistry();
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < reg.size(); ++i) {
    const auto& d = reg[i];
    if (i > 0) out << ",";
    out << "{";
    out << "\"family\":\"" << ToString(d.family) << "\",";
    out << "\"id\":\"" << d.id << "\",";
    out << "\"display_name\":\"" << d.display_name << "\",";
    out << "\"status\":\"" << ToString(d.status) << "\",";
    out << "\"benchmark_approved\":" << (d.benchmark_approved ? "true" : "false") << ",";
    out << "\"notes\":\"" << d.notes << "\"";
    out << "}";
  }
  out << "]";
  return out.str();
}

}  // namespace vkpt::materials
