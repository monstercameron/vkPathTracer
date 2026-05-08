#ifdef PT_ENABLE_D3D12
#include "gpu/D3D12GpuPathTracerInternal.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace vkpt::gpu {

void D3D12GpuPathTracer::destroy_dxr_resources() {
  // The SBT is persistently mapped while the DXR pipeline is alive; unmap it
  // before releasing the upload resource to keep teardown order explicit.
  if (m_sbtMappedPtr && m_sbtBuffer) { m_sbtBuffer->Unmap(0, nullptr); m_sbtMappedPtr = nullptr; }
  m_rtPsoProps.Reset();    m_rtPso.Reset();
  m_sbtBuffer.Reset();     m_dxrDescHeap.Reset();
  m_dxrGlobalRootSig.Reset();
  m_blasBuffer.Reset();    m_blasScratch.Reset();
  m_dxrBlasBuffers.clear();
  m_dxrBlasScratch.clear();
  m_dxrInstanceDescs.clear();
  m_tlasBuffer.Reset();    m_tlasScratch.Reset();
  m_tlasInstanceBuf.Reset();
  if (m_dxrFenceEvent) { CloseHandle(m_dxrFenceEvent); m_dxrFenceEvent = nullptr; }
  m_dxrFence.Reset();
  m_dxrAccelReady    = false;
  m_dxrPipelineReady = false;
}

bool D3D12GpuPathTracer::compile_dxil(const std::string& path, std::vector<uint8_t>& outDxil) {
  // Load dxil.dll FIRST (for signing), then dxcompiler.dll
  static HMODULE s_dxcLib = nullptr;
  if (!s_dxcLib) {
    const wchar_t* kSdkDir =
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64\\";
    // Pre-load dxil.dll so DXC can find it for DXIL signing
    wchar_t dxilPath[MAX_PATH]{};
    wcscpy_s(dxilPath, kSdkDir); wcscat_s(dxilPath, L"dxil.dll");
    HMODULE dxilMod = LoadLibraryW(dxilPath);
    if (!dxilMod) {
      // Try bare name (might be in PATH)
      dxilMod = LoadLibraryW(L"dxil.dll");
    }
    if (dxilMod) LogDebug("compile_dxil: dxil.dll loaded for signing");
    else          LogInfo("compile_dxil: dxil.dll not found — DXIL will be unsigned");

    // Load dxcompiler.dll
    wchar_t dxcPath[MAX_PATH]{};
    wcscpy_s(dxcPath, kSdkDir); wcscat_s(dxcPath, L"dxcompiler.dll");
    s_dxcLib = LoadLibraryW(dxcPath);
    if (!s_dxcLib) s_dxcLib = LoadLibraryW(L"dxcompiler.dll");
    if (!s_dxcLib) {
      m_error = "dxcompiler.dll not found";
      LogError("compile_dxil: " + m_error);
      return false;
    }
  }

  typedef HRESULT (__stdcall *PFN_DxcCreate)(REFCLSID, REFIID, LPVOID*);
  auto dxcCreate = (PFN_DxcCreate)GetProcAddress(s_dxcLib, "DxcCreateInstance");
  if (!dxcCreate) {
    m_error = "DxcCreateInstance not in dxcompiler.dll";
    LogError("compile_dxil: " + m_error);
    return false;
  }

  Microsoft::WRL::ComPtr<IDxcUtils>     utils;
  Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
  if (FAILED(dxcCreate(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
      FAILED(dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
    m_error = "DXC utils/compiler create failed";
    LogError("compile_dxil: " + m_error);
    return false;
  }

  Microsoft::WRL::ComPtr<IDxcBlobEncoding> sourceBlob;
  std::wstring wpath(path.begin(), path.end());
  if (FAILED(utils->LoadFile(wpath.c_str(), nullptr, &sourceBlob))) {
    m_error = "DXC LoadFile failed: " + path;
    LogError("compile_dxil: " + m_error);
    return false;
  }

  DxcBuffer src{};
  src.Ptr      = sourceBlob->GetBufferPointer();
  src.Size     = sourceBlob->GetBufferSize();
  src.Encoding = DXC_CP_ACP;

  // Runtime defines keep the DXR shader feature set aligned with benchmark
  // toggles without requiring multiple checked-in DXIL variants.
  const bool dxrShadowRays = ParseEnvBool("PT_D3D12_DXR_SHADOW_RAYS", true);
  const wchar_t* shadowRayDefine =
      dxrShadowRays ? L"-DPT_D3D12_DXR_SHADOW_RAYS=1" : L"-DPT_D3D12_DXR_SHADOW_RAYS=0";
  LPCWSTR args[] = { L"-T", L"lib_6_3", L"-HV", L"2021", L"-O3", shadowRayDefine };
  Microsoft::WRL::ComPtr<IDxcResult> result;
  HRESULT hr = compiler->Compile(&src, args, ARRAYSIZE(args), nullptr, IID_PPV_ARGS(&result));

  Microsoft::WRL::ComPtr<IDxcBlobUtf8> errors;
  if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr))
      && errors && errors->GetStringLength() > 0) {
    LogError("DXC errors:\n" + std::string(errors->GetStringPointer(), errors->GetStringLength()));
  }

  HRESULT status = E_FAIL;
  if (result) result->GetStatus(&status);
  if (FAILED(hr) || FAILED(status)) {
    m_error = "DXC compile failed for " + path;
    LogError("compile_dxil: " + m_error);
    return false;
  }

  Microsoft::WRL::ComPtr<IDxcBlob> dxilBlob;
  if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxilBlob), nullptr))
      || !dxilBlob || dxilBlob->GetBufferSize() == 0) {
    m_error = "DXC produced empty DXIL blob";
    LogError("compile_dxil: " + m_error);
    return false;
  }

  outDxil.resize(dxilBlob->GetBufferSize());
  std::memcpy(outDxil.data(), dxilBlob->GetBufferPointer(), outDxil.size());
  LogInfo("DXR shader compiled " + std::to_string(outDxil.size()) + " bytes from " + path +
          " hardware_shadow_rays=" + (dxrShadowRays ? "true" : "false") +
          " shader_shadow_occlusion=" + (dxrShadowRays ? "false" : "true"));
  return true;
}

