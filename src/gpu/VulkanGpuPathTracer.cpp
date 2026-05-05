#ifdef PT_ENABLE_VULKAN

#include "gpu/VulkanGpuPathTracer.h"
#include "core/Logging.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

// Convenience macro: check VkResult and log+return-false on failure
#define VK_CHK(expr)                                                            \
  do {                                                                           \
    VkResult _r = (expr);                                                       \
    if (_r != VK_SUCCESS) {                                                      \
      std::ostringstream _ss;                                                    \
      _ss << #expr << " failed: VkResult=" << static_cast<int>(_r);             \
      m_error = _ss.str();                                                       \
      vkpt::log::Logger::instance().log(                                         \
          vkpt::log::Severity::Error, "vulkan", m_error);                        \
      return false;                                                              \
    }                                                                            \
  } while (0)

namespace vkpt::gpu {

// ============================================================================
// Construction / destruction
// ============================================================================

VulkanGpuPathTracer::VulkanGpuPathTracer(std::string spv_path)
    : m_spvPath(std::move(spv_path)) {
  m_valid = init_device() && create_cmd_pool() && create_pipeline();
  if (!m_valid) {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Error, "vulkan",
        "VulkanGpuPathTracer init failed: " + m_error);
  }
}

VulkanGpuPathTracer::~VulkanGpuPathTracer() {
  shutdown();
}

// ============================================================================
// IPathTracer
// ============================================================================

bool VulkanGpuPathTracer::configure(const vkpt::pathtracer::RenderSettings& s) {
  if (!m_valid) return false;
  m_settings    = s;
  m_configured  = true;
  m_filmPixels  = s.width * s.height;

  // (Re)allocate film buffer
  destroy_film_buffers();
  const VkDeviceSize filmBytes =
      static_cast<VkDeviceSize>(m_filmPixels) * 4u * sizeof(float);
  if (!make_buffer(filmBytes,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   m_filmBuf, m_filmMem)) return false;
  VK_CHK(vkMapMemory(m_device, m_filmMem, 0, filmBytes, 0, &m_filmPtr));
  std::memset(m_filmPtr, 0, static_cast<std::size_t>(filmBytes));

  m_film.resize(s.width, s.height);
  m_film.clear();
  m_counters      = {};
  m_hasScene      = false;
  m_sceneUploaded = false;

  // Descriptors depend on the film buffer — reset them
  if (m_ds != VK_NULL_HANDLE) {
    vkFreeDescriptorSets(m_device, m_dsPool, 1, &m_ds);
    m_ds = VK_NULL_HANDLE;
  }
  return create_descriptors();
}

bool VulkanGpuPathTracer::load_scene_snapshot(
    const vkpt::pathtracer::RTSceneData& scene) {
  if (!m_configured) return false;
  m_sceneData     = scene;
  m_hasScene      = true;
  m_sceneUploaded = false;
  return true;
}

