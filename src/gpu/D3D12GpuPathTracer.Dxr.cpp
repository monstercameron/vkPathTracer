#ifdef PT_ENABLE_D3D12
#include "gpu/D3D12GpuPathTracerInternal.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace vkpt::gpu {

bool D3D12GpuPathTracer::create_dxr_desc_heap() {
  // Descriptor order is the DXR shader ABI: TLAS is a root SRV, while all
  // remaining scene buffers and the film UAV live in this shader-visible heap.
  // 16 descriptors: slots 0-14 -> scene SRVs (t1-t15), slot 15 -> film UAV (u0)
  D3D12_DESCRIPTOR_HEAP_DESC dh{};
  dh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  dh.NumDescriptors = 16;
  dh.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  const HRESULT hr = m_device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&m_dxrDescHeap));
  if (FAILED(hr)) {
    m_error = "dxr CreateDescriptorHeap hr=" + FormatHr(hr);
    LogError("create_dxr_desc_heap: " + m_error);
    return false;
  }

  const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_dxrDescHeap->GetCPUDescriptorHandleForHeapStart();

  auto makeSrv = [&](UINT slot, ID3D12Resource* buf, UINT64 bytes, DXGI_FORMAT fmt) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = fmt;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements      = static_cast<UINT>(bytes / 4u);
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu;
    h.ptr += slot * inc;
    m_device->CreateShaderResourceView(buf, &srv, h);
  };
  makeSrv(0, m_vertBuf.Get(),   m_gpuVerts.size()  * sizeof(float),    DXGI_FORMAT_R32_FLOAT);
  makeSrv(1, m_idxBuf.Get(),    m_gpuIdx.size()    * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(2, m_matBuf.Get(),    m_gpuMats.size()   * sizeof(float),    DXGI_FORMAT_R32_FLOAT);
  makeSrv(3, m_instBuf.Get(),   m_gpuInsts.size()  * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(4, m_ltBuf.Get(),     m_gpuLights.size() * sizeof(float),    DXGI_FORMAT_R32_FLOAT);
  makeSrv(5, m_triMatBuf.Get(), m_gpuTriMat.size() * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(6, m_triDataBuf.Get(), m_gpuTriData.size() * sizeof(float),   DXGI_FORMAT_R32_FLOAT);
  makeSrv(7, m_texelBuf.Get(), m_gpuTexels.size() * sizeof(uint32_t),   DXGI_FORMAT_R32_UINT);
  makeSrv(8, m_texMetaBuf.Get(), m_gpuTextureMeta.size() * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(9, m_envBuf.Get(), m_gpuEnv.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(10, m_envMetaBuf.Get(), m_gpuEnvMeta.size() * sizeof(uint32_t), DXGI_FORMAT_R32_UINT);
  makeSrv(11, m_sdfBuf.Get(), m_gpuSdfs.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(12, m_bvhBuf.Get(), m_gpuBvh.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(13, m_dynamicBvhBuf.Get(), m_gpuDynamicBvh.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);
  makeSrv(14, m_localBvhBuf.Get(), m_gpuLocalBvh.size() * sizeof(float), DXGI_FORMAT_R32_FLOAT);

  const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = MakeRawBufferUavDesc(filmSize);
  D3D12_CPU_DESCRIPTOR_HANDLE h15 = cpu;
  h15.ptr += 15 * inc;
  m_device->CreateUnorderedAccessView(m_filmBuf.Get(), nullptr, &uav, h15);
  LogDebug("DXR descriptor heap created");
  return true;
}

bool D3D12GpuPathTracer::build_dxr_acceleration_structures() {
  if (!m_device5 || !m_cmdList || !m_vertBuf || !m_idxBuf || m_gpuVerts.empty() || m_gpuIdx.empty()) {
    m_error = "build_dxr_acceleration_structures: missing device5 or scene buffers";
    return false;
  }

  m_blasBuffer.Reset();
  m_blasScratch.Reset();
  m_tlasBuffer.Reset();
  m_tlasScratch.Reset();
  m_tlasInstanceBuf.Reset();
  m_dxrBlasBuffers.clear();
  m_dxrBlasScratch.clear();
  m_dxrInstanceDescs.clear();
  const uint32_t sceneInstanceCount = static_cast<uint32_t>(m_gpuInsts.size() / kGpuInstanceStrideU32);
  const std::size_t maxBlasCount =
      static_cast<std::size_t>(m_staticTriangleCount > 0u ? 1u : 0u) + sceneInstanceCount;
  m_dxrBlasBuffers.reserve(maxBlasCount);
  m_dxrBlasScratch.reserve(maxBlasCount);
  m_dxrInstanceDescs.reserve(maxBlasCount);

  auto createBuffer = [&](UINT64 bytes,
                          D3D12_HEAP_TYPE heapType,
                          D3D12_RESOURCE_FLAGS flags,
                          D3D12_RESOURCE_STATES initialState,
                          Microsoft::WRL::ComPtr<ID3D12Resource>& out) -> bool {
    bytes = std::max<UINT64>(bytes, 4ull);
    const D3D12_HEAP_PROPERTIES hp = MakeHeapProperties(heapType);
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    const HRESULT hr = m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, initialState, nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr)) {
      m_error = "DXR buffer create hr=" + FormatHr(hr);
      return false;
    }
    return true;
  };

  // BLAS inputs read the existing default-heap scene buffers. This avoids a
  // second CPU copy into upload heaps and lets the AS builder consume GPU-local memory.
  if (!wait_for_gpu()) {
    LogError("build_dxr_acceleration_structures: " + m_error);
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> buildList;
  if (FAILED(m_cmdList->QueryInterface(IID_PPV_ARGS(&buildList)))) {
    m_error = "build_dxr_acceleration_structures: compute command list is not ID3D12GraphicsCommandList4";
    LogError(m_error);
    return false;
  }
  if (FAILED(m_cmdAllocator->Reset()) ||
      FAILED(buildList->Reset(m_cmdAllocator.Get(), nullptr))) {
    m_error = "build_dxr_acceleration_structures: command reset failed";
    LogError(m_error);
    return false;
  }

  auto buildBlasRange = [&](uint32_t firstTriangle,
                            uint32_t triangleCount,
                            D3D12_GPU_VIRTUAL_ADDRESS& blasAddress) -> bool {
    // Static geometry can share one BLAS; dynamic instances get individual
    // BLAS objects so TLAS refits only need updated instance transforms.
    const uint64_t firstIndex = static_cast<uint64_t>(firstTriangle) * 3ull;
    const uint64_t indexCount = static_cast<uint64_t>(triangleCount) * 3ull;
    const uint64_t gpuIndexCount = static_cast<uint64_t>(m_gpuIdx.size());
    if (triangleCount == 0u || firstIndex > gpuIndexCount || indexCount > gpuIndexCount - firstIndex) {
      m_error = "build_dxr_acceleration_structures: invalid BLAS triangle range";
      return false;
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geomDesc{};
    geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geomDesc.Triangles.VertexBuffer.StartAddress = m_vertBuf->GetGPUVirtualAddress();
    geomDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3u;
    geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geomDesc.Triangles.VertexCount = static_cast<UINT>(m_gpuVerts.size() / 3u);
    geomDesc.Triangles.IndexBuffer = m_idxBuf->GetGPUVirtualAddress() + firstIndex * sizeof(uint32_t);
    geomDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geomDesc.Triangles.IndexCount = static_cast<UINT>(indexCount);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = DxrBuildPreferenceFlags(m_dxrBuildMode);
    inputs.NumDescs = 1u;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = &geomDesc;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0u) {
      m_error = "build_dxr_acceleration_structures: empty BLAS prebuild result";
      return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> blas;
    Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
    if (!createBuffer(info.ResultDataMaxSizeInBytes,
                      D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                      blas) ||
        !createBuffer(info.ScratchDataSizeInBytes,
                      D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      scratch)) {
      return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
    buildDesc.Inputs = inputs;
    buildDesc.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    buildList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    blasAddress = blas->GetGPUVirtualAddress();
    m_dxrBlasBuffers.push_back(blas);
    m_dxrBlasScratch.push_back(scratch);
    return true;
  };

  D3D12_GPU_VIRTUAL_ADDRESS staticBlas = 0u;
  if (m_staticTriangleCount > 0u) {
    if (!buildBlasRange(0u, m_staticTriangleCount, staticBlas)) {
      LogError("build_dxr_acceleration_structures: static BLAS failed: " + m_error);
      return false;
    }
    m_dxrInstanceDescs.push_back(MakeDxrInstanceDesc(
        kDxrStaticInstanceId,
        staticBlas,
        {0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 1.0f}));
  }

  for (uint32_t instanceIndex = 0u; instanceIndex < sceneInstanceCount; ++instanceIndex) {
    const std::size_t ib = static_cast<std::size_t>(instanceIndex) * kGpuInstanceStrideU32;
    const uint32_t firstTriangle = m_gpuInsts[ib + 0u];
    const uint32_t triangleCount = m_gpuInsts[ib + 1u];
    const uint32_t flags = m_gpuInsts[ib + 3u];
    if ((flags & vkpt::pathtracer::kRTInstanceFlagDynamicTransform) == 0u || triangleCount == 0u) {
      continue;
    }
    D3D12_GPU_VIRTUAL_ADDRESS blasAddress = 0u;
    if (!buildBlasRange(firstTriangle, triangleCount, blasAddress)) {
      LogError("build_dxr_acceleration_structures: dynamic BLAS failed: " + m_error);
      return false;
    }
    const vkpt::pathtracer::Vec3 translation{
        UintBitsToFloat(m_gpuInsts[ib + 4u]),
        UintBitsToFloat(m_gpuInsts[ib + 5u]),
        UintBitsToFloat(m_gpuInsts[ib + 6u])};
    const vkpt::pathtracer::Quat4 rotation{
        UintBitsToFloat(m_gpuInsts[ib + 8u]),
        UintBitsToFloat(m_gpuInsts[ib + 9u]),
        UintBitsToFloat(m_gpuInsts[ib + 10u]),
        UintBitsToFloat(m_gpuInsts[ib + 11u])};
    const vkpt::pathtracer::Vec3 scale{
        UintBitsToFloat(m_gpuInsts[ib + 12u]),
        UintBitsToFloat(m_gpuInsts[ib + 13u]),
        UintBitsToFloat(m_gpuInsts[ib + 14u])};
    m_dxrInstanceDescs.push_back(
        MakeDxrInstanceDesc(instanceIndex, blasAddress, translation, rotation, scale));
  }

  if (m_dxrInstanceDescs.empty()) {
    m_error = "build_dxr_acceleration_structures: no TLAS instances";
    LogError(m_error);
    return false;
  }
  D3D12_RESOURCE_BARRIER blasBarrier{};
  blasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  blasBarrier.UAV.pResource = nullptr;
  buildList->ResourceBarrier(1, &blasBarrier);

  m_blasBuffer = m_dxrBlasBuffers.front();
  m_blasScratch = m_dxrBlasScratch.front();

  const UINT64 instanceBytes =
      static_cast<UINT64>(m_dxrInstanceDescs.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
  if (!createBuffer(instanceBytes,
                    D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    m_tlasInstanceBuf)) {
    LogError("build_dxr_acceleration_structures: " + m_error);
    return false;
  }
  void* instancePtr = nullptr;
  if (FAILED(m_tlasInstanceBuf->Map(0, nullptr, &instancePtr))) {
    m_error = "build_dxr_acceleration_structures: TLAS instance map failed";
    return false;
  }
  std::memcpy(instancePtr, m_dxrInstanceDescs.data(), static_cast<std::size_t>(instanceBytes));
  m_tlasInstanceBuf->Unmap(0, nullptr);

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
  tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  tlasInputs.Flags = AddDxrBuildFlags(
      DxrBuildPreferenceFlags(m_dxrBuildMode),
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
  tlasInputs.NumDescs = static_cast<UINT>(m_dxrInstanceDescs.size());
  tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  tlasInputs.InstanceDescs = m_tlasInstanceBuf->GetGPUVirtualAddress();

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo{};
  m_device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);
  const UINT64 tlasScratchBytes =
      std::max(tlasInfo.ScratchDataSizeInBytes, tlasInfo.UpdateScratchDataSizeInBytes);
  if (tlasInfo.ResultDataMaxSizeInBytes == 0u ||
      !createBuffer(tlasInfo.ResultDataMaxSizeInBytes,
                    D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                    m_tlasBuffer) ||
      !createBuffer(tlasScratchBytes,
                    D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    m_tlasScratch)) {
    LogError("build_dxr_acceleration_structures: TLAS alloc failed: " + m_error);
    return false;
  }

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc{};
  tlasDesc.Inputs = tlasInputs;
  tlasDesc.DestAccelerationStructureData = m_tlasBuffer->GetGPUVirtualAddress();
  tlasDesc.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();
  buildList->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);

  D3D12_RESOURCE_BARRIER tlasBarrier{};
  tlasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  tlasBarrier.UAV.pResource = m_tlasBuffer.Get();
  buildList->ResourceBarrier(1, &tlasBarrier);

  if (FAILED(buildList->Close())) {
    m_error = "build_dxr_acceleration_structures: command close failed";
    LogError(m_error);
    return false;
  }
  ID3D12CommandList* lists[] = {buildList.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    LogError("build_dxr_acceleration_structures: " + m_error);
    return false;
  }
  const HRESULT removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during DXR AS build hr=" + FormatHr(removeHr);
    LogError("build_dxr_acceleration_structures: " + m_error);
    return false;
  }

  m_dxrDescHeap.Reset();
  if (!create_dxr_desc_heap()) {
    return false;
  }
  m_dxrAccelReady = true;
  LogInfo("DXR BLAS/TLAS built: blas=" + std::to_string(m_dxrBlasBuffers.size()) +
          " tlas_instances=" + std::to_string(m_dxrInstanceDescs.size()) +
          " dynamic=" + std::to_string(m_dxrInstanceDescs.size() - (m_staticTriangleCount > 0u ? 1u : 0u)));
  return true;
}

bool D3D12GpuPathTracer::update_dxr_instance_buffer_and_tlas() {
  return update_dxr_instance_buffer_and_tlas_from(m_gpuInsts, m_dxrInstanceDescs);
}

bool D3D12GpuPathTracer::update_dxr_instance_buffer_and_tlas_from(
    const std::vector<uint32_t>& gpuInstances,
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& dxrInstanceDescs) {
  if (!m_device5 || !m_cmdList || !m_tlasBuffer || !m_tlasScratch ||
      !m_tlasInstanceBuf || dxrInstanceDescs.empty()) {
    m_error = "update_dxr_instance_buffer_and_tlas: DXR TLAS resources are not ready";
    LogError(m_error);
    return false;
  }
  if (gpuInstances.size() < m_sceneData.instances.size() * kGpuInstanceStrideU32) {
    m_error = "update_dxr_instance_buffer_and_tlas: instance buffer is incomplete";
    LogError(m_error);
    return false;
  }

  // Only instance transforms change here. BLAS geometry addresses remain stable,
  // so the TLAS can be updated in place with PERFORM_UPDATE.
  for (auto& desc : dxrInstanceDescs) {
    if (desc.InstanceID == kDxrStaticInstanceId) {
      continue;
    }
    const uint32_t instanceIndex = desc.InstanceID;
    if (instanceIndex >= m_sceneData.instances.size()) {
      continue;
    }
    const std::size_t ib = static_cast<std::size_t>(instanceIndex) * kGpuInstanceStrideU32;
    const vkpt::pathtracer::Vec3 translation{
        UintBitsToFloat(gpuInstances[ib + 4u]),
        UintBitsToFloat(gpuInstances[ib + 5u]),
        UintBitsToFloat(gpuInstances[ib + 6u])};
    const vkpt::pathtracer::Quat4 rotation{
        UintBitsToFloat(gpuInstances[ib + 8u]),
        UintBitsToFloat(gpuInstances[ib + 9u]),
        UintBitsToFloat(gpuInstances[ib + 10u]),
        UintBitsToFloat(gpuInstances[ib + 11u])};
    const vkpt::pathtracer::Vec3 scale{
        UintBitsToFloat(gpuInstances[ib + 12u]),
        UintBitsToFloat(gpuInstances[ib + 13u]),
        UintBitsToFloat(gpuInstances[ib + 14u])};
    const auto blasAddress = desc.AccelerationStructure;
    desc = MakeDxrInstanceDesc(instanceIndex, blasAddress, translation, rotation, scale);
  }

  const UINT64 instanceBytes =
      static_cast<UINT64>(dxrInstanceDescs.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
  void* instancePtr = nullptr;
  if (FAILED(m_tlasInstanceBuf->Map(0, nullptr, &instancePtr))) {
    m_error = "update_dxr_instance_buffer_and_tlas: TLAS instance map failed";
    LogError(m_error);
    return false;
  }
  std::memcpy(instancePtr, dxrInstanceDescs.data(), static_cast<std::size_t>(instanceBytes));
  m_tlasInstanceBuf->Unmap(0, nullptr);

  if (!wait_for_gpu()) {
    LogError("update_dxr_instance_buffer_and_tlas: " + m_error);
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cl4;
  if (FAILED(m_cmdList->QueryInterface(IID_PPV_ARGS(&cl4)))) {
    m_error = "update_dxr_instance_buffer_and_tlas: compute command list is not ID3D12GraphicsCommandList4";
    LogError(m_error);
    return false;
  }
  if (FAILED(m_cmdAllocator->Reset()) ||
      FAILED(cl4->Reset(m_cmdAllocator.Get(), nullptr))) {
    m_error = "update_dxr_instance_buffer_and_tlas: command reset failed";
    LogError(m_error);
    return false;
  }

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
  tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  tlasInputs.Flags = AddDxrBuildFlags(
      AddDxrBuildFlags(DxrBuildPreferenceFlags(m_dxrBuildMode),
                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE),
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
  tlasInputs.NumDescs = static_cast<UINT>(dxrInstanceDescs.size());
  tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  tlasInputs.InstanceDescs = m_tlasInstanceBuf->GetGPUVirtualAddress();

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC updateDesc{};
  updateDesc.Inputs = tlasInputs;
  updateDesc.SourceAccelerationStructureData = m_tlasBuffer->GetGPUVirtualAddress();
  updateDesc.DestAccelerationStructureData = m_tlasBuffer->GetGPUVirtualAddress();
  updateDesc.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();
  cl4->BuildRaytracingAccelerationStructure(&updateDesc, 0, nullptr);

  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = m_tlasBuffer.Get();
  cl4->ResourceBarrier(1, &barrier);

  if (FAILED(cl4->Close())) {
    m_error = "update_dxr_instance_buffer_and_tlas: command close failed";
    LogError(m_error);
    return false;
  }
  ID3D12CommandList* lists[] = {cl4.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    LogError("update_dxr_instance_buffer_and_tlas: " + m_error);
    return false;
  }
  const HRESULT removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during DXR TLAS update hr=" + FormatHr(removeHr);
    LogError("update_dxr_instance_buffer_and_tlas: " + m_error);
    return false;
  }
  return true;
}

bool D3D12GpuPathTracer::dispatch_dxr_rays(uint32_t sample_idx, uint32_t frame_idx, bool doReadback) {
  // Use the compute queue's command list (same queue as BLAS build) to avoid
  // any cross-queue memory visibility issues with the acceleration structures.
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cl4;
  if (FAILED(m_cmdList->QueryInterface(IID_PPV_ARGS(&cl4)))) {
    m_error = "dispatch_dxr_rays: CL4 QI failed — DXR not supported on compute list";
    LogError(m_error);
    return false;
  }
  if (FAILED(m_cmdAllocator->Reset()) ||
      FAILED(cl4->Reset(m_cmdAllocator.Get(), nullptr))) {
    m_error = "dispatch_dxr_rays: cmd list reset failed";
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }

  // Build PathTraceConstants (identical layout to compute path)
  const auto& sc = m_sceneData;
  auto norm3 = [](float x, float y, float z, float* out) {
    float l = std::sqrt(x*x + y*y + z*z);
    if (l < 1e-9f) l = 1.0f;
    out[0]=x/l; out[1]=y/l; out[2]=z/l;
  };
  float fwd[3]; norm3(sc.camera_target.x - sc.camera_position.x,
      sc.camera_target.y - sc.camera_position.y,
      sc.camera_target.z - sc.camera_position.z, fwd);
  float rt[3]  = {fwd[1]*sc.camera_up.z - fwd[2]*sc.camera_up.y,
                  fwd[2]*sc.camera_up.x - fwd[0]*sc.camera_up.z,
                  fwd[0]*sc.camera_up.y - fwd[1]*sc.camera_up.x};
  float rn[3]; norm3(rt[0], rt[1], rt[2], rn);
  float un[3]; norm3(rn[1]*fwd[2]-rn[2]*fwd[1],
      rn[2]*fwd[0]-rn[0]*fwd[2], rn[0]*fwd[1]-rn[1]*fwd[0], un);

  PathTraceConstants pc{};
  pc.camera_pos_x = sc.camera_position.x;
  pc.camera_pos_y = sc.camera_position.y;
  pc.camera_pos_z = sc.camera_position.z;
  pc.fov_tan_half = std::tan(0.5f * sc.camera_fov_deg * 3.14159265f / 180.0f);
  pc.cam_fwd_x=fwd[0]; pc.cam_fwd_y=fwd[1]; pc.cam_fwd_z=fwd[2];
  pc.aspect    = static_cast<float>(m_settings.width) /
                 std::max(1.0f, static_cast<float>(m_settings.height));
  pc.cam_right_x=rn[0]; pc.cam_right_y=rn[1]; pc.cam_right_z=rn[2];
  pc.num_sdfs = static_cast<uint32_t>(sc.sdf_primitives.size());
  pc.cam_up_x=un[0]; pc.cam_up_y=un[1]; pc.cam_up_z=un[2];
  pc.sample_index = sample_idx;
  pc.num_insts    = static_cast<uint32_t>(sc.instances.size());
  pc.num_mats     = static_cast<uint32_t>(sc.materials.size());
  pc.num_lights   = static_cast<uint32_t>(sc.lights.size());
  pc.width        = m_settings.width;
  pc.height       = m_settings.height;
  pc.base_seed    = static_cast<uint32_t>(m_settings.seed & 0xFFFFFFFFu)
                    ^ (frame_idx * 2654435761u);
  pc.env_r = sc.environment_color.x;
  pc.env_g = sc.environment_color.y;
  pc.env_b = sc.environment_color.z;
  pc.max_depth_f  = static_cast<float>(std::max(1u, m_settings.max_depth));
  pc.aperture_radius = sc.camera_aperture_radius > 0.0f
      ? sc.camera_aperture_radius
      : m_settings.camera_aperture_radius;
  pc.focus_distance = sc.camera_focus_distance > 0.0f
      ? sc.camera_focus_distance
      : m_settings.camera_focus_distance;
  pc.iris_blade_count = std::min(sc.camera_iris_blade_count, 64u);
  pc.iris_rotation_radians = sc.camera_iris_rotation_degrees * (3.14159265f / 180.0f);
  pc.iris_roundness = std::clamp(sc.camera_iris_roundness, 0.0f, 1.0f);
  pc.anamorphic_squeeze = std::isfinite(sc.camera_anamorphic_squeeze)
      ? std::max(0.01f, sc.camera_anamorphic_squeeze)
      : 1.0f;
  const auto resolveSettings =
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_sceneData);
  const auto whiteBalance = vkpt::pathtracer::WhiteBalanceScale(resolveSettings.white_balance_kelvin);
  pc.rays_per_pixel = std::max(1u, m_raysPerPixelPerDispatch);
  pc.exposure = resolveSettings.exposure;
  pc.tone_map = static_cast<uint32_t>(resolveSettings.tone_map);
  pc.output_transform = static_cast<uint32_t>(resolveSettings.output_transform);
  pc.gamma = resolveSettings.gamma;
  pc.clamp_output = resolveSettings.clamp_output ? 1u : 0u;
  pc.white_balance_r = whiteBalance.x;
  pc.white_balance_g = whiteBalance.y;
  pc.white_balance_b = whiteBalance.z;
  const bool doDenoise = doReadback && m_settings.enable_denoiser;
  const bool doTemporal = doReadback && m_settings.enable_temporal_aa;
  const bool doGuide = doDenoise || doTemporal;
  if (!m_settings.enable_temporal_aa) {
    m_temporalHistoryValid = false;
  }
  pc.denoiser_enabled = doDenoise ? 1u : 0u;
  pc.denoiser_strength = doDenoise ? 1.0f : 0.0f;
  pc.denoiser_color_sigma = 0.22f;
  pc.temporal_enabled = doTemporal ? 1u : 0u;
  pc.temporal_history_valid = (doTemporal && m_temporalHistoryValid) ? 1u : 0u;
  pc.temporal_feedback = 0.92f;
  pc.temporal_depth_sigma = 0.05f;
  pc.temporal_normal_power = 28.0f;
  pc.temporal_color_margin = 0.12f;
  pc.static_bvh_node_count = (m_staticTriangleCount > 0u)
      ? static_cast<uint32_t>(m_gpuBvh.size() / 8u)
      : 0u;
  pc.dynamic_bvh_node_count = (m_dynamicInstanceCount > 0u)
      ? static_cast<uint32_t>(m_gpuDynamicBvh.size() / 8u)
      : 0u;
  FillPreviousCameraConstants(pc, m_temporalHistoryValid ? m_temporalPrevCamera : MakeTemporalCameraState(pc));
  if (doReadback && (!m_ldrBuf || !m_ldrReadbackBuf || !m_ldrReadbackPtr || !ensure_compute_srv_uav_heap())) {
    m_error = "DXR LDR readback resources unavailable";
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }
  if (doDenoise && (!m_guidePso || !m_denoisePso || !m_guideBuf || !m_denoiseBuf)) {
    m_error = "DXR GPU denoiser resources unavailable";
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }
  if (doTemporal && (!m_guidePso || !m_temporalPso || !m_guideBuf ||
                     !m_temporalBuf || !m_temporalHistoryBuf || !m_prevGuideBuf)) {
    m_error = "DXR GPU temporal AA resources unavailable";
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }

  // Set the DXR global root signature for DispatchRays. Postprocess dispatches
  // below switch back to the compute root signature before running.
  cl4->SetComputeRootSignature(m_dxrGlobalRootSig.Get());
  std::memcpy(m_uploadPtr, &pc, sizeof(pc));
  cl4->SetComputeRootConstantBufferView(0, m_uploadBuf->GetGPUVirtualAddress());
  cl4->SetComputeRootShaderResourceView(1, m_tlasBuffer->GetGPUVirtualAddress());
  ID3D12DescriptorHeap* heaps[] = {m_dxrDescHeap.Get()};
  cl4->SetDescriptorHeaps(1, heaps);
  cl4->SetComputeRootDescriptorTable(2, m_dxrDescHeap->GetGPUDescriptorHandleForHeapStart());

  // Transition film to UAV for DXR accumulation.
  D3D12_RESOURCE_BARRIER filmBarrier{};
  filmBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  filmBarrier.Transition.pResource   = m_filmBuf.Get();
  filmBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  filmBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  filmBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  cl4->ResourceBarrier(1, &filmBarrier);

  // Set pipeline state and dispatch
  cl4->SetPipelineState1(m_rtPso.Get());

  // Log dispatch constants once when verbose D3D12 diagnostics are enabled.
  if (D3D12VerboseLoggingEnabled()) {
    static std::atomic_bool s_logged{false};
    if (!s_logged.exchange(true, std::memory_order_acq_rel)) {
      std::ostringstream dc;
      dc << "dispatch_dxr_rays constants: env=(" << pc.env_r << "," << pc.env_g << "," << pc.env_b
         << ") maxDepth=" << pc.max_depth_f << " rpp=" << pc.rays_per_pixel
         << " camPos=(" << pc.camera_pos_x << "," << pc.camera_pos_y << "," << pc.camera_pos_z << ")"
         << " camFwd=(" << pc.cam_fwd_x << "," << pc.cam_fwd_y << "," << pc.cam_fwd_z << ")"
         << " w=" << pc.width << " h=" << pc.height;
      LogDebug(dc.str());
    }
  }

  constexpr UINT kSbtAlign = 64; // D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT
  const D3D12_GPU_VIRTUAL_ADDRESS sbtGpu = m_sbtBuffer->GetGPUVirtualAddress();
  D3D12_DISPATCH_RAYS_DESC drd{};
  drd.RayGenerationShaderRecord = {sbtGpu + 0,          kSbtAlign};
  drd.MissShaderTable           = {sbtGpu + kSbtAlign,  kSbtAlign, kSbtAlign};
  drd.HitGroupTable             = {sbtGpu + kSbtAlign*2, kSbtAlign, kSbtAlign};
  drd.Width  = m_settings.width;
  drd.Height = m_settings.height;
  drd.Depth  = 1;
  constexpr wchar_t kDxrEvent[] = L"D3D12 DXR DispatchRays";
  cl4->BeginEvent(0, kDxrEvent, sizeof(kDxrEvent));
  cl4->DispatchRays(&drd);
  cl4->EndEvent();

  if (doReadback) {
    // The hardware raygen writes the same HDR film buffer as the compute path.
    // Readback samples then reuse compute PSOs for guide, temporal, denoise,
    // tonemap, and LDR copy-out so both render paths produce identical output.
    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_filmBuf.Get();
    cl4->ResourceBarrier(1, &uavBarrier);

    D3D12_RESOURCE_BARRIER ldrBarrier{};
    ldrBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ldrBarrier.Transition.pResource   = m_ldrBuf.Get();
    ldrBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    ldrBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ldrBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl4->ResourceBarrier(1, &ldrBarrier);

    D3D12_RESOURCE_BARRIER denoiseBarrier = MakeTransitionBarrier(
        m_denoiseBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER guideBarrier = MakeTransitionBarrier(
        m_guideBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER temporalBarrier = MakeTransitionBarrier(
        m_temporalBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER temporalHistoryBarrier = MakeTransitionBarrier(
        m_temporalHistoryBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER prevGuideBarrier = MakeTransitionBarrier(
        m_prevGuideBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (doDenoise || doGuide || doTemporal) {
      std::array<D3D12_RESOURCE_BARRIER, 5> startBarriers{};
      UINT barrierCount = 0u;
      if (doDenoise) startBarriers[barrierCount++] = denoiseBarrier;
      if (doGuide) startBarriers[barrierCount++] = guideBarrier;
      if (doTemporal) {
        startBarriers[barrierCount++] = temporalBarrier;
        startBarriers[barrierCount++] = temporalHistoryBarrier;
        startBarriers[barrierCount++] = prevGuideBarrier;
      }
      if (barrierCount > 0u) {
        cl4->ResourceBarrier(barrierCount, startBarriers.data());
      }
    }

    ID3D12DescriptorHeap* tonemapHeaps[] = {m_srvUavHeap.Get()};
    cl4->SetDescriptorHeaps(1, tonemapHeaps);
    cl4->SetComputeRootSignature(m_rootSig.Get());
    cl4->SetComputeRootDescriptorTable(1, m_srvUavHeap->GetGPUDescriptorHandleForHeapStart());
    cl4->SetComputeRootConstantBufferView(0, m_uploadBuf->GetGPUVirtualAddress());
    const UINT groupsX = (m_settings.width + 7u) / 8u;
    const UINT groupsY = (m_settings.height + 7u) / 8u;
    if (doGuide) {
      cl4->SetPipelineState(m_guidePso.Get());
      constexpr wchar_t kDxrGuideEvent[] = L"D3D12 DXR Guide Dispatch";
      cl4->BeginEvent(0, kDxrGuideEvent, sizeof(kDxrGuideEvent));
      cl4->Dispatch(groupsX, groupsY, 1u);
      cl4->EndEvent();

      uavBarrier.UAV.pResource = m_guideBuf.Get();
      cl4->ResourceBarrier(1, &uavBarrier);
    }

    if (doTemporal) {
      cl4->SetPipelineState(m_temporalPso.Get());
      constexpr wchar_t kDxrTemporalEvent[] = L"D3D12 DXR Temporal AA Dispatch";
      cl4->BeginEvent(0, kDxrTemporalEvent, sizeof(kDxrTemporalEvent));
      cl4->Dispatch(groupsX, groupsY, 1u);
      cl4->EndEvent();

      uavBarrier.UAV.pResource = m_temporalBuf.Get();
      cl4->ResourceBarrier(1, &uavBarrier);
    }

    if (doDenoise) {
      cl4->SetPipelineState(m_denoisePso.Get());
      constexpr wchar_t kDxrDenoiseEvent[] = L"D3D12 DXR Denoise Dispatch";
      cl4->BeginEvent(0, kDxrDenoiseEvent, sizeof(kDxrDenoiseEvent));
      cl4->Dispatch(groupsX, groupsY, 1u);
      cl4->EndEvent();

      uavBarrier.UAV.pResource = m_denoiseBuf.Get();
      cl4->ResourceBarrier(1, &uavBarrier);
    }
    cl4->SetPipelineState(m_tonemapPso.Get());
    constexpr wchar_t kDxrTonemapEvent[] = L"D3D12 DXR Tonemap Dispatch";
    cl4->BeginEvent(0, kDxrTonemapEvent, sizeof(kDxrTonemapEvent));
    cl4->Dispatch(groupsX, groupsY, 1u);
    cl4->EndEvent();

    uavBarrier.UAV.pResource = m_ldrBuf.Get();
    cl4->ResourceBarrier(1, &uavBarrier);

    if (doTemporal) {
      // Keep history copies GPU-local and state-transition them around the copy
      // because the next frame samples them as UAV/SRV-style raw buffers.
      const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
      const UINT64 guideSize = static_cast<UINT64>(m_filmPixels) * 8u * sizeof(float);
      std::array<D3D12_RESOURCE_BARRIER, 4> copyStartBarriers = {
          MakeTransitionBarrier(m_temporalBuf.Get(),
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
          MakeTransitionBarrier(m_temporalHistoryBuf.Get(),
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
          MakeTransitionBarrier(m_guideBuf.Get(),
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
          MakeTransitionBarrier(m_prevGuideBuf.Get(),
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)};
      cl4->ResourceBarrier(static_cast<UINT>(copyStartBarriers.size()), copyStartBarriers.data());
      cl4->CopyBufferRegion(m_temporalHistoryBuf.Get(), 0, m_temporalBuf.Get(), 0, filmSize);
      cl4->CopyBufferRegion(m_prevGuideBuf.Get(), 0, m_guideBuf.Get(), 0, guideSize);

      std::array<D3D12_RESOURCE_BARRIER, 4> copyEndBarriers = {
          MakeTransitionBarrier(m_temporalBuf.Get(),
              D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
          MakeTransitionBarrier(m_temporalHistoryBuf.Get(),
              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
          MakeTransitionBarrier(m_guideBuf.Get(),
              D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
          MakeTransitionBarrier(m_prevGuideBuf.Get(),
              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)};
      cl4->ResourceBarrier(static_cast<UINT>(copyEndBarriers.size()), copyEndBarriers.data());
    }

    filmBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    filmBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    ldrBarrier.Transition.StateBefore  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ldrBarrier.Transition.StateAfter   = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (doDenoise || (doGuide && !doTemporal)) {
      std::array<D3D12_RESOURCE_BARRIER, 4> readbackBarriers{};
      UINT barrierCount = 0u;
      readbackBarriers[barrierCount++] = filmBarrier;
      readbackBarriers[barrierCount++] = ldrBarrier;
      denoiseBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      denoiseBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      guideBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      guideBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      if (doDenoise) readbackBarriers[barrierCount++] = denoiseBarrier;
      if (doGuide && !doTemporal) readbackBarriers[barrierCount++] = guideBarrier;
      cl4->ResourceBarrier(barrierCount, readbackBarriers.data());
    } else {
      D3D12_RESOURCE_BARRIER readbackBarriers[2] = {filmBarrier, ldrBarrier};
      cl4->ResourceBarrier(2, readbackBarriers);
    }

    const UINT64 ldrSize = static_cast<UINT64>(m_filmPixels) * sizeof(uint32_t);
    cl4->CopyBufferRegion(m_ldrReadbackBuf.Get(), 0, m_ldrBuf.Get(), 0, ldrSize);

    ldrBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ldrBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    cl4->ResourceBarrier(1, &ldrBarrier);
  } else {
    filmBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    filmBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    cl4->ResourceBarrier(1, &filmBarrier);
  }

  const HRESULT closeHr = cl4->Close();
  if (FAILED(closeHr)) {
    m_error = "DXR dispatch command list close hr=" + FormatHr(closeHr);
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }
  ID3D12CommandList* lists[] = {cl4.Get()};
  m_cmdQueue->ExecuteCommandLists(1, lists);
  if (!wait_for_gpu()) {
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }

  // Drain any D3D12 InfoQueue messages only during verbose diagnostics.
  if (D3D12VerboseLoggingEnabled()) {
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> iq;
    if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&iq)))) {
      static std::atomic_bool s_iqLogged{false};
      if (!s_iqLogged.exchange(true, std::memory_order_acq_rel)) {
        const UINT64 n = iq->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
          SIZE_T len = 0;
          iq->GetMessage(i, nullptr, &len);
          if (len < sizeof(D3D12_MESSAGE)) {
            continue;
          }
          std::vector<std::max_align_t> storage(
              (len + sizeof(std::max_align_t) - 1u) / sizeof(std::max_align_t));
          auto* msg = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
          if (SUCCEEDED(iq->GetMessage(i, msg, &len))) {
            std::string sev = (msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR)   ? "ERROR"   :
                              (msg->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ? "WARNING" : "INFO";
            LogInfo("[D3D12] " + sev + ": " + std::string(msg->pDescription, msg->DescriptionByteLength));
          }
        }
        if (n == 0) LogInfo("[D3D12 InfoQueue] No messages stored");
      }
    }
  }
  const auto removeHr = m_device->GetDeviceRemovedReason();
  if (FAILED(removeHr)) {
    m_error = "device removed during DXR dispatch hr=" + FormatHr(removeHr);
    LogError("dispatch_dxr_rays: " + m_error);
    return false;
  }
  if (doTemporal) {
    m_temporalHistoryValid = true;
    m_temporalPrevCamera = MakeTemporalCameraState(pc);
  }

  if (doReadback) {
    m_ldrResolve.width  = m_settings.width;
    m_ldrResolve.height = m_settings.height;
    m_ldrResolve.rgba8.resize(static_cast<size_t>(m_filmPixels) * 4u);
    std::memcpy(m_ldrResolve.rgba8.data(), m_ldrReadbackPtr,
                static_cast<size_t>(m_filmPixels) * 4u);
  }
  const uint64_t rpp = static_cast<uint64_t>(std::max(1u, m_raysPerPixelPerDispatch));
  const uint64_t inc = static_cast<uint64_t>(m_settings.width) * m_settings.height * rpp;
  const uint64_t raysPerSample =
      EstimateLogicalRaysPerD3D12Sample(m_settings, m_sceneData, m_usingDxrDispatch);
  m_counters.samples += inc;
  m_counters.rays    += SaturatingMulU64(inc, raysPerSample);
  m_lastSampleIdx     = sample_idx * m_raysPerPixelPerDispatch + (m_raysPerPixelPerDispatch - 1u);
  return true;
}

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
