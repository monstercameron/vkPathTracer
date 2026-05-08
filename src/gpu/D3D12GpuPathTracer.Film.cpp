#ifdef PT_ENABLE_D3D12

#include "gpu/D3D12GpuPathTracerInternal.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

namespace vkpt::gpu {

bool D3D12GpuPathTracer::reset_accumulation() {
  if (!m_configured || !m_filmBuf) return false;
  if (m_filmPixels == 0u) {
    m_error = "film pixel count is zero";
    return false;
  }

  // The D3D12 backend accumulates in the GPU UAV (m_filmBuf); the shader
  // overwrites accumulator slots when it sees sample_index == 0, so there is
  // no CPU-side clear to perform. m_film is a vestigial CPU FilmBuffer that
  // resolve_hdr() returns; nothing in the D3D12 path writes to it, so clearing
  // it would memset tens of megabytes of unused buffers per drag tick.
  m_counters = {};
  m_temporalHistoryValid = false;
  m_lastSampleIdx = 0u;
  m_latestFilmReadbackToken = {};
  ++m_filmGeneration;
  m_ldrResolveGeneration = 0u;
  LogDebug("reset_accumulation complete");
  debug_check_state_contract("reset_accumulation");
  return true;
}

vkpt::core::Status D3D12GpuPathTracer::reset_accumulation_status() {
  const bool ok = reset_accumulation();
  return GpuBackendOperationStatus(
      "d3d12.reset_accumulation",
      ok,
      m_error,
      vkpt::core::StatusCode::NotReady);
}

bool D3D12GpuPathTracer::should_readback_sample(uint32_t sample_idx) const {
  if (m_fastMotionSamplesRemaining > 0u) {
    return true;
  }
  if (m_forceReadbackEverySample) {
    return true;
  }
  const bool finiteSpp = m_settings.spp != std::numeric_limits<uint32_t>::max();
  if (finiteSpp) {
    return sample_idx + 1u >= m_settings.spp;
  }
  const uint32_t interval = std::max(1u, m_readbackInterval);
  return sample_idx < 4u || (sample_idx % interval) == 0u;
}

vkpt::pathtracer::FilmLdr D3D12GpuPathTracer::resolve_ldr() const {
  // The GPU tonemap pass already produced an RGBA8 result during render_tile().
  // Return it directly — no CPU tonemapping, no box filter, no per-pixel work.
  if (!m_ldrResolve.rgba8.empty()) {
    return m_ldrResolve;
  }
  // Fallback: if no GPU frame has been produced yet, return blank.
  vkpt::pathtracer::FilmLdr blank;
  blank.width  = m_settings.width;
  blank.height = m_settings.height;
  blank.rgba8.assign(static_cast<size_t>(m_filmPixels) * 4u, 0u);
  return blank;
}

vkpt::pathtracer::FilmReadbackToken D3D12GpuPathTracer::request_film_readback() {
  if (!m_valid || !m_configured || m_ldrResolve.rgba8.empty() ||
      m_ldrResolveGeneration != m_filmGeneration) {
    return {};
  }
  vkpt::pathtracer::FilmReadbackToken token;
  token.id = m_nextFilmReadbackId++;
  token.fence_value = static_cast<std::uint64_t>(m_fenceValue);
  token.width = m_ldrResolve.width;
  token.height = m_ldrResolve.height;
  m_latestFilmReadbackToken = token;
  return token;
}

vkpt::pathtracer::FilmReadbackResult D3D12GpuPathTracer::poll_film(
    vkpt::pathtracer::FilmReadbackToken token) {
  if (!token || token.id != m_latestFilmReadbackToken.id ||
      token.width != m_latestFilmReadbackToken.width ||
      token.height != m_latestFilmReadbackToken.height) {
    return {
        vkpt::pathtracer::FilmReadbackState::Invalid,
        {},
        "invalid D3D12 film readback token"};
  }
  if (!m_valid || !m_configured) {
    return {
        vkpt::pathtracer::FilmReadbackState::Failed,
        {},
        "D3D12 film readback requested before backend is ready"};
  }
  if (m_fence && m_fence->GetCompletedValue() < token.fence_value) {
    return {vkpt::pathtracer::FilmReadbackState::Pending, {}, {}};
  }
  if (m_ldrResolve.rgba8.empty()) {
    return {
        vkpt::pathtracer::FilmReadbackState::Failed,
        {},
        "D3D12 film readback has no completed LDR snapshot"};
  }
  return {
      vkpt::pathtracer::FilmReadbackState::Ready,
      m_ldrResolve,
      {}};
}

vkpt::pathtracer::FilmHdr D3D12GpuPathTracer::resolve_hdr() const {
  return m_film.resolve_hdr();
}
vkpt::pathtracer::SampleCounters D3D12GpuPathTracer::read_counters() const {
  return m_counters;
}

bool D3D12GpuPathTracer::create_film_buffer() {
  const UINT64 filmSize = static_cast<UINT64>(m_filmPixels) * 4u * sizeof(float);
  if (filmSize == 0u) {
    m_error = "film size is zero";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  LogDebug("create_film_buffer pixels=" + std::to_string(m_filmPixels) + " bytes=" + std::to_string(filmSize));

  // Default heap (GPU-visible UAV)
  const D3D12_HEAP_PROPERTIES defhp = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC   rd{};
  rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width            = filmSize;
  rd.Height           = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels        = 1;
  rd.Format           = DXGI_FORMAT_UNKNOWN;
  rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd.SampleDesc.Count = 1;
  rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const HRESULT createFilmHr = m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &rd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_filmBuf));
  if (FAILED(createFilmHr)) {
    m_error = "film buf hr=" + FormatHr(createFilmHr);
    LogError("create_film_buffer: " + m_error);
    return false;
  }