bool VulkanGpuPathTracer::build_or_update_acceleration() {
  if (!m_hasScene) return false;

  // Pack RTSceneData into flat GPU arrays
  m_gpuVerts.clear();
  for (const auto& v : m_sceneData.vertices) {
    m_gpuVerts.push_back(v.x); m_gpuVerts.push_back(v.y); m_gpuVerts.push_back(v.z);
  }
  if (m_gpuVerts.empty()) m_gpuVerts.assign(3, 0.0f);

  m_gpuIdx = m_sceneData.indices;
  if (m_gpuIdx.empty()) m_gpuIdx.push_back(0u);

  m_gpuMats.clear();
  for (const auto& m : m_sceneData.materials) {
    m_gpuMats.push_back(m.albedo.x);   m_gpuMats.push_back(m.albedo.y);
    m_gpuMats.push_back(m.albedo.z);
    m_gpuMats.push_back(m.emissive.x); m_gpuMats.push_back(m.emissive.y);
    m_gpuMats.push_back(m.emissive.z);
    m_gpuMats.push_back(m.roughness);  m_gpuMats.push_back(static_cast<float>(m.material_model));
    m_gpuMats.push_back(m.metallic);   m_gpuMats.push_back(m.ior);
    m_gpuMats.push_back(m.transmission); m_gpuMats.push_back(m.clearcoat);
    m_gpuMats.push_back(m.sheen);      m_gpuMats.push_back(m.anisotropy);
    const uint32_t packed_effect = (m.material_effect & 1023u) | ((m.material_flags & 1u) ? 1024u : 0u);
    m_gpuMats.push_back(m.alpha);      m_gpuMats.push_back(static_cast<float>(packed_effect));
  }
  if (m_gpuMats.empty()) m_gpuMats.assign(16, 0.0f);

  m_gpuInsts.clear();
  for (const auto& inst : m_sceneData.instances) {
    m_gpuInsts.push_back(inst.first_triangle);
    m_gpuInsts.push_back(inst.triangle_count);
    m_gpuInsts.push_back(inst.material_index);
    m_gpuInsts.push_back(0u);
  }
  if (m_gpuInsts.empty()) m_gpuInsts.assign(4, 0u);

  m_gpuLights.clear();
  for (const auto& lt : m_sceneData.lights) {
    m_gpuLights.push_back(lt.position.x); m_gpuLights.push_back(lt.position.y);
    m_gpuLights.push_back(lt.position.z);
    m_gpuLights.push_back(lt.color.x);    m_gpuLights.push_back(lt.color.y);
    m_gpuLights.push_back(lt.color.z);
    m_gpuLights.push_back(lt.intensity);  m_gpuLights.push_back(std::max(0.0f, lt.radius));
  }
  if (m_gpuLights.empty()) m_gpuLights.assign(8, 0.0f);

  // Upload — use HOST_VISIBLE | HOST_COHERENT buffers (mapped directly)
  destroy_scene_buffers();

  struct Upload { const void* data; VkDeviceSize sz; VkBuffer* buf; VkDeviceMemory* mem; };
  const Upload ups[] = {
    {m_gpuVerts.data(),  m_gpuVerts.size()  * sizeof(float),    &m_vertBuf, &m_vertMem},
    {m_gpuIdx.data(),    m_gpuIdx.size()    * sizeof(uint32_t), &m_idxBuf,  &m_idxMem},
    {m_gpuMats.data(),   m_gpuMats.size()   * sizeof(float),    &m_matBuf,  &m_matMem},
    {m_gpuInsts.data(),  m_gpuInsts.size()  * sizeof(uint32_t), &m_instBuf, &m_instMem},
    {m_gpuLights.data(), m_gpuLights.size() * sizeof(float),    &m_ltBuf,   &m_ltMem},
  };
  for (const auto& u : ups) {
    if (!make_buffer(u.sz,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     *u.buf, *u.mem)) return false;
    void* p = nullptr;
    VK_CHK(vkMapMemory(m_device, *u.mem, 0, u.sz, 0, &p));
    std::memcpy(p, u.data, static_cast<std::size_t>(u.sz));
    vkUnmapMemory(m_device, *u.mem);
  }
  m_sceneUploaded = true;

  // Rebind descriptors
  if (m_ds != VK_NULL_HANDLE) {
    vkFreeDescriptorSets(m_device, m_dsPool, 1, &m_ds);
    m_ds = VK_NULL_HANDLE;
  }
  return create_descriptors();
}

bool VulkanGpuPathTracer::reset_accumulation() {
  if (!m_configured || !m_filmPtr) return false;
  std::memset(m_filmPtr, 0,
              static_cast<std::size_t>(m_filmPixels) * 4u * sizeof(float));
  m_film.clear();
  m_counters = {};
  return true;
}

