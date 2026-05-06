#include "app/ViewportInteractionInternal.h"

#ifdef PT_ENABLE_QT

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vkpt::app {

std::vector<vkpt::pathtracer::Vec3>
BoundsCorners(const vkpt::editor::Bounds &bounds) {
  return {
      {bounds.min.x, bounds.min.y, bounds.min.z},
      {bounds.max.x, bounds.min.y, bounds.min.z},
      {bounds.min.x, bounds.max.y, bounds.min.z},
      {bounds.max.x, bounds.max.y, bounds.min.z},
      {bounds.min.x, bounds.min.y, bounds.max.z},
      {bounds.max.x, bounds.min.y, bounds.max.z},
      {bounds.min.x, bounds.max.y, bounds.max.z},
      {bounds.max.x, bounds.max.y, bounds.max.z},
  };
}

std::optional<ProjectedViewportPoint>
ProjectWorldPointToOverlay(const vkpt::pathtracer::Vec3 &point,
                           const ViewportCameraPose &camera, float width,
                           float height, float renderAspect) {
  if (width <= 1.0f || height <= 1.0f) {
    return std::nullopt;
  }
  const auto forward = PtNormalize(PtSub(camera.target, camera.position));
  const auto right =
      PtNormalize(PtCross(forward, camera.up), {1.0f, 0.0f, 0.0f});
  const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});
  const float aspect = std::max(0.01f, renderAspect);
  const float tanHalfFov =
      std::tan(0.5f * DegToRad(std::max(1.0f, camera.fov_deg)));

  const auto rel = PtSub(point, camera.position);
  const float depth = PtDot(rel, forward);
  if (depth <= 1.0e-4f) {
    return std::nullopt;
  }
  const float cameraX = PtDot(rel, right);
  const float cameraY = PtDot(rel, up);
  const float ndcX = cameraX / (depth * tanHalfFov * aspect);
  const float ndcY = cameraY / (depth * tanHalfFov);
  return ProjectedViewportPoint{(ndcX + 1.0f) * 0.5f * width,
                                (1.0f - ndcY) * 0.5f * height, depth};
}

void AddProjectedOverlayLine(vkpt::platform::QtSelectionOverlayBox &box,
                             const ProjectedViewportPoint &a,
                             const ProjectedViewportPoint &b,
                             OverlayColor color, float lineWidth) {
  box.lines.push_back(vkpt::platform::QtSelectionOverlayBox::Line{
      a.x, a.y, b.x, b.y, color.r, color.g, color.b, color.a, lineWidth});
}

void AddWorldOverlayLine(vkpt::platform::QtSelectionOverlayBox &box,
                         const ViewportCameraPose &camera, float width,
                         float height, float renderAspect,
                         const vkpt::pathtracer::Vec3 &a,
                         const vkpt::pathtracer::Vec3 &b, OverlayColor color,
                         float lineWidth) {
  const auto projectedA =
      ProjectWorldPointToOverlay(a, camera, width, height, renderAspect);
  const auto projectedB =
      ProjectWorldPointToOverlay(b, camera, width, height, renderAspect);
  if (!projectedA || !projectedB) {
    return;
  }
  AddProjectedOverlayLine(box, *projectedA, *projectedB, color, lineWidth);
}

void AddWorldOverlayPoint(vkpt::platform::QtSelectionOverlayBox &box,
                          const ViewportCameraPose &camera, float width,
                          float height, float renderAspect,
                          const vkpt::pathtracer::Vec3 &point,
                          OverlayColor color, float radius, std::string label) {
  const auto projected =
      ProjectWorldPointToOverlay(point, camera, width, height, renderAspect);
  if (!projected) {
    return;
  }
  box.points.push_back(vkpt::platform::QtSelectionOverlayBox::Point{
      projected->x, projected->y, radius, color.r, color.g, color.b, color.a,
      std::move(label)});
}