  // Readback buffer
  const D3D12_HEAP_PROPERTIES rdhp = MakeHeapProperties(D3D12_HEAP_TYPE_READBACK);
  D3D12_RESOURCE_DESC   rd2{};
  rd2.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd2.Width            = filmSize;
  rd2.Height           = 1;
  rd2.DepthOrArraySize = 1;
  rd2.MipLevels        = 1;
  rd2.Format           = DXGI_FORMAT_UNKNOWN;
  rd2.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  rd2.SampleDesc.Count = 1;
  const HRESULT createReadbackHr = m_device->CreateCommittedResource(&rdhp, D3D12_HEAP_FLAG_NONE, &rd2,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_filmReadbackBuf));
  if (FAILED(createReadbackHr)) {
    m_error = "film readback buf hr=" + FormatHr(createReadbackHr);
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  const HRESULT mapReadbackHr = m_filmReadbackBuf->Map(0, nullptr, &m_filmReadbackPtr);
  if (FAILED(mapReadbackHr)) {
    m_error = "film readback map hr=" + FormatHr(mapReadbackHr);
    LogError("create_film_buffer: " + m_error);
    return false;
  }

  // ---- LDR output buffer: GPU-side RGBA8 (one uint per pixel) ----------------
  const UINT64 ldrSize = static_cast<UINT64>(m_filmPixels) * sizeof(uint32_t);
  D3D12_RESOURCE_DESC ldrRd{};
  ldrRd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  ldrRd.Width            = ldrSize;
  ldrRd.Height           = 1;
  ldrRd.DepthOrArraySize = 1;
  ldrRd.MipLevels        = 1;
  ldrRd.Format           = DXGI_FORMAT_UNKNOWN;
  ldrRd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ldrRd.SampleDesc.Count = 1;
  ldrRd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &ldrRd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_ldrBuf)))) {
    m_error = "ldr buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }

  // LDR readback buffer (CPU-visible, 4 bytes/pixel — 4× smaller than RGBA32F)
  D3D12_RESOURCE_DESC ldrRb{};
  ldrRb.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
  ldrRb.Width            = ldrSize;
  ldrRb.Height           = 1;
  ldrRb.DepthOrArraySize = 1;
  ldrRb.MipLevels        = 1;
  ldrRb.Format           = DXGI_FORMAT_UNKNOWN;
  ldrRb.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ldrRb.SampleDesc.Count = 1;
  if (FAILED(m_device->CreateCommittedResource(&rdhp, D3D12_HEAP_FLAG_NONE, &ldrRb,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_ldrReadbackBuf)))) {
    m_error = "ldr readback buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  if (FAILED(m_ldrReadbackBuf->Map(0, nullptr, &m_ldrReadbackPtr))) {
    m_error = "ldr readback map failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }

  // ---- HDR denoise buffers stay entirely on the GPU -------------------------
  D3D12_RESOURCE_DESC denoiseRd = rd;
  denoiseRd.Width = filmSize;
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &denoiseRd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_denoiseBuf)))) {
    m_error = "denoise buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }

  D3D12_RESOURCE_DESC guideRd = rd;
  const UINT64 guideSize = static_cast<UINT64>(m_filmPixels) * 8u * sizeof(float);
  guideRd.Width = guideSize;
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &guideRd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_guideBuf)))) {
    m_error = "denoise guide buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }

  D3D12_RESOURCE_DESC temporalRd = rd;
  temporalRd.Width = filmSize;
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &temporalRd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_temporalBuf)))) {
    m_error = "temporal buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &temporalRd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_temporalHistoryBuf)))) {
    m_error = "temporal history buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  if (FAILED(m_device->CreateCommittedResource(&defhp, D3D12_HEAP_FLAG_NONE, &guideRd,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_prevGuideBuf)))) {
    m_error = "previous guide buf create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  m_temporalHistoryValid = false;

  // ---- Persistent clear heap for reset_accumulation (avoids per-frame alloc) -
  D3D12_DESCRIPTOR_HEAP_DESC chd{};
  chd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  chd.NumDescriptors = 1;
  chd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(m_device->CreateDescriptorHeap(&chd, IID_PPV_ARGS(&m_clearHeap)))) {
    m_error = "clear heap create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC cpuChd = chd;
  cpuChd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  if (FAILED(m_device->CreateDescriptorHeap(&cpuChd, IID_PPV_ARGS(&m_clearCpuHeap)))) {
    m_error = "clear CPU heap create failed";
    LogError("create_film_buffer: " + m_error);
    return false;
  }
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC clearUav = MakeRawBufferUavDesc(filmSize);
    m_device->CreateUnorderedAccessView(m_filmBuf.Get(), nullptr, &clearUav,
        m_clearHeap->GetCPUDescriptorHandleForHeapStart());
    m_device->CreateUnorderedAccessView(m_filmBuf.Get(), nullptr, &clearUav,
        m_clearCpuHeap->GetCPUDescriptorHandleForHeapStart());
  }

  return true;
}

void D3D12GpuPathTracer::destroy_film_buffer() {
  if (m_filmReadbackPtr && m_filmReadbackBuf) m_filmReadbackBuf->Unmap(0, nullptr);
  m_filmReadbackPtr = nullptr;
  m_filmReadbackBuf.Reset();
  m_filmBuf.Reset();
  if (m_ldrReadbackPtr && m_ldrReadbackBuf) m_ldrReadbackBuf->Unmap(0, nullptr);
  m_ldrReadbackPtr = nullptr;
  m_ldrReadbackBuf.Reset();
  m_ldrBuf.Reset();
  m_denoiseBuf.Reset();
  m_guideBuf.Reset();
  m_temporalBuf.Reset();
  m_temporalHistoryBuf.Reset();
  m_prevGuideBuf.Reset();
  m_temporalHistoryValid = false;
  m_latestFilmReadbackToken = {};
  ++m_filmGeneration;
  m_ldrResolveGeneration = 0u;
  m_clearHeap.Reset();
  m_clearCpuHeap.Reset();
  m_srvUavHeap.Reset();
}

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
