#pragma once

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "render/backends/FrameGraph.h"
#include "render/backends/NullBackend.h"
#include "render/interface/RenderContracts.h"

namespace vkpt::render {

namespace detail {

inline std::string LowerCopy(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

inline bool EndsWith(std::string_view value, std::string_view suffix) {
  if (suffix.size() > value.size()) {
    return false;
  }
  return value.substr(value.size() - suffix.size()) == suffix;
}

inline bool SourcePathMatchesFormat(std::string_view source_path, ShaderSourceFormat format) {
  const std::string lower = LowerCopy(source_path);
  switch (format) {
    case ShaderSourceFormat::Wgsl:
      return EndsWith(lower, ".wgsl");
    case ShaderSourceFormat::Msl:
      return EndsWith(lower, ".metal") || EndsWith(lower, ".msl");
    case ShaderSourceFormat::Hlsl:
      return EndsWith(lower, ".hlsl");
    case ShaderSourceFormat::Glsl:
      return EndsWith(lower, ".glsl") || EndsWith(lower, ".comp");
    case ShaderSourceFormat::Unknown:
    default:
      return true;
  }
}

struct AdapterBackendConfig {
  BackendKind kind = BackendKind::Unknown;
  std::string stable_name;
  std::string display_name;
  ShaderSourceFormat required_source_format = ShaderSourceFormat::Unknown;
  std::string default_source_path;
  std::vector<std::string> supported_features;
  RenderBackendCapabilities capabilities;
};

/// Compiler facade shared by optional adapter skeletons.
///
/// It validates source format and entry-point metadata, then emits a synthetic
/// artifact id; native shader-module creation belongs to future backend adapters.
class AdapterShaderCompiler final : public IShaderCompiler {
 public:
  explicit AdapterShaderCompiler(AdapterBackendConfig config) : m_config(std::move(config)) {}

  bool supports_feature(std::string_view feature) const override {
    return std::find(m_config.supported_features.begin(), m_config.supported_features.end(), std::string(feature)) !=
           m_config.supported_features.end();
  }

  bool compile_compute_shader(const ComputePipelineDesc& desc,
                              std::string& out_artifact,
                              std::string* diagnostics) override {
    const std::string source_path = desc.source_path.empty() ? m_config.default_source_path : desc.source_path;
    if (source_path.empty()) {
      if (diagnostics) {
        *diagnostics = m_config.stable_name + " compute shader compile failed: missing source path";
      }
      return false;
    }
    if (desc.entry_point.empty()) {
      if (diagnostics) {
        *diagnostics = m_config.stable_name + " compute shader compile failed: missing entry point";
      }
      return false;
    }
    if (!SourcePathMatchesFormat(source_path, m_config.required_source_format)) {
      if (diagnostics) {
        *diagnostics = m_config.stable_name + " requires " +
                       std::string(ShaderSourceFormatToString(m_config.required_source_format)) +
                       " shader source; got '" + source_path + "'";
      }
      return false;
    }

    std::string defines;
    for (const auto& define : desc.defines) {
      if (!defines.empty()) {
        defines.push_back(',');
      }
      defines += define;
    }
    out_artifact = m_config.stable_name + ":" + std::string(ShaderSourceFormatToString(m_config.required_source_format)) +
                   ":" + source_path + ":" + desc.entry_point + ":" + defines;
    if (diagnostics) {
      *diagnostics = "skeleton validation only; no native compiler invoked";
    }
    return true;
  }

 private:
  AdapterBackendConfig m_config;
};

/// In-memory shader cache used by adapter skeletons.
class AdapterShaderCache final : public IShaderCache {
 public:
  AdapterShaderCache(std::string backend, ShaderSourceFormat source_format, std::string default_source_path)
      : m_backend(std::move(backend)),
        m_source_format(source_format),
        m_default_source_path(std::move(default_source_path)) {}

  bool query(std::string_view key, std::string& binary) override {
    const auto it = m_entries.find(std::string(key));
    if (it == m_entries.end()) {
      return false;
    }
    binary = it->second;
    return true;
  }

  bool store(std::string_view key, const std::string& binary) override {
    m_entries[std::string(key)] = binary;
    return true;
  }

  bool invalidate(std::string_view key) override {
    const auto it = m_entries.find(std::string(key));
    if (it == m_entries.end()) {
      return false;
    }
    m_entries.erase(it);
    return true;
  }

  std::string explain_miss(std::string_view key) const override {
    return m_backend + " shader cache miss: " + std::string(key);
  }

  std::vector<CachedManifest> dump_manifest() const override {
    std::vector<CachedManifest> out;
    out.reserve(m_entries.size());
    for (const auto& entry : m_entries) {
      ShaderManifest manifest;
      manifest.shader_family = "cached";
      manifest.entry_point = "main";
      manifest.backend = m_backend;
      manifest.source_format = m_source_format;
      manifest.source_path = m_default_source_path;
      manifest.source_hash = MakeShaderManifestHash(entry.first);
      manifest.variant_hash = MakeShaderManifestHash(entry.second);
      manifest.cache_key = entry.first;
      manifest.artifact_path = entry.second;
      manifest.manifest_dump_path = BuildShaderManifestDumpPath("shader_cache", manifest);
      manifest.compile_success = true;
      manifest.validation_success = true;
      out.push_back({SerializeShaderManifest(manifest),
                     m_backend,
                     manifest.cache_key,
                     manifest.artifact_path,
                     manifest.manifest_dump_path,
                     manifest.compile_success});
    }
    return out;
  }

 private:
  std::string m_backend;
  ShaderSourceFormat m_source_format = ShaderSourceFormat::Unknown;
  std::string m_default_source_path;
  std::unordered_map<std::string, std::string> m_entries;
};

/// Optional backend skeleton for platform APIs not yet wired to native devices.
class AdapterBackend final : public IRenderBackend {
 public:
  explicit AdapterBackend(AdapterBackendConfig config) : m_config(std::move(config)) {}

  bool initialize() override {
    if (m_initialized) {
      return true;
    }
    m_compiler = std::make_unique<AdapterShaderCompiler>(m_config);
    m_cache = std::make_unique<AdapterShaderCache>(
        m_config.stable_name, m_config.required_source_format, m_config.default_source_path);
    m_initialized = true;
    return true;
  }

  bool shutdown() override {
    m_initialized = false;
    m_compiler.reset();
    m_cache.reset();
    return true;
  }

  BackendKind kind() const override { return m_config.kind; }
  std::string name() const override { return m_config.display_name; }
  RenderBackendCapabilities capabilities() const override { return m_config.capabilities; }

  std::unique_ptr<IRenderDevice> create_device() override {
    if (!m_initialized) {
      return {};
    }
    auto allocator = std::make_unique<NullResourceAllocator>();
    auto swapchain = std::make_unique<NullSwapchain>();
    return std::make_unique<NullDevice>(std::move(allocator), std::move(swapchain));
  }

  IShaderCompiler* compiler() override { return m_compiler.get(); }
  IShaderCache* shader_cache() override { return m_cache.get(); }
  std::unique_ptr<IFrameGraph> create_frame_graph() override { return std::make_unique<FrameGraph>(); }
  std::string last_error() const override { return m_last_error; }

 private:
  AdapterBackendConfig m_config;
  bool m_initialized = false;
  std::string m_last_error;
  std::unique_ptr<AdapterShaderCompiler> m_compiler;
  std::unique_ptr<AdapterShaderCache> m_cache;
};

/// Run the common one-pass compute graph against an adapter skeleton.
inline bool RunAdapterComputeSmoke(IRenderBackend& backend, std::string_view label) {
  if (!backend.initialize()) {
    return false;
  }
  auto device = backend.create_device();
  if (!device || !device->begin()) {
    return false;
  }
  auto context = device->create_command_context();
  if (!context) {
    device->end();
    return false;
  }
  auto graph = backend.create_frame_graph();
  FrameGraphDesc desc;
  desc.debug_label = std::string(label);
  desc.passes = {{0u, "compute", PassType::Compute, {}, {}}};
  std::vector<std::string> diagnostics;
  if (!graph || !graph->build(desc, &diagnostics)) {
    device->end();
    return false;
  }
  FrameContext frame;
  frame.debug_label = desc.debug_label;
  const bool ok = graph->execute(*context, frame);
  device->end();
  return ok;
}

/// Fill conservative capability defaults for simulated adapter skeletons.
inline RenderBackendCapabilities MakeAdapterBaseCapabilities(std::string backend_name) {
  RenderBackendCapabilities caps;
  caps.backend_name = std::move(backend_name);
  caps.compute = true;
  caps.storage_buffers = true;
  caps.storage_textures = true;
  caps.timestamp_queries = false;
  caps.subgroups = false;
  caps.descriptor_indexing = false;
  caps.bindless_like_resources = false;
  caps.texture_formats = true;
  caps.ray_tracing = false;
  caps.ray_query = false;
  caps.ray_query_supported = false;
  caps.acceleration_structure_supported = false;
  caps.presentation = false;
  caps.readback = true;
  caps.is_simulated = true;
  caps.supports_present = false;
  caps.supports_multiqueue = false;
  caps.max_workgroup_size_x = 256u;
  caps.max_workgroup_size_y = 256u;
  caps.max_workgroup_size_z = 64u;
  caps.max_buffer_alignment = 256u;
  caps.platform.headless = true;
  caps.texture_formats_caps.rgba8_unorm = true;
  caps.texture_formats_caps.rgba16_float = true;
  caps.texture_formats_caps.storage_texture_formats = true;
  caps.texture_formats_caps.sampled_texture_formats = true;
  caps.texture_formats_caps.guaranteed_formats = {"RGBA8", "RGBA16F"};
  caps.memory_budget.upload_alignment_bytes = 256u;
  caps.memory_budget.readback_alignment_bytes = 256u;
  caps.memory_budget.max_buffer_size_bytes = 512ull * 1024ull * 1024ull;
  caps.memory_budget.budget_unavailable_reason = "adapter skeleton does not own native GPU memory";
  return caps;
}

}  // namespace detail

#if defined(PT_ENABLE_METAL)
inline std::unique_ptr<IRenderBackend> CreateMetalBackendSkeleton() {
  auto caps = detail::MakeAdapterBaseCapabilities("metal-compute (skeleton)");
  caps.timestamp_fallback_reason = "Metal timestamp sampling is not wired in the skeleton; use CPU frame timing";
  caps.memory_model = "metal-skeleton";
  caps.notes = "Metal adapter skeleton; no MTLDevice, MTLCommandQueue, or native handles are exposed.";
  caps.platform.platform_name = "macos-metal";
  caps.platform.native_surface = false;
  caps.platform.notes = "Native Metal surface hookup is intentionally outside this renderer contract.";
  caps.ray_tracing_caps.unsupported_reason = "Metal acceleration-structure support is not probed until a native MTLDevice adapter is linked";
  caps.shader.msl = true;
  caps.shader.supported_source_formats = {"msl"};
  caps.shader.notes = "MSL source contract only; native Metal compiler is not invoked.";
  caps.texture_formats_caps.bgra8_unorm = true;
  caps.texture_formats_caps.guaranteed_formats.push_back("BGRA8");

  detail::AdapterBackendConfig config;
  config.kind = BackendKind::Metal;
  config.stable_name = "metal";
  config.display_name = caps.backend_name;
  config.required_source_format = ShaderSourceFormat::Msl;
  config.default_source_path = "src/shaders/gpu/pathtrace.metal";
  config.supported_features = {"compute", "storage-buffers", "storage-textures", "msl"};
  config.capabilities = std::move(caps);
  return std::make_unique<detail::AdapterBackend>(std::move(config));
}

inline bool RunMetalComputeSmoke(IRenderBackend& backend) {
  return detail::RunAdapterComputeSmoke(backend, "metal_compute_smoke");
}
#endif

#if defined(PT_ENABLE_WEBGPU)
inline std::unique_ptr<IRenderBackend> CreateWebGpuBackendSkeleton() {
  auto caps = detail::MakeAdapterBaseCapabilities("webgpu-compute (wgsl skeleton)");
  caps.timestamp_fallback_reason = "WebGPU timestamp-query is optional and not requested by the skeleton; use CPU frame timing";
  caps.memory_model = "webgpu-wgsl";
  caps.notes = "WebGPU adapter skeleton with first-class WGSL shader path; browser canvas/device hookup is external.";
  caps.platform.platform_name = "webgpu";
  caps.platform.browser_canvas = true;
  caps.platform.wasm = true;
  caps.platform.notes = "Requires a browser or native WebGPU device provider outside this adapter.";
  caps.ray_tracing_caps.unsupported_reason = "WebGPU does not expose a portable hardware ray tracing pipeline in this contract";
  caps.shader.wgsl = true;
  caps.shader.supported_source_formats = {"wgsl"};
  caps.shader.notes = "WGSL validation contract only; native WebGPU shader module creation is not invoked.";

  detail::AdapterBackendConfig config;
  config.kind = BackendKind::WebGpu;
  config.stable_name = "webgpu";
  config.display_name = caps.backend_name;
  config.required_source_format = ShaderSourceFormat::Wgsl;
  config.default_source_path = "src/shaders/gpu/pathtrace.wgsl";
  config.supported_features = {"compute", "storage-buffers", "storage-textures", "wgsl"};
  config.capabilities = std::move(caps);
  return std::make_unique<detail::AdapterBackend>(std::move(config));
}

inline bool RunWebGpuComputeSmoke(IRenderBackend& backend) {
  return detail::RunAdapterComputeSmoke(backend, "webgpu_compute_smoke");
}
#endif

#if defined(PT_ENABLE_OPENGL_EXPERIMENTAL)
inline std::unique_ptr<IRenderBackend> CreateOpenGLExperimentalBackendSkeleton() {
  auto caps = detail::MakeAdapterBaseCapabilities("opengl-compute (experimental skeleton)");
  caps.timestamp_fallback_reason = "OpenGL timestamp queries are not wired in the experimental skeleton; use CPU frame timing";
  caps.memory_model = "opengl-experimental";
  caps.notes = "Experimental OpenGL 4.3+ compute adapter; disabled unless PT_ENABLE_OPENGL_EXPERIMENTAL is set.";
  caps.platform.platform_name = "opengl-experimental";
  caps.platform.native_surface = false;
  caps.platform.notes = "No GL context or platform surface is created by this skeleton.";
  caps.ray_tracing_caps.unsupported_reason = "OpenGL experimental compute path has no hardware ray tracing contract";
  caps.shader.glsl = true;
  caps.shader.supported_source_formats = {"glsl"};
  caps.shader.notes = "GLSL compute source contract only; no GL shader object is created.";

  detail::AdapterBackendConfig config;
  config.kind = BackendKind::OpenGLExperimental;
  config.stable_name = "opengl-experimental";
  config.display_name = caps.backend_name;
  config.required_source_format = ShaderSourceFormat::Glsl;
  config.default_source_path = "src/shaders/gpu/pathtrace.comp";
  config.supported_features = {"compute", "storage-buffers", "storage-textures", "glsl"};
  config.capabilities = std::move(caps);
  return std::make_unique<detail::AdapterBackend>(std::move(config));
}

inline bool RunOpenGLExperimentalComputeSmoke(IRenderBackend& backend) {
  return detail::RunAdapterComputeSmoke(backend, "opengl_experimental_compute_smoke");
}
#endif

}  // namespace vkpt::render