std::optional<vkpt::platform::QtSelectionOverlayBox>
ProjectBoundsToOverlay(const vkpt::editor::Bounds &bounds,
                       const ViewportCameraPose &camera, float width,
                       float height, float renderAspect, std::string label,
                       bool primary, vkpt::editor::GizmoMode gizmoMode,
                       const std::optional<ViewportGizmoHit> &hover) {
  if (!bounds.valid || width <= 1.0f || height <= 1.0f) {
    return std::nullopt;
  }
  constexpr std::array<std::pair<int, int>, 12> kBoundsEdges{{
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
  const auto corners = BoundsCorners(bounds);
  std::array<std::optional<ProjectedViewportPoint>, 8> projectedCorners{};
  float minX = std::numeric_limits<float>::infinity();
  float minY = std::numeric_limits<float>::infinity();
  float maxX = -std::numeric_limits<float>::infinity();
  float maxY = -std::numeric_limits<float>::infinity();
  bool anyProjected = false;
  for (std::size_t i = 0; i < corners.size(); ++i) {
    projectedCorners[i] = ProjectWorldPointToOverlay(corners[i], camera, width,
                                                     height, renderAspect);
    if (!projectedCorners[i]) {
      continue;
    }
    minX = std::min(minX, projectedCorners[i]->x);
    minY = std::min(minY, projectedCorners[i]->y);
    maxX = std::max(maxX, projectedCorners[i]->x);
    maxY = std::max(maxY, projectedCorners[i]->y);
    anyProjected = true;
  }
  if (!anyProjected) {
    return std::nullopt;
  }
  constexpr OverlayColor kPrimaryBox{255u, 214u, 64u, 245u};
  constexpr OverlayColor kSecondaryBox{102u, 204u, 255u, 230u};
  vkpt::platform::QtSelectionOverlayBox box{};
  box.label = std::move(label);
  box.primary = primary;
  auto boxColor = primary ? kPrimaryBox : kSecondaryBox;
  if (primary && hover &&
      hover->kind == ViewportGizmoDragKind::FreeformTranslate) {
    boxColor = {255u, 244u, 164u, 255u};
  }
  for (const auto [a, b] : kBoundsEdges) {
    if (!projectedCorners[static_cast<std::size_t>(a)] ||
        !projectedCorners[static_cast<std::size_t>(b)]) {
      continue;
    }
    AddProjectedOverlayLine(
        box, *projectedCorners[static_cast<std::size_t>(a)],
        *projectedCorners[static_cast<std::size_t>(b)], boxColor,
        primary
            ? (hover && hover->kind == ViewportGizmoDragKind::FreeformTranslate
                   ? 2.8f
                   : 2.0f)
            : 1.5f);
  }
  if (primary) {
    AddSelectionGizmo(box, bounds, camera, width, height, renderAspect,
                      gizmoMode, hover);
  }

  const float margin = 4.0f;
  minX = ClampFloat(minX - margin, -width, width * 2.0f);
  minY = ClampFloat(minY - margin, -height, height * 2.0f);
  maxX = ClampFloat(maxX + margin, -width, width * 2.0f);
  maxY = ClampFloat(maxY + margin, -height, height * 2.0f);
  if (maxX <= minX || maxY <= minY) {
    return std::nullopt;
  }
  box.x = minX;
  box.y = minY;
  box.width = maxX - minX;
  box.height = maxY - minY;
  return box;
}

std::vector<vkpt::platform::QtSelectionOverlayBox>
BuildSelectionOverlayBoxes(const vkpt::editor::SelectionState &selection,
                           const std::vector<ViewportPickable> &pickables,
                           const ViewportCameraPose &camera, float width,
                           float height, float renderAspect,
                           vkpt::editor::GizmoMode gizmoMode,
                           const std::optional<ViewportGizmoHit> &activeHover) {
  std::vector<vkpt::platform::QtSelectionOverlayBox> boxes;
  boxes.reserve(selection.selected_entity_ids.size());
  for (const auto selectedId : selection.selected_entity_ids) {
    auto bounds = std::optional<vkpt::editor::Bounds>{};
    std::string label = "entity " + std::to_string(selectedId);
    for (const auto &item : selection.per_item_bounds) {
      if (item.entity_id == selectedId && item.bounds.valid) {
        bounds = item.bounds;
        break;
      }
    }
    for (const auto &pickable : pickables) {
      if (pickable.entity_id == selectedId) {
        if (!bounds) {
          bounds = pickable.bounds;
        }
        label = pickable.label;
        break;
      }
    }
    if (!bounds) {
      continue;
    }
    auto projected = ProjectBoundsToOverlay(
        *bounds, camera, width, height, renderAspect, label,
        selectedId == selection.active_primary_entity,
        selectedId == selection.active_primary_entity
            ? gizmoMode
            : vkpt::editor::GizmoMode::None,
        selectedId == selection.active_primary_entity
            ? activeHover
            : std::optional<ViewportGizmoHit>{});
    if (projected) {
      boxes.push_back(std::move(*projected));
    }
  }
  return boxes;
}

void RebuildSelectionBounds(vkpt::editor::SelectionState &selection,
                            const std::vector<ViewportPickable> &pickables) {
  selection.per_item_bounds.clear();
  selection.per_item_bounds.reserve(selection.selected_entity_ids.size());
  selection.aggregate_bounds = {};
  for (const auto selectedId : selection.selected_entity_ids) {
    const auto it =
        std::find_if(pickables.begin(), pickables.end(),
                     [selectedId](const ViewportPickable &pickable) {
                       return pickable.entity_id == selectedId;
                     });
    if (it == pickables.end() || !it->bounds.valid) {
      continue;
    }
    selection.per_item_bounds.push_back({selectedId, it->bounds});
    ExpandBounds(selection.aggregate_bounds, ToPtVec3(it->bounds.min));
    ExpandBounds(selection.aggregate_bounds, ToPtVec3(it->bounds.max));
  }
}

} // namespace vkpt::app

#endif