bool VulkanGpuPathTracer::render_sample_batch(
    uint32_t /*sy*/, uint32_t /*ey*/,
    uint32_t sample_idx, uint32_t frame_idx) {
  if (!m_valid || !m_configured || !m_sceneUploaded || !m_filmPtr) return false;
  if (m_pipeline == VK_NULL_HANDLE || m_ds == VK_NULL_HANDLE)       return false;

  const auto& sc = m_sceneData;

  // Camera basis — matches ScalarCpuPathTracer::build_or_update_acceleration
  auto norm3 = [](float x, float y, float z, float out[3]) {
    float l = std::sqrt(x*x + y*y + z*z);
    if (l < 1e-9f) l = 1.0f;
    out[0] = x/l; out[1] = y/l; out[2] = z/l;
  };
  float fwd[3];
  norm3(sc.camera_target.x - sc.camera_position.x,
        sc.camera_target.y - sc.camera_position.y,
        sc.camera_target.z - sc.camera_position.z, fwd);
  // right = cross(forward, camera_up)
  float rt[3] = {
    fwd[1]*sc.camera_up.z - fwd[2]*sc.camera_up.y,
    fwd[2]*sc.camera_up.x - fwd[0]*sc.camera_up.z,
    fwd[0]*sc.camera_up.y - fwd[1]*sc.camera_up.x
  };
  float rn[3]; norm3(rt[0], rt[1], rt[2], rn);
  // up = cross(right, forward)
  float un[3]; norm3(rn[1]*fwd[2]-rn[2]*fwd[1],
                     rn[2]*fwd[0]-rn[0]*fwd[2],
                     rn[0]*fwd[1]-rn[1]*fwd[0], un);

  PathTracePushConstants pc{};
  pc.camera_pos[0] = sc.camera_position.x;
  pc.camera_pos[1] = sc.camera_position.y;
  pc.camera_pos[2] = sc.camera_position.z;
  pc.fov_tan_half  = std::tan(0.5f * sc.camera_fov_deg * 3.14159265f / 180.0f);
  pc.cam_forward[0]=fwd[0]; pc.cam_forward[1]=fwd[1]; pc.cam_forward[2]=fwd[2];
  pc.aspect        = static_cast<float>(m_settings.width) /
                     std::max(1.0f, static_cast<float>(m_settings.height));
  pc.cam_right[0]  = rn[0];  pc.cam_right[1]  = rn[1];  pc.cam_right[2]  = rn[2];
  pc.pad0          = 0.0f;
  pc.cam_up[0]     = un[0];  pc.cam_up[1]     = un[1];  pc.cam_up[2]     = un[2];
  pc.sample_index  = sample_idx;
  pc.num_insts     = static_cast<uint32_t>(sc.instances.size());
  pc.num_mats      = static_cast<uint32_t>(sc.materials.size());
  pc.num_lights    = static_cast<uint32_t>(sc.lights.size());
  pc.width         = m_settings.width;
  pc.height        = m_settings.height;
  pc.base_seed     = static_cast<uint32_t>(m_settings.seed & 0xFFFFFFFFu)
                     ^ (frame_idx * 2654435761u);
  pc.env_color[0]  = sc.environment_color.x;
  pc.env_color[1]  = sc.environment_color.y;
  pc.env_color[2]  = sc.environment_color.z;
  pc.max_depth_f   = static_cast<float>(std::max(1u, m_settings.max_depth));

  // Record command buffer
  VK_CHK(vkResetCommandBuffer(m_cmdBuf, 0));
  VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHK(vkBeginCommandBuffer(m_cmdBuf, &cbi));

  vkCmdBindPipeline(m_cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
  vkCmdBindDescriptorSets(m_cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_pipeLayout, 0, 1, &m_ds, 0, nullptr);
  vkCmdPushConstants(m_cmdBuf, m_pipeLayout,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(PathTracePushConstants), &pc);
  vkCmdDispatch(m_cmdBuf,
                (m_settings.width + 7u) / 8u,
                (m_settings.height + 7u) / 8u, 1u);
  VK_CHK(vkEndCommandBuffer(m_cmdBuf));

  // Submit and wait
  VK_CHK(vkResetFences(m_device, 1, &m_fence));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers    = &m_cmdBuf;
  VK_CHK(vkQueueSubmit(m_queue, 1, &si, m_fence));
  VK_CHK(vkWaitForFences(m_device, 1, &m_fence, VK_TRUE,
                          static_cast<uint64_t>(10e9)));

  // GPU film buffer is HOST_COHERENT: directly readable by CPU.
  // Rebuild the CPU FilmBuffer from the accumulated GPU data so resolve_ldr works.
  const float* src = reinterpret_cast<const float*>(m_filmPtr);
  m_film.resize(m_settings.width, m_settings.height);
  m_film.clear();
  for (uint32_t y = 0; y < m_settings.height; ++y) {
    for (uint32_t x = 0; x < m_settings.width; ++x) {
      const uint32_t p   = (y * m_settings.width + x) * 4u;
      const float    cnt = std::max(1.0f, src[p + 3u]);
      // Store the mean as a single synthetic sample so the film resolve
      // divides by sample_count=1 and gives the correct average.
      const vkpt::pathtracer::Vec3 avg{src[p]/cnt, src[p+1]/cnt, src[p+2]/cnt};
      m_film.add_sample(x, y, avg);
    }
  }

  const uint64_t sampleInc =
      static_cast<uint64_t>(m_settings.width) * m_settings.height;
  m_counters.samples += sampleInc;
  m_counters.rays    += sampleInc;
  return true;
}

vkpt::pathtracer::FilmLdr VulkanGpuPathTracer::resolve_ldr() const {
  return m_film.resolve_ldr();
}
vkpt::pathtracer::FilmHdr VulkanGpuPathTracer::resolve_hdr() const {
  return m_film.resolve_hdr();
}
vkpt::pathtracer::SampleCounters VulkanGpuPathTracer::read_counters() const {
  return m_counters;
}

void VulkanGpuPathTracer::shutdown() {
  if (m_device == VK_NULL_HANDLE) return;
  vkDeviceWaitIdle(m_device);

  if (m_ds != VK_NULL_HANDLE) {
    vkFreeDescriptorSets(m_device, m_dsPool, 1, &m_ds);  m_ds = VK_NULL_HANDLE;
  }
  destroy_film_buffers();
  destroy_scene_buffers();
  destroy_pipeline();

  if (m_dsPool   != VK_NULL_HANDLE) { vkDestroyDescriptorPool(m_device, m_dsPool, nullptr);             m_dsPool   = VK_NULL_HANDLE; }
  if (m_dsLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(m_device, m_dsLayout, nullptr);      m_dsLayout = VK_NULL_HANDLE; }
  if (m_fence    != VK_NULL_HANDLE) { vkDestroyFence(m_device, m_fence, nullptr);                       m_fence    = VK_NULL_HANDLE; }
  if (m_cmdPool  != VK_NULL_HANDLE) { vkDestroyCommandPool(m_device, m_cmdPool, nullptr);               m_cmdPool  = VK_NULL_HANDLE; }
  if (m_device   != VK_NULL_HANDLE) { vkDestroyDevice(m_device, nullptr);                               m_device   = VK_NULL_HANDLE; }
  if (m_instance != VK_NULL_HANDLE) { vkDestroyInstance(m_instance, nullptr);                           m_instance = VK_NULL_HANDLE; }
  m_valid = false;
}

// ============================================================================
// Private helpers
// ============================================================================

bool VulkanGpuPathTracer::init_device() {
  // Instance (no surface extensions needed for off-screen compute)
  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  ai.pApplicationName   = "vkPathTracer";
  ai.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  ai.apiVersion         = VK_API_VERSION_1_2;

  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &ai;
  VK_CHK(vkCreateInstance(&ici, nullptr, &m_instance));

  // Physical device — prefer discrete
  uint32_t cnt = 0;
  VK_CHK(vkEnumeratePhysicalDevices(m_instance, &cnt, nullptr));
  if (cnt == 0) { m_error = "No Vulkan physical devices"; return false; }
  std::vector<VkPhysicalDevice> devs(cnt);
  VK_CHK(vkEnumeratePhysicalDevices(m_instance, &cnt, devs.data()));

  // Enumerate all physical devices and log them to help verify the right GPU
  // is selected.  We prefer DISCRETE_GPU over INTEGRATED_GPU.
  auto deviceTypeName = [](VkPhysicalDeviceType t) -> const char* {
    switch (t) {
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "DISCRETE";
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "INTEGRATED";
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "VIRTUAL";
      case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
      default:                                      return "OTHER";
    }
  };

  m_physDev = devs[0];
  for (uint32_t i = 0; i < devs.size(); ++i) {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(devs[i], &p);

    // Sum DEVICE_LOCAL heap sizes to get VRAM estimate
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(devs[i], &mp);
    uint64_t vramBytes = 0;
    for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
      if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        vramBytes += mp.memoryHeaps[h].size;
    const uint32_t vramMb = static_cast<uint32_t>(vramBytes / (1024u * 1024u));
    const uint32_t apiMaj = VK_VERSION_MAJOR(p.apiVersion);
    const uint32_t apiMin = VK_VERSION_MINOR(p.apiVersion);

    std::ostringstream ds;
    ds << "  [gpu" << i << "] " << p.deviceName
       << "  type=" << deviceTypeName(p.deviceType)
       << "  api=" << apiMaj << "." << apiMin
       << "  vram=" << vramMb << " MB"
       << (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "  <-- DISCRETE (preferred)" : "");
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Info, "vulkan", ds.str());

    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
      m_physDev = devs[i];   // last discrete wins; keeps highest-perf GPU
  }

  // Confirm selected device
  {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(m_physDev, &p);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(m_physDev, &mp);
    uint64_t vramBytes = 0;
    for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
      if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        vramBytes += mp.memoryHeaps[h].size;

    m_gpuName    = p.deviceName;
    m_vramMb     = static_cast<uint32_t>(vramBytes / (1024u * 1024u));
    m_apiVersion = p.apiVersion;
    m_gpuType    = deviceTypeName(p.deviceType);

    std::ostringstream sel;
    sel << "Selected GPU: " << m_gpuName
        << "  type=" << m_gpuType
        << "  api=" << VK_VERSION_MAJOR(m_apiVersion) << "." << VK_VERSION_MINOR(m_apiVersion)
        << "  vram=" << m_vramMb << " MB";
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Info, "vulkan", sel.str());
  }

  // Compute queue family
  uint32_t qfCnt = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(m_physDev, &qfCnt, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qfCnt);
  vkGetPhysicalDeviceQueueFamilyProperties(m_physDev, &qfCnt, qf.data());
  m_qFam = UINT32_MAX;
  for (uint32_t i = 0; i < qfCnt; ++i)
    if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { m_qFam = i; break; }
  if (m_qFam == UINT32_MAX) { m_error = "No compute queue family"; return false; }

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = m_qFam;
  qci.queueCount       = 1;
  qci.pQueuePriorities = &prio;

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos    = &qci;
  VK_CHK(vkCreateDevice(m_physDev, &dci, nullptr, &m_device));
  vkGetDeviceQueue(m_device, m_qFam, 0, &m_queue);
  return true;
}