bool D3D12GpuPathTracer::create_dxr_global_root_sig() {
  // Root bindings mirror pathtrace_rt.hlsl: constants, TLAS root SRV, then
  // the descriptor table for scene buffers and the HDR film UAV.
  // Param 0: constants CBV (PCBuf) at b0
  // Param 1: root SRV at t0 (TLAS)
  // Param 2: descriptor table [t1-t15 SRV, u0 UAV]
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0;
  params[0].Descriptor.RegisterSpace  = 0;
  params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType                = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[1].Descriptor.ShaderRegister   = 0;   // t0
  params[1].Descriptor.RegisterSpace    = 0;
  params[1].ShaderVisibility            = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 15;         // t1-t15
  ranges[0].BaseShaderRegister = 1;
  ranges[0].RegisterSpace = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;          // u0
  ranges[1].BaseShaderRegister = 0;
  ranges[1].RegisterSpace = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = 15;
  params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 2;
  params[2].DescriptorTable.pDescriptorRanges   = ranges;
  params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rsd{};
  rsd.NumParameters = 3;
  rsd.pParameters   = params;
  rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
  HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
  if (FAILED(hr)) {
    m_error = "dxr SerializeRootSignature";
    if (err) LogError(static_cast<const char*>(err->GetBufferPointer()));
    return false;
  }
  const HRESULT createHr = m_device->CreateRootSignature(0, sig->GetBufferPointer(),
      sig->GetBufferSize(), IID_PPV_ARGS(&m_dxrGlobalRootSig));
  if (FAILED(createHr)) {
    m_error = "dxr CreateRootSignature hr=" + FormatHr(createHr);
    LogError("create_dxr_global_root_sig: " + m_error);
    return false;
  }
  LogDebug("DXR global root sig created");
  return true;
}

