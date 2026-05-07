#pragma once

#ifdef PT_ENABLE_QT

#include "core/Types.h"
#include "editor/UiModels.h"
#include "pathtracer/PathTracer.h"
#include "platform/qt/QtPlatform.h"
#include "scene/Scene.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace vkpt::app {

struct ViewportCameraPose {
  vkpt::pathtracer::Vec3 position{};
  vkpt::pathtracer::Vec3 target{};
  vkpt::pathtracer::Vec3 up{0.0f, 1.0f, 0.0f};
  float fov_deg = 60.0f;
};

struct ViewportPickable {
  struct Triangle {
    vkpt::pathtracer::Vec3 v0{};
    vkpt::pathtracer::Vec3 v1{};
    vkpt::pathtracer::Vec3 v2{};
    vkpt::pathtracer::Vec3 bounds_min{};
    vkpt::pathtracer::Vec3 bounds_max{};
    bool bounds_valid = false;
  };

  vkpt::core::StableId entity_id = 0;
  vkpt::editor::Bounds bounds{};
  std::string label;
  std::vector<Triangle> triangles;
  bool require_triangle_hit = false;
};

struct ViewportRay {
  vkpt::pathtracer::Vec3 origin{};
  vkpt::pathtracer::Vec3 direction{};
};

struct ViewportPickResult {
  vkpt::core::StableId entity_id = 0;
  vkpt::editor::Bounds bounds{};
  std::string label;
  float distance = 0.0f;
};

struct FpsPlayerState {
  bool initialized = false;
  vkpt::pathtracer::Vec3 feet_position{};
  vkpt::pathtracer::Vec3 velocity{};
  bool grounded = false;
  bool crouching = false;
  bool running = false;
  bool jump_queued = false;
  float eye_height = 1.62f;
  float current_speed = 0.0f;
};

struct FpsCollisionHit {
  bool hit = false;
  float distance = 0.0f;
  vkpt::pathtracer::Vec3 position{};
  vkpt::pathtracer::Vec3 normal{0.0f, 1.0f, 0.0f};
};

struct FpsMovementTuning {
  float stand_eye_height = 1.62f;
  float crouch_eye_height = 1.05f;
  float radius = 0.32f;
  float step_height = 0.38f;
  float skin = 0.03f;
  float walk_speed = 1.45f;
  float run_speed = 2.4f;
  float crouch_speed = 0.65f;
  float air_control_scale = 0.45f;
  float gravity = 18.0f;
  float jump_speed = 5.2f;
};

struct FpsMovementRequest {
  std::uint64_t sequence = 0u;
  std::uint64_t collision_revision = 0u;
  FpsPlayerState player{};
  vkpt::pathtracer::Vec3 wish_move{};
  FpsMovementTuning tuning{};
  float dt_seconds = 0.0f;
  bool crouching = false;
  bool running = false;
};

struct FpsMovementResult {
  std::uint64_t sequence = 0u;
  std::uint64_t collision_revision = 0u;
  FpsPlayerState player{};
  bool pose_changed = false;
  bool state_changed = false;
  double solve_ms = 0.0;
};

class FpsCollisionWorker final {
 public:
  FpsCollisionWorker();
  ~FpsCollisionWorker();

  void stop();
  void set_pickables(std::vector<ViewportPickable> pickables);
  std::uint64_t collision_revision() const;
  void submit(FpsMovementRequest request);
  bool has_work() const;
  void discard_pending_results();
  std::optional<FpsMovementResult> take_latest_result();

 private:
  void run(std::stop_token stop);

  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::jthread m_thread;
  std::shared_ptr<const std::vector<ViewportPickable>> m_pickables;
  std::uint64_t m_revision = 0u;
  std::optional<FpsMovementRequest> m_pending;
  std::optional<FpsMovementResult> m_result;
  bool m_busy = false;
};

enum class ViewportGizmoDragKind {
  None,
  Translate,
  Rotate,
  ScaleAxis,
};

struct ViewportGizmoHit {
  ViewportGizmoDragKind kind = ViewportGizmoDragKind::None;
  vkpt::pathtracer::Vec3 axis{};
  vkpt::pathtracer::Vec3 pivot{};
  float screen_axis_x = 1.0f;
  float screen_axis_y = 0.0f;
  float pixels_per_unit = 1.0f;
  float axis_world_length = 1.0f;
  int axis_index = -1;
};

