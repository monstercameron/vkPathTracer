#pragma once

#include "pathtracer/PathTracer.h"

namespace vkpt::cpu {

// RaySoA: 8-lane SoA ray packet used by the AVX2 intersection kernel.
struct RaySoA {
  __m256 ox, oy, oz;
  __m256 dx, dy, dz;
};

// 8-wide AVX2+FMA path tracer.
// Triangle intersection uses 8-wide Moller-Trumbore via AVX2.
// Path tracing logic (camera, RNG, shading, NEE) mirrors ScalarCpuPathTracer.
class Avx2CpuPathTracer final : public vkpt::pathtracer::IPathTracer {
 public:
  bool configure(const vkpt::pathtracer::RenderSettings& settings) override;
  bool load_scene_snapshot(const vkpt::pathtracer::RTSceneData& scene) override;
  bool build_or_update_acceleration() override;
  bool reset_accumulation() override;
  bool render_sample_batch(uint32_t start_y, uint32_t end_y,
                           uint32_t sample_index, uint32_t frame_index) override;
  vkpt::pathtracer::FilmLdr resolve_ldr() const override { return m_film.resolve_ldr(); }
  vkpt::pathtracer::FilmHdr resolve_hdr() const override { return m_film.resolve_hdr(); }
  vkpt::pathtracer::SampleCounters read_counters() const override;
  const vkpt::pathtracer::FilmBuffer& film() const override { return m_film; }
  void shutdown() override;

 private:
  void set_camera_basis();

  bool configured_ = false;
  bool has_scene_  = false;
  vkpt::pathtracer::RenderSettings settings_;
  vkpt::pathtracer::RTSceneData    scene_;
  vkpt::pathtracer::FilmBuffer     m_film;
  mutable vkpt::pathtracer::SampleCounters counters_{};

  vkpt::pathtracer::Vec3 cam_right_{};
  vkpt::pathtracer::Vec3 cam_up_{};
  vkpt::pathtracer::Vec3 cam_forward_{};
};

} // namespace vkpt::cpu