bool VulkanGpuPathTracer::create_cmd_pool() {
  VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  ci.queueFamilyIndex = m_qFam;
  VK_CHK(vkCreateCommandPool(m_device, &ci, nullptr, &m_cmdPool));

  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool        = m_cmdPool;
  ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VK_CHK(vkAllocateCommandBuffers(m_device, &ai, &m_cmdBuf));

  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_CHK(vkCreateFence(m_device, &fi, nullptr, &m_fence));
  return true;
}

bool VulkanGpuPathTracer::create_pipeline() {
  // Load SPIR-V from file
  std::ifstream f(m_spvPath, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    m_error = "Cannot open SPIR-V: " + m_spvPath;
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Error, "vulkan", m_error);
    return false;
  }
  const auto spvSz = static_cast<std::size_t>(f.tellg());
  f.seekg(0);
  std::vector<uint32_t> spv(spvSz / 4u);
  f.read(reinterpret_cast<char*>(spv.data()), static_cast<std::streamsize>(spvSz));
  f.close();

  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = spvSz;
  smci.pCode    = spv.data();
  VK_CHK(vkCreateShaderModule(m_device, &smci, nullptr, &m_shaderMod));

  // Descriptor set layout — 6 storage buffers
  VkDescriptorSetLayoutBinding binds[6]{};
  for (uint32_t i = 0; i < 6; ++i) {
    binds[i].binding         = i;
    binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binds[i].descriptorCount = 1;
    binds[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dsli.bindingCount = 6;
  dsli.pBindings    = binds;
  VK_CHK(vkCreateDescriptorSetLayout(m_device, &dsli, nullptr, &m_dsLayout));

  // Push constant range
  VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PathTracePushConstants)};

  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount         = 1;
  plci.pSetLayouts            = &m_dsLayout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges    = &pcr;
  VK_CHK(vkCreatePipelineLayout(m_device, &plci, nullptr, &m_pipeLayout));

  VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = m_shaderMod;
  stage.pName  = "main";

  VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage  = stage;
  cpci.layout = m_pipeLayout;
  VK_CHK(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci,
                                   nullptr, &m_pipeline));
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Info, "vulkan",
      "Compute pipeline created from " + m_spvPath);
  return true;
}

