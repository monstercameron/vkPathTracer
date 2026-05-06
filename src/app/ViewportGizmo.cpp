#include "app/ViewportInteractionInternal.h"

#ifdef PT_ENABLE_QT

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace vkpt::app {

void AddGizmoCornerArc(vkpt::platform::QtSelectionOverlayBox &box,
                       const ViewportCameraPose &camera, float width,
                       float height, float renderAspect,
                       const vkpt::pathtracer::Vec3 &corner,
                       const vkpt::pathtracer::Vec3 &axisA,
                       const vkpt::pathtracer::Vec3 &axisB, float radius,
                       OverlayColor color, float lineWidth = 1.0f) {
  if (radius <= 1.0e-4f) {
    return;
  }
  constexpr int kArcSegments = 14;
  constexpr float kHalfPi = 1.57079632679489661923f;
  auto previous = ProjectedViewportPoint{};
  bool previousValid = false;
  for (int segment = 0; segment <= kArcSegments; ++segment) {
    const float t =
        (static_cast<float>(segment) / static_cast<float>(kArcSegments)) *
        kHalfPi;
    const auto world = PtAdd(corner, PtAdd(PtMul(axisA, std::cos(t) * radius),
                                           PtMul(axisB, std::sin(t) * radius)));
    const auto projected =
        ProjectWorldPointToOverlay(world, camera, width, height, renderAspect);
    if (projected && previousValid) {
      AddProjectedOverlayLine(box, previous, *projected, color, lineWidth);
    }
    if (projected) {
      previous = *projected;
      previousValid = true;
    } else {
      previousValid = false;
    }
  }
}

vkpt::platform::QtViewportCursor
CursorForGizmoHit(const ViewportGizmoHit &hit) {
  switch (hit.kind) {
  case ViewportGizmoDragKind::Translate:
  case ViewportGizmoDragKind::FreeformTranslate:
    return vkpt::platform::QtViewportCursor::Translate;
  case ViewportGizmoDragKind::Rotate:
    return vkpt::platform::QtViewportCursor::Rotate;
  case ViewportGizmoDragKind::ScaleAxis:
    return vkpt::platform::QtViewportCursor::Scale;
  case ViewportGizmoDragKind::None:
  default:
    return vkpt::platform::QtViewportCursor::Default;
  }
}

float ScreenDistance(float ax, float ay, float bx, float by) {
  const float dx = ax - bx;
  const float dy = ay - by;
  return std::sqrt(dx * dx + dy * dy);
}

float ScreenDistanceToSegment(float px, float py,
                              const ProjectedViewportPoint &a,
                              const ProjectedViewportPoint &b,
                              float *tangentX = nullptr,
                              float *tangentY = nullptr) {
  const float vx = b.x - a.x;
  const float vy = b.y - a.y;
  const float lenSq = vx * vx + vy * vy;
  if (lenSq <= 1.0e-6f) {
    if (tangentX != nullptr) {
      *tangentX = 1.0f;
    }
    if (tangentY != nullptr) {
      *tangentY = 0.0f;
    }
    return ScreenDistance(px, py, a.x, a.y);
  }
  const float t =
      ClampFloat(((px - a.x) * vx + (py - a.y) * vy) / lenSq, 0.0f, 1.0f);
  const float closestX = a.x + vx * t;
  const float closestY = a.y + vy * t;
  const float len = std::sqrt(lenSq);
  if (tangentX != nullptr) {
    *tangentX = vx / len;
  }
  if (tangentY != nullptr) {
    *tangentY = vy / len;
  }
  return ScreenDistance(px, py, closestX, closestY);
}

bool SameGizmoHandle(const std::optional<ViewportGizmoHit> &a,
                     const std::optional<ViewportGizmoHit> &b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  if (!a) {
    return true;
  }
  return a->kind == b->kind && a->axis_index == b->axis_index;
}

bool IsHoveredGizmoHandle(const std::optional<ViewportGizmoHit> &hover,
                          ViewportGizmoDragKind kind, int axisIndex) {
  return hover && hover->kind == kind && hover->axis_index == axisIndex;
}

std::optional<vkpt::pathtracer::Vec3>
ScreenPointOnCameraPlane(const ViewportCameraPose &camera, float x, float y,
                         float width, float height, float renderAspect,
                         const vkpt::pathtracer::Vec3 &planePoint) {
  const auto ray = BuildViewportRay(camera, x, y, width, height, renderAspect);
  const auto forward = PtNormalize(PtSub(camera.target, camera.position));
  const float denom = PtDot(ray.direction, forward);
  if (std::fabs(denom) <= 1.0e-5f) {
    return std::nullopt;
  }
  const float t = PtDot(PtSub(planePoint, ray.origin), forward) / denom;
  if (t <= 1.0e-5f) {
    return std::nullopt;
  }
  return PtAdd(ray.origin, PtMul(ray.direction, t));
}

std::optional<ViewportGizmoHit> PickSelectionGizmoHandle(
    const vkpt::editor::Bounds &bounds, const ViewportCameraPose &camera,
    float width, float height, float renderAspect, vkpt::editor::GizmoMode mode,
    float mouseX, float mouseY) {
  if (!bounds.valid || mode == vkpt::editor::GizmoMode::None) {
    return std::nullopt;
  }
  const auto min = ToPtVec3(bounds.min);
  const auto max = ToPtVec3(bounds.max);
  const auto center = PtMul(PtAdd(min, max), 0.5f);
  const float extentX = std::fabs(max.x - min.x);
  const float extentY = std::fabs(max.y - min.y);
  const float extentZ = std::fabs(max.z - min.z);
  const float maxExtent = std::max({extentX, extentY, extentZ});
  if (maxExtent <= 1.0e-5f) {
    return std::nullopt;
  }

  const auto corners = BoundsCorners(bounds);
  std::size_t anchorIndex = 0u;
  float nearestDepth = std::numeric_limits<float>::infinity();
  // Use the nearest projected corner as the handle origin so the overlay stays
  // visible and hit targets do not hide behind the selected bounds.
  for (std::size_t i = 0; i < corners.size(); ++i) {
    const auto projected = ProjectWorldPointToOverlay(corners[i], camera, width,
                                                      height, renderAspect);
    if (projected && projected->depth < nearestDepth) {
      nearestDepth = projected->depth;
      anchorIndex = i;
    }
  }
  const auto anchor = corners[anchorIndex];
  const auto projectedAnchor =
      ProjectWorldPointToOverlay(anchor, camera, width, height, renderAspect);
  if (!projectedAnchor) {
    return std::nullopt;
  }

  const bool anchorMinX =
      std::fabs(anchor.x - min.x) <= std::fabs(anchor.x - max.x);
  const bool anchorMinY =
      std::fabs(anchor.y - min.y) <= std::fabs(anchor.y - max.y);
  const bool anchorMinZ =
      std::fabs(anchor.z - min.z) <= std::fabs(anchor.z - max.z);
  struct CornerAxis {
    int axis_index = -1;
    vkpt::pathtracer::Vec3 axis{};
    vkpt::pathtracer::Vec3 endpoint{};
    float length = 0.0f;
  };
  const std::array<CornerAxis, 3> axes{{
      {0,
       {anchorMinX ? 1.0f : -1.0f, 0.0f, 0.0f},
       {anchorMinX ? max.x : min.x, anchor.y, anchor.z},
       extentX},
      {1,
       {0.0f, anchorMinY ? 1.0f : -1.0f, 0.0f},
       {anchor.x, anchorMinY ? max.y : min.y, anchor.z},
       extentY},
      {2,
       {0.0f, 0.0f, anchorMinZ ? 1.0f : -1.0f},
       {anchor.x, anchor.y, anchorMinZ ? max.z : min.z},
       extentZ},
  }};
  const auto tickLength = [maxExtent](float axisExtent) {
    if (axisExtent <= 1.0e-5f) {
      return 0.0f;
    }
    return std::max(0.025f, std::min(axisExtent * 0.35f, maxExtent * 0.18f));
  };
  const float xLength = tickLength(extentX);
  const float yLength = tickLength(extentY);
  const float zLength = tickLength(extentZ);

  std::optional<ViewportGizmoHit> best;
  float bestDistance = std::numeric_limits<float>::infinity();
  int bestPriority = -1;
  constexpr float kTranslateHitRadius = 11.0f;
  constexpr float kRotateHitRadius = 10.0f;
  constexpr float kScaleHitRadius = 12.0f;
  const auto accept_hit = [&](float distance, int priority,
                              ViewportGizmoHit hit) {
    // Different handle types can overlap in screen space; priority keeps small
    // scale endpoints and rotate arcs usable without requiring pixel-perfect
    // mouse placement.
    constexpr float kTieBreakPixels = 0.75f;
    if (distance + kTieBreakPixels < bestDistance ||
        (std::fabs(distance - bestDistance) <= kTieBreakPixels &&
         priority > bestPriority)) {
      bestDistance = distance;
      bestPriority = priority;
      best = hit;
    }
  };

  const bool drawTranslate = mode == vkpt::editor::GizmoMode::Translate ||
                             mode == vkpt::editor::GizmoMode::Universal;
  const bool drawRotate = mode == vkpt::editor::GizmoMode::Rotate ||
                          mode == vkpt::editor::GizmoMode::Universal;
  const bool drawScale = mode == vkpt::editor::GizmoMode::Scale ||
                         mode == vkpt::editor::GizmoMode::Universal;

  const auto consider_axis_line = [&](const CornerAxis &axis) {
    if (!drawTranslate || axis.length <= 1.0e-5f) {
      return;
    }
    const auto projectedEnd = ProjectWorldPointToOverlay(
        axis.endpoint, camera, width, height, renderAspect);
    if (!projectedEnd) {
      return;
    }
    float tangentX = 1.0f;
    float tangentY = 0.0f;
    const float distance = ScreenDistanceToSegment(
        mouseX, mouseY, *projectedAnchor, *projectedEnd, &tangentX, &tangentY);
    if (distance > kTranslateHitRadius) {
      return;
    }
    const float screenPixels =
        ScreenDistance(projectedAnchor->x, projectedAnchor->y, projectedEnd->x,
                       projectedEnd->y);
    accept_hit(
        distance, 10,
        ViewportGizmoHit{
            ViewportGizmoDragKind::Translate, axis.axis, center, tangentX,
            tangentY,
            std::max(1.0f, screenPixels / std::max(axis.length, 1.0e-4f)),
            axis.length, axis.axis_index});
  };

  const auto consider_axis_endpoint = [&](const CornerAxis &axis) {
    if (!drawScale || axis.length <= 1.0e-5f) {
      return;
    }
    const auto projectedEnd = ProjectWorldPointToOverlay(
        axis.endpoint, camera, width, height, renderAspect);
    if (!projectedEnd) {
      return;
    }
    const float distance =
        ScreenDistance(mouseX, mouseY, projectedEnd->x, projectedEnd->y);
    if (distance > kScaleHitRadius) {
      return;
    }
    float sx = projectedEnd->x - projectedAnchor->x;
    float sy = projectedEnd->y - projectedAnchor->y;
    float pixels = std::sqrt(sx * sx + sy * sy);
    if (pixels <= 1.0e-3f) {
      sx = 1.0f;
      sy = 0.0f;
      pixels = 1.0f;
    }
    accept_hit(distance, 30,
               ViewportGizmoHit{
                   ViewportGizmoDragKind::ScaleAxis, axis.axis, anchor,
                   sx / pixels, sy / pixels,
                   std::max(1.0f, pixels / std::max(axis.length, 1.0e-4f)),
                   axis.length, axis.axis_index});
  };

  for (const auto &axis : axes) {
    consider_axis_line(axis);
    consider_axis_endpoint(axis);
  }

  const auto consider_arc = [&](const vkpt::pathtracer::Vec3 &axis,
                                int axisIndex,
                                const vkpt::pathtracer::Vec3 &axisA,
                                const vkpt::pathtracer::Vec3 &axisB,
                                float radius) {
    if (!drawRotate || radius <= 1.0e-4f) {
      return;
    }
    constexpr int kArcSegments = 14;
    constexpr float kHalfPi = 1.57079632679489661923f;
    auto previous = ProjectedViewportPoint{};
    bool previousValid = false;
    for (int segment = 0; segment <= kArcSegments; ++segment) {
      const float t =
          (static_cast<float>(segment) / static_cast<float>(kArcSegments)) *
          kHalfPi;
      const auto world =
          PtAdd(anchor, PtAdd(PtMul(axisA, std::cos(t) * radius),
                              PtMul(axisB, std::sin(t) * radius)));
      const auto projected = ProjectWorldPointToOverlay(world, camera, width,
                                                        height, renderAspect);
      if (projected && previousValid) {
        float tangentX = 1.0f;
        float tangentY = 0.0f;
        const float distance = ScreenDistanceToSegment(
            mouseX, mouseY, previous, *projected, &tangentX, &tangentY);
        if (distance <= kRotateHitRadius) {
          accept_hit(distance, 20,
                     ViewportGizmoHit{ViewportGizmoDragKind::Rotate, axis,
                                      center, tangentX, tangentY, 1.0f,
                                      std::max(radius, 1.0e-4f), axisIndex});
        }
      }
      if (projected) {
        previous = *projected;
        previousValid = true;
      } else {
        previousValid = false;
      }
    }
  };

  consider_arc(axes[0].axis, 0, axes[1].axis, axes[2].axis,
               std::min(yLength, zLength));
  consider_arc(axes[1].axis, 1, axes[0].axis, axes[2].axis,
               std::min(xLength, zLength));
  consider_arc(axes[2].axis, 2, axes[0].axis, axes[1].axis,
               std::min(xLength, yLength));
  return best;
}

std::optional<ViewportGizmoHit> PickSelectionBoundsFreeform(
    const vkpt::editor::Bounds &bounds, const ViewportCameraPose &camera,
    float width, float height, float renderAspect, vkpt::editor::GizmoMode mode,
    float mouseX, float mouseY) {
  if (!bounds.valid || mode == vkpt::editor::GizmoMode::None) {
    return std::nullopt;
  }
  const auto corners = BoundsCorners(bounds);
  std::array<std::optional<ProjectedViewportPoint>, 8> projected{};
  for (std::size_t i = 0; i < corners.size(); ++i) {
    projected[i] = ProjectWorldPointToOverlay(corners[i], camera, width, height,
                                              renderAspect);
  }

  float bestDistance = 10.0f;
  for (const auto [a, b] : kViewportBoundsEdges) {
    const auto &pa = projected[static_cast<std::size_t>(a)];
    const auto &pb = projected[static_cast<std::size_t>(b)];
    if (!pa || !pb) {
      continue;
    }
    const float distance = ScreenDistanceToSegment(mouseX, mouseY, *pa, *pb);
    bestDistance = std::min(bestDistance, distance);
  }
  if (bestDistance > 9.5f) {
    return std::nullopt;
  }

  const auto min = ToPtVec3(bounds.min);
  const auto max = ToPtVec3(bounds.max);
  return ViewportGizmoHit{ViewportGizmoDragKind::FreeformTranslate,
                          {},
                          PtMul(PtAdd(min, max), 0.5f),
                          1.0f,
                          0.0f,
                          1.0f,
                          1.0f,
                          -1};
}

void AddSelectionGizmo(vkpt::platform::QtSelectionOverlayBox &box,
                       const vkpt::editor::Bounds &bounds,
                       const ViewportCameraPose &camera, float width,
                       float height, float renderAspect,
                       vkpt::editor::GizmoMode mode,
                       const std::optional<ViewportGizmoHit> &hover) {
  if (mode == vkpt::editor::GizmoMode::None || !bounds.valid) {
    return;
  }
  const auto min = ToPtVec3(bounds.min);
  const auto max = ToPtVec3(bounds.max);
  const float extentX = std::fabs(max.x - min.x);
  const float extentY = std::fabs(max.y - min.y);
  const float extentZ = std::fabs(max.z - min.z);
  const float maxExtent = std::max({extentX, extentY, extentZ});
  if (maxExtent <= 1.0e-5f) {
    return;
  }

  const auto corners = BoundsCorners(bounds);
  std::size_t anchorIndex = 0u;
  float nearestDepth = std::numeric_limits<float>::infinity();
  // Rendering mirrors the picking anchor calculation; changing one path should
  // be reflected in the other to keep hover feedback aligned with hit tests.
  for (std::size_t i = 0; i < corners.size(); ++i) {
    const auto projected = ProjectWorldPointToOverlay(corners[i], camera, width,
                                                      height, renderAspect);
    if (projected && projected->depth < nearestDepth) {
      nearestDepth = projected->depth;
      anchorIndex = i;
    }
  }
  const auto anchor = corners[anchorIndex];
  const bool anchorMinX =
      std::fabs(anchor.x - min.x) <= std::fabs(anchor.x - max.x);
  const bool anchorMinY =
      std::fabs(anchor.y - min.y) <= std::fabs(anchor.y - max.y);
  const bool anchorMinZ =
      std::fabs(anchor.z - min.z) <= std::fabs(anchor.z - max.z);
  struct CornerAxis {
    int axis_index = -1;
    vkpt::pathtracer::Vec3 axis{};
    vkpt::pathtracer::Vec3 endpoint{};
    float length = 0.0f;
    OverlayColor color{};
  };
  constexpr OverlayColor kX{245u, 76u, 76u, 170u};
  constexpr OverlayColor kY{84u, 214u, 112u, 170u};
  constexpr OverlayColor kZ{74u, 144u, 255u, 170u};
  const std::array<CornerAxis, 3> axes{{
      {0,
       {anchorMinX ? 1.0f : -1.0f, 0.0f, 0.0f},
       {anchorMinX ? max.x : min.x, anchor.y, anchor.z},
       extentX,
       kX},
      {1,
       {0.0f, anchorMinY ? 1.0f : -1.0f, 0.0f},
       {anchor.x, anchorMinY ? max.y : min.y, anchor.z},
       extentY,
       kY},
      {2,
       {0.0f, 0.0f, anchorMinZ ? 1.0f : -1.0f},
       {anchor.x, anchor.y, anchorMinZ ? max.z : min.z},
       extentZ,
       kZ},
  }};
  const auto tickLength = [maxExtent](float axisExtent) {
    if (axisExtent <= 1.0e-5f) {
      return 0.0f;
    }
    return std::max(0.025f, std::min(axisExtent * 0.35f, maxExtent * 0.18f));
  };
  const float xLength = tickLength(extentX);
  const float yLength = tickLength(extentY);
  const float zLength = tickLength(extentZ);

  const bool drawTranslate = mode == vkpt::editor::GizmoMode::Translate ||
                             mode == vkpt::editor::GizmoMode::Universal;
  const bool drawRotate = mode == vkpt::editor::GizmoMode::Rotate ||
                          mode == vkpt::editor::GizmoMode::Universal;
  const bool drawScale = mode == vkpt::editor::GizmoMode::Scale ||
                         mode == vkpt::editor::GizmoMode::Universal;

  constexpr OverlayColor kCorner{255u, 255u, 255u, 170u};
  const auto highlight_color = [](OverlayColor color) {
    color.a = 255u;
    color.r = static_cast<std::uint8_t>(
        std::min(255, static_cast<int>(color.r) + 22));
    color.g = static_cast<std::uint8_t>(
        std::min(255, static_cast<int>(color.g) + 22));
    color.b = static_cast<std::uint8_t>(
        std::min(255, static_cast<int>(color.b) + 22));
    return color;
  };

  if (drawTranslate) {
    for (const auto &axis : axes) {
      if (axis.length <= 1.0e-5f) {
        continue;
      }
      const bool hovered = IsHoveredGizmoHandle(
          hover, ViewportGizmoDragKind::Translate, axis.axis_index);
      AddWorldOverlayLine(box, camera, width, height, renderAspect, anchor,
                          axis.endpoint,
                          hovered ? highlight_color(axis.color) : axis.color,
                          hovered ? 2.4f : 1.35f);
    }
  }

  if (drawScale) {
    for (const auto &axis : axes) {
      if (axis.length <= 1.0e-5f) {
        continue;
      }
      const bool hovered = IsHoveredGizmoHandle(
          hover, ViewportGizmoDragKind::ScaleAxis, axis.axis_index);
      AddWorldOverlayPoint(box, camera, width, height, renderAspect,
                           axis.endpoint,
                           hovered ? highlight_color(axis.color) : axis.color,
                           hovered ? 4.5f : 3.1f);
    }
  }

  if (drawRotate) {
    const bool hoverX =
        IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::Rotate, 0);
    const bool hoverY =
        IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::Rotate, 1);
    const bool hoverZ =
        IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::Rotate, 2);
    AddGizmoCornerArc(box, camera, width, height, renderAspect, anchor,
                      axes[1].axis, axes[2].axis, std::min(yLength, zLength),
                      hoverX ? highlight_color(kX) : kX, hoverX ? 2.3f : 1.1f);
    AddGizmoCornerArc(box, camera, width, height, renderAspect, anchor,
                      axes[0].axis, axes[2].axis, std::min(xLength, zLength),
                      hoverY ? highlight_color(kY) : kY, hoverY ? 2.3f : 1.1f);
    AddGizmoCornerArc(box, camera, width, height, renderAspect, anchor,
                      axes[0].axis, axes[1].axis, std::min(xLength, yLength),
                      hoverZ ? highlight_color(kZ) : kZ, hoverZ ? 2.3f : 1.1f);
  }

  if (drawTranslate || drawRotate || drawScale) {
    AddWorldOverlayPoint(box, camera, width, height, renderAspect, anchor,
                         kCorner, 2.8f);
  }
}

} // namespace vkpt::app

#endif
