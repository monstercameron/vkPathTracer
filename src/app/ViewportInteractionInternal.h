#pragma once

#ifdef PT_ENABLE_QT

#include "app/ViewportInteraction.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vkpt::app {

struct ProjectedViewportPoint {
  float x = 0.0f;
  float y = 0.0f;
  float depth = 0.0f;
};

inline constexpr std::array<std::pair<int, int>, 12> kViewportBoundsEdges{{
    {0, 1},
    {1, 3},
    {3, 2},
    {2, 0},
    {4, 5},
    {5, 7},
    {7, 6},
    {6, 4},
    {0, 4},
    {1, 5},
    {2, 6},
    {3, 7},
}};

vkpt::pathtracer::Vec3
TransformPointForPreview(const vkpt::scene::Vec3 &point,
                         const vkpt::scene::TransformComponent &transform);

bool IntersectBounds(const ViewportRay &ray, const vkpt::editor::Bounds &bounds,
                     float &t_near);
bool IntersectFrontFacingTriangle(const ViewportRay &ray,
                                  const ViewportPickable::Triangle &triangle,
                                  float maxDistance, float &t_out);
bool IntersectTriangleDoubleSided(const ViewportRay &ray,
                                  const ViewportPickable::Triangle &triangle,
                                  float maxDistance, float &t_out,
                                  vkpt::pathtracer::Vec3 &normal_out);
bool BoundsOverlapsAabb(const vkpt::editor::Bounds &bounds,
                        const vkpt::pathtracer::Vec3 &queryMin,
                        const vkpt::pathtracer::Vec3 &queryMax);
bool TriangleOverlapsAabb(const ViewportPickable::Triangle &triangle,
                          const vkpt::pathtracer::Vec3 &queryMin,
                          const vkpt::pathtracer::Vec3 &queryMax);

std::vector<vkpt::pathtracer::Vec3>
BoundsCorners(const vkpt::editor::Bounds &bounds);
std::optional<ProjectedViewportPoint>
ProjectWorldPointToOverlay(const vkpt::pathtracer::Vec3 &point,
                           const ViewportCameraPose &camera, float width,
                           float height, float renderAspect);
void AddProjectedOverlayLine(vkpt::platform::QtSelectionOverlayBox &box,
                             const ProjectedViewportPoint &a,
                             const ProjectedViewportPoint &b,
                             OverlayColor color, float lineWidth);
void AddSelectionGizmo(vkpt::platform::QtSelectionOverlayBox &box,
                       const vkpt::editor::Bounds &bounds,
                       const ViewportCameraPose &camera, float width,
                       float height, float renderAspect,
                       vkpt::editor::GizmoMode mode,
                       const std::optional<ViewportGizmoHit> &hover);

} // namespace vkpt::app

#endif