struct OverlayColor {
  std::uint8_t r = 255u;
  std::uint8_t g = 255u;
  std::uint8_t b = 255u;
  std::uint8_t a = 255u;
};

ViewportPickable::Triangle MakeViewportTriangle(const vkpt::pathtracer::Vec3& v0,
                                                const vkpt::pathtracer::Vec3& v1,
                                                const vkpt::pathtracer::Vec3& v2);
vkpt::pathtracer::Vec3 PtAdd(const vkpt::pathtracer::Vec3& a,
                             const vkpt::pathtracer::Vec3& b);
vkpt::pathtracer::Vec3 PtSub(const vkpt::pathtracer::Vec3& a,
                             const vkpt::pathtracer::Vec3& b);
vkpt::pathtracer::Vec3 PtMul(const vkpt::pathtracer::Vec3& v, float scale);
float PtDot(const vkpt::pathtracer::Vec3& a, const vkpt::pathtracer::Vec3& b);
vkpt::pathtracer::Vec3 PtCross(const vkpt::pathtracer::Vec3& a,
                               const vkpt::pathtracer::Vec3& b);
float PtLength(const vkpt::pathtracer::Vec3& v);
vkpt::pathtracer::Vec3 PtNormalize(const vkpt::pathtracer::Vec3& v,
                                   vkpt::pathtracer::Vec3 fallback = {0.0f, 0.0f, -1.0f});

vkpt::scene::Quat NormalizeQuat(vkpt::scene::Quat q);
vkpt::scene::Quat QuatMultiply(const vkpt::scene::Quat& a, const vkpt::scene::Quat& b);
vkpt::scene::Quat QuatFromAxisAngle(const vkpt::pathtracer::Vec3& axis, float radians);
vkpt::scene::Quat QuatFromCameraForwardUp(const vkpt::pathtracer::Vec3& forwardIn,
                                          const vkpt::pathtracer::Vec3& upIn);
vkpt::pathtracer::Vec3 RotatePointByQuat(const vkpt::pathtracer::Vec3& point,
                                         const vkpt::scene::Quat& rotation);
vkpt::pathtracer::Vec3 InverseRotatePointByQuat(const vkpt::pathtracer::Vec3& point,
                                                const vkpt::scene::Quat& rotation);
vkpt::pathtracer::Vec3 ApplySceneTransformToPoint(const vkpt::pathtracer::Vec3& point,
                                                  const vkpt::scene::TransformComponent& transform);
vkpt::pathtracer::Vec3 InverseSceneTransformPoint(const vkpt::pathtracer::Vec3& point,
                                                  const vkpt::scene::TransformComponent& transform);
float ClampFloat(float value, float min_value, float max_value);
float DegToRad(float degrees);
vkpt::pathtracer::Vec3 ToPtVec3(const vkpt::scene::Vec3& v);
vkpt::scene::Vec3 ToSceneVec3(const vkpt::pathtracer::Vec3& v);
vkpt::pathtracer::Quat4 ToPtQuat4(const vkpt::scene::Quat& q);
vkpt::editor::Vec3 ToEditorVec3(const vkpt::pathtracer::Vec3& v);
vkpt::pathtracer::Vec3 ToPtVec3(const vkpt::editor::Vec3& v);

int RunDynamicPhysicsPerformanceGate(std::string scenePath,
                                     std::string backend,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t frames);
int RunThirdPersonScriptPerformanceGate(std::string scenePath,
                                        std::string backend,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t frames);
void ExpandBounds(vkpt::editor::Bounds& bounds, const vkpt::pathtracer::Vec3& point);
std::optional<vkpt::scene::SceneWorld> BuildSceneWorldSnapshot(
    const vkpt::scene::SceneDocument& document);
vkpt::scene::TransformComponent ResolveEntityWorldTransform(
    const vkpt::scene::SceneEntityDefinition& entity,
    const vkpt::scene::SceneWorld* world);
vkpt::scene::TransformComponent TransformFromRtInstance(
    const vkpt::pathtracer::RTInstance& instance);