bool D3D12GpuPathTracer::create_dxr_pipeline() {
  if (!create_dxr_global_root_sig()) return false;

  // The raygen shader owns the path loop, so TraceRay recursion depth stays at
  // one unless hardware shadow rays are compiled in. In that mode the closest-hit
  // shader traces a secondary visibility ray, which requires one nested level.
  const bool dxrShadowRays = ParseEnvBool("PT_D3D12_DXR_SHADOW_RAYS", true);

  // Compile DXIL library
  std::vector<uint8_t> dxil;
  if (!compile_dxil(m_rtHlslPath, dxil)) return false;

  // Subobject array: library, hitgroup, shaderconfig, pipelineconfig, globalrootsig
  constexpr UINT kNumSubobjects = 5;
  D3D12_STATE_SUBOBJECT subobjects[kNumSubobjects]{};
  UINT si = 0;

  // 1. DXIL library
  D3D12_EXPORT_DESC exports[4]{};
  exports[0] = {L"RayGen",    nullptr, D3D12_EXPORT_FLAG_NONE};
  exports[1] = {L"Miss",      nullptr, D3D12_EXPORT_FLAG_NONE};
  exports[2] = {L"ClosestHit",nullptr, D3D12_EXPORT_FLAG_NONE};
  exports[3] = {L"AnyHit",    nullptr, D3D12_EXPORT_FLAG_NONE};
  D3D12_DXIL_LIBRARY_DESC libDesc{};
  libDesc.DXILLibrary = {dxil.data(), dxil.size()};
  libDesc.NumExports  = 4;
  libDesc.pExports    = exports;
  subobjects[si++] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc};

  // 2. Hit group
  D3D12_HIT_GROUP_DESC hitGroup{};
  hitGroup.HitGroupExport          = L"HitGroup0";
  hitGroup.Type                    = D3D12_HIT_GROUP_TYPE_TRIANGLES;
  hitGroup.ClosestHitShaderImport  = L"ClosestHit";
  hitGroup.AnyHitShaderImport      = L"AnyHit";
  hitGroup.IntersectionShaderImport= nullptr;
  subobjects[si++] = {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroup};

  // 3. Shader config (payload + attribute sizes)
  D3D12_RAYTRACING_SHADER_CONFIG shaderCfg{};
  shaderCfg.MaxPayloadSizeInBytes   = 56; // sizeof(PathPayload) in pathtrace_rt.hlsl
  shaderCfg.MaxAttributeSizeInBytes = 8;  // float2 barycentrics
  subobjects[si++] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderCfg};

  // 4. Pipeline config (raygen primary trace, plus optional closest-hit shadow trace)
  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineCfg{};
  pipelineCfg.MaxTraceRecursionDepth = dxrShadowRays ? 2u : 1u;
  subobjects[si++] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineCfg};

  // 5. Global root signature
  D3D12_GLOBAL_ROOT_SIGNATURE globalRootSig{};
  globalRootSig.pGlobalRootSignature = m_dxrGlobalRootSig.Get();
  subobjects[si++] = {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRootSig};

  D3D12_STATE_OBJECT_DESC soDesc{};
  soDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
  soDesc.NumSubobjects = si;
  soDesc.pSubobjects   = subobjects;

  const HRESULT soHr = m_device5->CreateStateObject(&soDesc, IID_PPV_ARGS(&m_rtPso));
  if (FAILED(soHr)) {
    m_error = "CreateStateObject (DXR pipeline) hr=" + FormatHr(soHr);
    LogError("create_dxr_pipeline: " + m_error);
    return false;
  }
  const HRESULT propsHr = m_rtPso->QueryInterface(IID_PPV_ARGS(&m_rtPsoProps));
  if (FAILED(propsHr)) {
    m_error = "QueryInterface ID3D12StateObjectProperties hr=" + FormatHr(propsHr);
    LogError("create_dxr_pipeline: " + m_error);
    return false;
  }

  // Build shader binding table (3 x 64-byte records: raygen, miss, hitgroup).
  // Records contain only identifiers because all resources are in the global root.
  constexpr UINT kSbtRecordSize  = 32; // D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES
  constexpr UINT kSbtRecordAlign = 64; // D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT
  constexpr UINT kSbtTotalBytes  = kSbtRecordAlign * 3; // raygen + miss + hitgroup

  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC   rd{};
  rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width            = kSbtTotalBytes;
  rd.Height           = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels        = 1;
  rd.Format           = DXGI_FORMAT_UNKNOWN;
  rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.SampleDesc.Count = 1;

  const HRESULT sbtHr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_sbtBuffer));
  if (FAILED(sbtHr)) {
    m_error = "SBT CreateCommittedResource hr=" + FormatHr(sbtHr);
    LogError("create_dxr_pipeline: " + m_error);
    return false;
  }
  const HRESULT mapSbtHr = m_sbtBuffer->Map(0, nullptr, &m_sbtMappedPtr);
  if (FAILED(mapSbtHr) || m_sbtMappedPtr == nullptr) {
    m_error = "SBT map hr=" + FormatHr(mapSbtHr);
    LogError("create_dxr_pipeline: " + m_error);
    m_sbtMappedPtr = nullptr;
    m_sbtBuffer.Reset();
    return false;
  }
  std::memset(m_sbtMappedPtr, 0, kSbtTotalBytes);

  auto* sbtBytes = static_cast<uint8_t*>(m_sbtMappedPtr);
  void* rayGenId  = m_rtPsoProps->GetShaderIdentifier(L"RayGen");
  void* missId    = m_rtPsoProps->GetShaderIdentifier(L"Miss");
  void* hitId     = m_rtPsoProps->GetShaderIdentifier(L"HitGroup0");
  if (!rayGenId || !missId || !hitId) {
    m_error = "GetShaderIdentifier returned null — names may not match PSO exports";
    LogError("create_dxr_pipeline: " + m_error);
    m_sbtBuffer->Unmap(0, nullptr);
    m_sbtMappedPtr = nullptr;
    m_sbtBuffer.Reset();
    return false;
  }
  std::memcpy(sbtBytes + 0,              rayGenId, kSbtRecordSize);
  std::memcpy(sbtBytes + kSbtRecordAlign, missId,  kSbtRecordSize);
  std::memcpy(sbtBytes + kSbtRecordAlign * 2, hitId, kSbtRecordSize);

  m_dxrPipelineReady = true;
  LogInfo("DXR pipeline ready; SBT written (" + std::to_string(kSbtTotalBytes) + " bytes)");
  return true;
}

}  // namespace vkpt::gpu
#endif  // PT_ENABLE_D3D12