bool VulkanGpuPathTracer::create_descriptors() {
  // Create descriptor pool on first call
  if (m_dsPool == VK_NULL_HANDLE) {
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 24};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets       = 8;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = &ps;
    VK_CHK(vkCreateDescriptorPool(m_device, &dpci, nullptr, &m_dsPool));
  }

  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool     = m_dsPool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts        = &m_dsLayout;
  VK_CHK(vkAllocateDescriptorSets(m_device, &ai, &m_ds));

  // Only bind if all buffers exist
  if (!m_filmBuf || !m_vertBuf) return true;

  const std::size_t szV  = m_gpuVerts.size()   * sizeof(float);
  const std::size_t szI  = m_gpuIdx.size()     * sizeof(uint32_t);
  const std::size_t szM  = m_gpuMats.size()    * sizeof(float);
  const std::size_t szIn = m_gpuInsts.size()   * sizeof(uint32_t);
  const std::size_t szL  = m_gpuLights.size()  * sizeof(float);
  const std::size_t szF  = static_cast<std::size_t>(m_filmPixels) * 4u * sizeof(float);

  const VkDescriptorBufferInfo dbi[6] = {
    {m_vertBuf, 0, szV},  {m_idxBuf, 0, szI},
    {m_matBuf,  0, szM},  {m_instBuf, 0, szIn},
    {m_ltBuf,   0, szL},  {m_filmBuf, 0, szF},
  };
  VkWriteDescriptorSet writes[6]{};
  for (uint32_t i = 0; i < 6; ++i) {
    writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet          = m_ds;
    writes[i].dstBinding      = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo     = &dbi[i];
  }
  vkUpdateDescriptorSets(m_device, 6, writes, 0, nullptr);
  return true;
}

