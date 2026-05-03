#include "materials/MaterialDescriptors.h"

#include <algorithm>
#include <sstream>

namespace vkpt::materials {

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

const std::vector<MaterialDescriptor>& GetMaterialRegistry() {
  static const std::vector<MaterialDescriptor> s_registry = [] {
    std::vector<MaterialDescriptor> r;

    // Pack 1 — benchmark core (13 materials)
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

    // Pack 2 — experimental (19 materials)
    r.push_back({MaterialFamily::Velvet,                    "velvet",                    "Velvet",                   ImplementationStatus::Experimental, false, "Retro-reflective cloth sheen"});
    r.push_back({MaterialFamily::Charcoal,                  "charcoal",                  "Charcoal",                 ImplementationStatus::Experimental, false, "Dark absorptive diffuse"});
    r.push_back({MaterialFamily::Rubber,                    "rubber",                    "Rubber",                   ImplementationStatus::Experimental, false, "Low-IOR smooth dielectric"});
    r.push_back({MaterialFamily::XRay,                      "xray",                      "X-Ray",                    ImplementationStatus::Experimental, false, "Silhouette-emphasizing shader"});
    r.push_back({MaterialFamily::SubsurfaceApprox,          "subsurface_approx",         "Subsurface Approx",        ImplementationStatus::Experimental, false, "Dipole SSS approximation"});
    r.push_back({MaterialFamily::SpectralGlassApprox,       "spectral_glass_approx",     "Spectral Glass Approx",    ImplementationStatus::Experimental, false, "RGB dispersion glass"});
    r.push_back({MaterialFamily::ProceduralMaterial,        "procedural_material",       "Procedural Material",      ImplementationStatus::Experimental, false, "SDF-driven procedural patterns"});
    r.push_back({MaterialFamily::SdfFractalMaterial,        "sdf_fractal_material",      "SDF Fractal Material",     ImplementationStatus::Experimental, false, "Mandelbulb-shaded surface"});
    r.push_back({MaterialFamily::VolumetricShafts,          "volumetric_shafts",         "Volumetric Shafts",        ImplementationStatus::Experimental, false, "Participating media light shafts"});
    r.push_back({MaterialFamily::CausticsInspiredResponse,  "caustics_inspired_response","Caustics-Inspired Response",ImplementationStatus::Experimental, false, "Approximate caustic highlight"});
    r.push_back({MaterialFamily::ThinFilmIridescent,        "thin_film_iridescent",      "Thin Film Iridescent",     ImplementationStatus::Experimental, false, "Oil-slick thin film interference"});
    r.push_back({MaterialFamily::Retroreflector,            "retroreflector",            "Retroreflector",           ImplementationStatus::Experimental, false, "Corner-cube-like return shading"});
    r.push_back({MaterialFamily::VoronoiCracks,             "voronoi_cracks",            "Voronoi Cracks",           ImplementationStatus::Experimental, false, "Procedural crack pattern"});
    r.push_back({MaterialFamily::DiffractionGrating,        "diffraction_grating",       "Diffraction Grating",      ImplementationStatus::Experimental, false, "CD/DVD spectral diffraction"});
    r.push_back({MaterialFamily::AnisotropicGgx,            "anisotropic_ggx",           "Anisotropic GGX",          ImplementationStatus::Experimental, false, "Brushed-metal anisotropic NDF"});
    r.push_back({MaterialFamily::BlackbodyEmission,         "blackbody_emission",        "Blackbody Emission",       ImplementationStatus::Experimental, false, "Temperature-driven emission"});
    r.push_back({MaterialFamily::FirePlasma,                "fire_plasma",               "Fire/Plasma",              ImplementationStatus::Experimental, false, "Animated volumetric fire"});
    r.push_back({MaterialFamily::ToonSurface,               "toon_surface",              "Toon Surface",             ImplementationStatus::Experimental, false, "Cel-shaded step ramp"});
    r.push_back({MaterialFamily::BokehMotionBlurStress,     "bokeh_motion_blur_stress",  "Bokeh/Motion Blur Stress", ImplementationStatus::Experimental, false, "High-specular bokeh emitter"});

    // Pack 3 — backlog (15 materials)
    r.push_back({MaterialFamily::Plastic,           "plastic",            "Plastic",             ImplementationStatus::Backlog, false, "Layered dielectric + diffuse"});
    r.push_back({MaterialFamily::FabricCloth,       "fabric_cloth",       "Fabric Cloth",        ImplementationStatus::Backlog, false, "Woven microstructure"});
    r.push_back({MaterialFamily::PorcelainCeramic,  "porcelain_ceramic",  "Porcelain/Ceramic",   ImplementationStatus::Backlog, false, "Smooth matte + specular glaze"});
    r.push_back({MaterialFamily::Paint,             "paint",              "Paint",               ImplementationStatus::Backlog, false, "Lambertian + thin clearcoat"});
    r.push_back({MaterialFamily::CarPaint,          "car_paint",          "Car Paint",           ImplementationStatus::Backlog, false, "Metallic flake car paint"});
    r.push_back({MaterialFamily::WetSurface,        "wet_surface",        "Wet Surface",         ImplementationStatus::Backlog, false, "Extra specular from water film"});
    r.push_back({MaterialFamily::FrostedGlass,      "frosted_glass",      "Frosted Glass",       ImplementationStatus::Backlog, false, "Rough dielectric"});
    r.push_back({MaterialFamily::DirtyGlass,        "dirty_glass",        "Dirty Glass",         ImplementationStatus::Backlog, false, "Smudge layer over glass"});
    r.push_back({MaterialFamily::CorrosionOxidation,"corrosion_oxidation","Corrosion/Oxidation", ImplementationStatus::Backlog, false, "Rust/patina procedural blend"});
    r.push_back({MaterialFamily::Stone,             "stone",              "Stone",               ImplementationStatus::Backlog, false, "Granular rough diffuse"});
    r.push_back({MaterialFamily::Concrete,          "concrete",           "Concrete",            ImplementationStatus::Backlog, false, "Grey diffuse with cracks"});
    r.push_back({MaterialFamily::Plaster,           "plaster",            "Plaster",             ImplementationStatus::Backlog, false, "Matte whitish diffuse"});
    r.push_back({MaterialFamily::WaterFluidSurface, "water_fluid_surface","Water Fluid Surface",  ImplementationStatus::Backlog, false, "Animated dielectric surface"});
    r.push_back({MaterialFamily::IceCrystal,        "ice_crystal",        "Ice Crystal",         ImplementationStatus::Backlog, false, "Birefringent translucent solid"});
    r.push_back({MaterialFamily::StylizedDiffuse,   "stylized_diffuse",   "Stylized Diffuse",    ImplementationStatus::Backlog, false, "Artist-driven BRDF control"});

    // Advanced pack — deferred (25 materials)
    r.push_back({MaterialFamily::Skin,                    "skin",                    "Skin",                    ImplementationStatus::Deferred, false, "Multi-layer SSS skin"});
    r.push_back({MaterialFamily::Wax,                     "wax",                    "Wax",                     ImplementationStatus::Deferred, false, "Translucent wax SSS"});
    r.push_back({MaterialFamily::MarbleScattering,        "marble_scattering",       "Marble Scattering",       ImplementationStatus::Deferred, false, "Veined stone with SSS"});
    r.push_back({MaterialFamily::HairFurLobes,            "hair_fur_lobes",          "Hair/Fur Lobes",          ImplementationStatus::Deferred, false, "Marschner hair BCSDF"});
    r.push_back({MaterialFamily::EnergyConservingLayered, "energy_conserving_layered","Energy-Conserving Layered",ImplementationStatus::Deferred, false, "Multi-layer energy compensation"});
    r.push_back({MaterialFamily::VolumetricMedium,        "volumetric_medium",       "Volumetric Medium",       ImplementationStatus::Deferred, false, "Heterogeneous volume"});
    r.push_back({MaterialFamily::Mud,                     "mud",                     "Mud",                     ImplementationStatus::Deferred, false, "Wet diffuse clay"});
    r.push_back({MaterialFamily::Sand,                    "sand",                    "Sand",                    ImplementationStatus::Deferred, false, "Granular micro-facet"});
    r.push_back({MaterialFamily::TerraEarth,              "terra_earth",             "Terra/Earth",             ImplementationStatus::Deferred, false, "Mixed soil material"});
    r.push_back({MaterialFamily::BrushedMetal,            "brushed_metal",           "Brushed Metal",           ImplementationStatus::Deferred, false, "Anisotropic conductor"});
    r.push_back({MaterialFamily::GroundMetal,             "ground_metal",            "Ground Metal",            ImplementationStatus::Deferred, false, "Rough isotropic conductor"});
    r.push_back({MaterialFamily::FireSparkleEmission,     "fire_sparkle_emission",   "Fire Sparkle Emission",   ImplementationStatus::Deferred, false, "Stochastic emissive particles"});
    r.push_back({MaterialFamily::LightEmittingTextile,    "light_emitting_textile",  "Light-Emitting Textile",  ImplementationStatus::Deferred, false, "LED fiber cloth"});
    r.push_back({MaterialFamily::HolographicCoating,      "holographic_coating",     "Holographic Coating",     ImplementationStatus::Deferred, false, "Spectral grating film"});
    r.push_back({MaterialFamily::Paper,                   "paper",                   "Paper",                   ImplementationStatus::Deferred, false, "Translucent cellulose"});
    r.push_back({MaterialFamily::Cardboard,               "cardboard",               "Cardboard",               ImplementationStatus::Deferred, false, "Rough opaque layered paper"});
    r.push_back({MaterialFamily::Resin,                   "resin",                   "Resin",                   ImplementationStatus::Deferred, false, "Clear solid dielectric"});
    r.push_back({MaterialFamily::Epoxy,                   "epoxy",                   "Epoxy",                   ImplementationStatus::Deferred, false, "Amber-tinted solid resin"});
    r.push_back({MaterialFamily::Gemstone,                "gemstone",                "Gemstone",                ImplementationStatus::Deferred, false, "High-IOR spectral crystal"});
    r.push_back({MaterialFamily::Smoke,                   "smoke",                   "Smoke",                   ImplementationStatus::Deferred, false, "Isotropic scattering volume"});
    r.push_back({MaterialFamily::ChromaticDust,           "chromatic_dust",          "Chromatic Dust",          ImplementationStatus::Deferred, false, "Coloured particle cloud"});
    r.push_back({MaterialFamily::PearlLustre,             "pearl_lustre",            "Pearl Lustre",            ImplementationStatus::Deferred, false, "Multi-layer iridescent coating"});
    r.push_back({MaterialFamily::FrostedAcrylic,          "frosted_acrylic",         "Frosted Acrylic",         ImplementationStatus::Deferred, false, "Rough translucent polymer"});
    r.push_back({MaterialFamily::TranslucentPolymer,      "translucent_polymer",     "Translucent Polymer",     ImplementationStatus::Deferred, false, "Semi-transparent plastic"});
    r.push_back({MaterialFamily::RustProgression,         "rust_progression",        "Rust Progression",        ImplementationStatus::Deferred, false, "Animated corrosion stages"});

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