vkpt::scene::TransformComponent ConvertWorldTransformToDocumentLocal(
    const vkpt::scene::SceneEntityDefinition& entity,
    const vkpt::scene::SceneWorld* currentWorld,
    const vkpt::scene::TransformComponent& worldTransform);

std::vector<ViewportPickable> BuildViewportPickables(const vkpt::scene::SceneDocument& document,
                                                     const vkpt::pathtracer::RTSceneData& scene);
ViewportRay BuildViewportRay(const ViewportCameraPose& camera,
                             float x,
                             float y,
                             float width,
                             float height,
                             float renderAspect = 0.0f);
FpsCollisionHit TraceFpsGround(const std::vector<ViewportPickable>& pickables,
                               const vkpt::pathtracer::Vec3& origin,
                               float maxDistance,
                               float minNormalY);
FpsCollisionHit TraceFpsWall(const std::vector<ViewportPickable>& pickables,
                             const vkpt::pathtracer::Vec3& origin,
                             const vkpt::pathtracer::Vec3& direction,
                             float maxDistance,
                             float minNormalY);
FpsCollisionHit TraceFpsBodyWall(const std::vector<ViewportPickable>& pickables,
                                 const vkpt::pathtracer::Vec3& feetPosition,
                                 const vkpt::pathtracer::Vec3& direction,
                                 float maxDistance,
                                 float radius,
                                 float height,
                                 float minNormalY);
vkpt::pathtracer::Vec3 ResolveFpsHorizontalDeltaForPlayer(
    const std::vector<ViewportPickable>& pickables,
    const vkpt::pathtracer::Vec3& feetPosition,
    const vkpt::pathtracer::Vec3& desiredDelta,
    float radius,
    float skin,
    float height);
FpsMovementResult SolveFpsMovement(const std::vector<ViewportPickable>& pickables,
                                   const FpsMovementRequest& request);
std::optional<ViewportPickResult> PickViewportObject(const std::vector<ViewportPickable>& pickables,
                                                     const ViewportCameraPose& camera,
                                                     float x,
                                                     float y,
                                                     float width,
                                                     float height,
                                                     float renderAspect = 0.0f);
vkpt::platform::QtViewportCursor CursorForGizmoHit(const ViewportGizmoHit& hit);
float ScreenDistance(float ax, float ay, float bx, float by);
bool SameGizmoHandle(const std::optional<ViewportGizmoHit>& a,
                     const std::optional<ViewportGizmoHit>& b);
std::optional<vkpt::pathtracer::Vec3> ScreenPointOnCameraPlane(
    const ViewportCameraPose& camera,
    float x,
    float y,
    float width,
    float height,
    float renderAspect,
    const vkpt::pathtracer::Vec3& planePoint);
std::optional<ViewportGizmoHit> PickSelectionGizmoHandle(const vkpt::editor::Bounds& bounds,
                                                         const ViewportCameraPose& camera,
                                                         float width,
                                                         float height,
                                                         float renderAspect,
                                                         vkpt::editor::GizmoMode mode,
                                                         float mouseX,
                                                         float mouseY);
void AddWorldOverlayLine(vkpt::platform::QtSelectionOverlayBox& box,
                         const ViewportCameraPose& camera,
                         float width,
                         float height,
                         float renderAspect,
                         const vkpt::pathtracer::Vec3& a,
                         const vkpt::pathtracer::Vec3& b,
                         OverlayColor color,
                         float lineWidth);
void AddWorldOverlayPoint(vkpt::platform::QtSelectionOverlayBox& box,
                          const ViewportCameraPose& camera,
                          float width,
                          float height,
                          float renderAspect,
                          const vkpt::pathtracer::Vec3& point,
                          OverlayColor color,
                          float radius,
                          std::string label = {});
std::vector<vkpt::platform::QtSelectionOverlayBox> BuildSelectionOverlayBoxes(
    const vkpt::editor::SelectionState& selection,
    const std::vector<ViewportPickable>& pickables,
    const ViewportCameraPose& camera,
    float width,
    float height,
    float renderAspect,
    vkpt::editor::GizmoMode gizmoMode,
    const std::optional<ViewportGizmoHit>& activeHover);
void RebuildSelectionBounds(vkpt::editor::SelectionState& selection,
                            const std::vector<ViewportPickable>& pickables);

}  // namespace vkpt::app

#endif