uint32_t VulkanGpuPathTracer::find_mem_type(uint32_t bits,
                                            VkMemoryPropertyFlags flags) const {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(m_physDev, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((bits & (1u << i)) &&
        (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
  return UINT32_MAX;
}

bool VulkanGpuPathTracer::make_buffer(VkDeviceSize size,
                                      VkBufferUsageFlags usage,
                                      VkMemoryPropertyFlags props,
                                      VkBuffer& buf, VkDeviceMemory& mem) {
  if (size == 0) size = 4;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size        = size;
  bci.usage       = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VK_CHK(vkCreateBuffer(m_device, &bci, nullptr, &buf));

  VkMemoryRequirements mr{};
  vkGetBufferMemoryRequirements(m_device, buf, &mr);
  const uint32_t mt = find_mem_type(mr.memoryTypeBits, props);
  if (mt == UINT32_MAX) { m_error = "No suitable memory type"; return false; }

  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize  = mr.size;
  mai.memoryTypeIndex = mt;
  VK_CHK(vkAllocateMemory(m_device, &mai, nullptr, &mem));
  VK_CHK(vkBindBufferMemory(m_device, buf, mem, 0));
  return true;
}

void VulkanGpuPathTracer::destroy_film_buffers() {
  if (m_filmPtr != nullptr && m_filmMem != VK_NULL_HANDLE)
    vkUnmapMemory(m_device, m_filmMem);
  m_filmPtr = nullptr;
  if (m_filmBuf != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_filmBuf, nullptr); m_filmBuf = VK_NULL_HANDLE; }
  if (m_filmMem != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_filmMem, nullptr);    m_filmMem = VK_NULL_HANDLE; }
}

void VulkanGpuPathTracer::destroy_scene_buffers() {
  auto del = [&](VkBuffer& b, VkDeviceMemory& m) {
    if (b != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, b, nullptr); b = VK_NULL_HANDLE; }
    if (m != VK_NULL_HANDLE) { vkFreeMemory(m_device, m, nullptr);    m = VK_NULL_HANDLE; }
  };
  del(m_vertBuf, m_vertMem); del(m_idxBuf, m_idxMem);
  del(m_matBuf,  m_matMem);  del(m_instBuf, m_instMem);
  del(m_ltBuf,   m_ltMem);
  m_sceneUploaded = false;
}

void VulkanGpuPathTracer::destroy_pipeline() {
  if (m_pipeline   != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_pipeline, nullptr);          m_pipeline   = VK_NULL_HANDLE; }
  if (m_pipeLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_pipeLayout, nullptr);  m_pipeLayout = VK_NULL_HANDLE; }
  if (m_shaderMod  != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_shaderMod, nullptr);     m_shaderMod  = VK_NULL_HANDLE; }
}

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_VULKAN
