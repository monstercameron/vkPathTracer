#pragma once

#include <immintrin.h>

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
  using vkpt::pathtracer::IPathTracer::configure;

  vkpt::core::Status configure(const vkpt::pathtracer::RenderSettings& settings) override;
  vkpt::core::Status load_scene_snapshot(const vkpt::pathtracer::PathTracerSceneSnapshot& scene) override;
  vkpt::core::Status build_or_update_acceleration() override;
  bool reset_accumulation() override;
  bool replace_film_history(const vkpt::pathtracer::FilmBuffer& film) override;
  bool update_camera(const vkpt::pathtracer::Vec3& pos,
                     const vkpt::pathtracer::Vec3& target,
                     const vkpt::pathtracer::Vec3& up,
                     float fov_deg) override;
  bool update_camera_state(const vkpt::pathtracer::RTCameraState& camera) override;
  bool update_instance_transforms(
      const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) override;
  bool update_scene_delta(const vkpt::pathtracer::RTSceneDeltaUpdate& update) override;
  bool render_tile(const vkpt::pathtracer::RenderTile& tile,
                   uint32_t frame_index) override;
  bool supports_tile_rendering() const override { return true; }
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
  vkpt::pathtracer::PathTracerSceneSnapshot    scene_;
  vkpt::pathtracer::FilmBuffer     m_film;
  mutable vkpt::pathtracer::SampleCounters counters_{};

  vkpt::pathtracer::Vec3 cam_right_{};
  vkpt::pathtracer::Vec3 cam_up_{};
  vkpt::pathtracer::Vec3 cam_forward_{};
};

} // namespace vkpt::cpu
