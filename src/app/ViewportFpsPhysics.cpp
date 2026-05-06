#include "app/ViewportInteractionInternal.h"

#ifdef PT_ENABLE_QT

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/Logging.h"

namespace vkpt::app {

FpsCollisionHit TraceFpsGround(const std::vector<ViewportPickable> &pickables,
                               const vkpt::pathtracer::Vec3 &origin,
                               float maxDistance, float minWalkableNormalY) {
  FpsCollisionHit best{};
  float bestDistance = maxDistance;
  const ViewportRay ray{origin, {0.0f, -1.0f, 0.0f}};
  constexpr float kQueryPad = 0.08f;
  // Narrow the triangle loop to the vertical capsule column under the player;
  // FPS collision runs often enough that broad AABB rejection matters.
  const vkpt::pathtracer::Vec3 queryMin{origin.x - kQueryPad,
                                        origin.y - maxDistance - kQueryPad,
                                        origin.z - kQueryPad};
  const vkpt::pathtracer::Vec3 queryMax{
      origin.x + kQueryPad, origin.y + kQueryPad, origin.z + kQueryPad};
  for (const auto &pickable : pickables) {
    if (pickable.triangles.empty()) {
      continue;
    }
    if (!BoundsOverlapsAabb(pickable.bounds, queryMin, queryMax)) {
      continue;
    }
    float boundsDistance = 0.0f;
    if (pickable.bounds.valid &&
        (!IntersectBounds(ray, pickable.bounds, boundsDistance) ||
         boundsDistance >= bestDistance)) {
      continue;
    }
    for (const auto &triangle : pickable.triangles) {
      if (!TriangleOverlapsAabb(triangle, queryMin, queryMax)) {
        continue;
      }
      float distance = 0.0f;
      vkpt::pathtracer::Vec3 normal{};
      if (!IntersectTriangleDoubleSided(ray, triangle, bestDistance, distance,
                                        normal)) {
        continue;
      }
      if (std::fabs(normal.y) < minWalkableNormalY) {
        continue;
      }
      best.hit = true;
      best.distance = distance;
      best.position = PtAdd(origin, PtMul(ray.direction, distance));
      best.normal = normal;
      bestDistance = distance;
    }
  }
  return best;
}

FpsCollisionHit TraceFpsWall(const std::vector<ViewportPickable> &pickables,
                             const vkpt::pathtracer::Vec3 &origin,
                             const vkpt::pathtracer::Vec3 &direction,
                             float maxDistance, float maxWalkableNormalY) {
  FpsCollisionHit best{};
  float bestDistance = maxDistance;
  const ViewportRay ray{origin, PtNormalize(direction, {1.0f, 0.0f, 0.0f})};
  constexpr float kQueryPad = 0.08f;
  const auto end = PtAdd(origin, PtMul(ray.direction, maxDistance));
  const vkpt::pathtracer::Vec3 queryMin{std::min(origin.x, end.x) - kQueryPad,
                                        std::min(origin.y, end.y) - kQueryPad,
                                        std::min(origin.z, end.z) - kQueryPad};
  const vkpt::pathtracer::Vec3 queryMax{std::max(origin.x, end.x) + kQueryPad,
                                        std::max(origin.y, end.y) + kQueryPad,
                                        std::max(origin.z, end.z) + kQueryPad};
  for (const auto &pickable : pickables) {
    if (pickable.triangles.empty()) {
      continue;
    }
    if (!BoundsOverlapsAabb(pickable.bounds, queryMin, queryMax)) {
      continue;
    }
    float boundsDistance = 0.0f;
    if (pickable.bounds.valid &&
        (!IntersectBounds(ray, pickable.bounds, boundsDistance) ||
         boundsDistance >= bestDistance)) {
      continue;
    }
    for (const auto &triangle : pickable.triangles) {
      if (!TriangleOverlapsAabb(triangle, queryMin, queryMax)) {
        continue;
      }
      float distance = 0.0f;
      vkpt::pathtracer::Vec3 normal{};
      if (!IntersectTriangleDoubleSided(ray, triangle, bestDistance, distance,
                                        normal)) {
        continue;
      }
      if (std::fabs(normal.y) > maxWalkableNormalY) {
        continue;
      }
      best.hit = true;
      best.distance = distance;
      best.position = PtAdd(origin, PtMul(ray.direction, distance));
      best.normal = normal;
      bestDistance = distance;
    }
  }
  return best;
}

FpsCollisionHit
TraceFpsBodyWallWithProbeHeights(const std::vector<ViewportPickable> &pickables,
                                 const vkpt::pathtracer::Vec3 &feetPosition,
                                 const vkpt::pathtracer::Vec3 &direction,
                                 const std::array<float, 3> &probeHeights,
                                 float maxDistance, float maxWalkableNormalY) {
  FpsCollisionHit best{};
  float bestDistance = maxDistance;
  const auto rayDirection = PtNormalize(direction, {1.0f, 0.0f, 0.0f});
  constexpr float kQueryPad = 0.08f;
  const auto end = PtAdd(feetPosition, PtMul(rayDirection, maxDistance));
  // Three horizontal probes approximate a standing capsule without building a
  // separate collision mesh: ankle, torso, and head clearance.
  const auto minmaxHeight =
      std::minmax_element(probeHeights.begin(), probeHeights.end());
  const float minHeight =
      minmaxHeight.first == probeHeights.end() ? 0.0f : *minmaxHeight.first;
  const float maxHeight =
      minmaxHeight.second == probeHeights.end() ? 0.0f : *minmaxHeight.second;
  const vkpt::pathtracer::Vec3 queryMin{
      std::min(feetPosition.x, end.x) - kQueryPad,
      feetPosition.y + minHeight - kQueryPad,
      std::min(feetPosition.z, end.z) - kQueryPad};
  const vkpt::pathtracer::Vec3 queryMax{
      std::max(feetPosition.x, end.x) + kQueryPad,
      feetPosition.y + maxHeight + kQueryPad,
      std::max(feetPosition.z, end.z) + kQueryPad};

  for (const auto &pickable : pickables) {
    if (pickable.triangles.empty() ||
        !BoundsOverlapsAabb(pickable.bounds, queryMin, queryMax)) {
      continue;
    }
    for (const auto &triangle : pickable.triangles) {
      if (!TriangleOverlapsAabb(triangle, queryMin, queryMax)) {
        continue;
      }
      for (const float height : probeHeights) {
        const ViewportRay ray{PtAdd(feetPosition, {0.0f, height, 0.0f}),
                              rayDirection};
        float distance = 0.0f;
        vkpt::pathtracer::Vec3 normal{};
        if (!IntersectTriangleDoubleSided(ray, triangle, bestDistance, distance,
                                          normal)) {
          continue;
        }
        if (std::fabs(normal.y) > maxWalkableNormalY) {
          continue;
        }
        best.hit = true;
        best.distance = distance;
        best.position = PtAdd(ray.origin, PtMul(ray.direction, distance));
        best.normal = normal;
        bestDistance = distance;
      }
    }
  }
  return best;
}

FpsCollisionHit TraceFpsBodyWall(const std::vector<ViewportPickable> &pickables,
                                 const vkpt::pathtracer::Vec3 &feetPosition,
                                 const vkpt::pathtracer::Vec3 &direction,
                                 float maxDistance, float radius, float height,
                                 float maxWalkableNormalY) {
  const std::array<float, 3> probeHeights{
      std::max(0.0f, radius), std::max(radius, height * 0.55f),
      std::max(radius, height - radius * 0.5f)};
  return TraceFpsBodyWallWithProbeHeights(pickables, feetPosition, direction,
                                          probeHeights, maxDistance,
                                          maxWalkableNormalY);
}

vkpt::pathtracer::Vec3 ResolveFpsHorizontalDeltaForPlayer(
    const std::vector<ViewportPickable> &pickables,
    const vkpt::pathtracer::Vec3 &feetPosition,
    const vkpt::pathtracer::Vec3 &desiredDelta, float radius, float skin,
    float eyeHeight) {
  vkpt::pathtracer::Vec3 resolved{};
  vkpt::pathtracer::Vec3 remaining = desiredDelta;
  // Resolve horizontal motion iteratively so a blocked step can slide along
  // one wall and still consume the remaining movement against another.
  for (int iteration = 0; iteration < 3; ++iteration) {
    const float remainingDistance = PtLength(remaining);
    if (remainingDistance <= 1.0e-5f) {
      break;
    }
    const auto direction = PtNormalize(remaining, {1.0f, 0.0f, 0.0f});
    const float traceDistance = remainingDistance + radius + skin;
    const std::array<float, 3> probeHeights{
        std::max(radius, skin), std::max(radius, eyeHeight * 0.55f),
        std::max(radius, eyeHeight - radius * 0.5f)};
    const auto nearest = TraceFpsBodyWallWithProbeHeights(
        pickables, PtAdd(feetPosition, resolved), direction, probeHeights,
        traceDistance, 0.62f);
    if (!nearest.hit || nearest.distance > traceDistance) {
      resolved = PtAdd(resolved, remaining);
      break;
    }

    const float allowedDistance =
        ClampFloat(nearest.distance - radius - skin, 0.0f, remainingDistance);
    resolved = PtAdd(resolved, PtMul(direction, allowedDistance));
    auto slide = PtMul(direction, remainingDistance - allowedDistance);
    auto wallNormal =
        vkpt::pathtracer::Vec3{nearest.normal.x, 0.0f, nearest.normal.z};
    wallNormal = PtNormalize(wallNormal, {});
    if (PtLength(wallNormal) <= 1.0e-5f) {
      break;
    }
    const float intoWall = PtDot(slide, wallNormal);
    if (intoWall < 0.0f) {
      slide = PtSub(slide, PtMul(wallNormal, intoWall));
    }
    remaining = slide;
  }
  return resolved;
}

FpsMovementResult
SolveFpsMovement(const std::vector<ViewportPickable> &pickables,
                 const FpsMovementRequest &request) {
  auto player = request.player;
  const auto eye_position = [](const FpsPlayerState &state) {
    return PtAdd(state.feet_position, {0.0f, state.eye_height, 0.0f});
  };

  const auto oldEye = eye_position(player);
  const auto oldFeet = player.feet_position;
  const bool oldGrounded = player.grounded;
  const bool oldCrouching = player.crouching;
  const bool oldRunning = player.running;
  const float oldEyeHeight = player.eye_height;

  player.crouching = request.crouching;
  player.running = request.running && !player.crouching;

  const float targetEyeHeight = player.crouching
                                    ? request.tuning.crouch_eye_height
                                    : request.tuning.stand_eye_height;
  const float crouchBlend = ClampFloat(request.dt_seconds * 12.0f, 0.0f, 1.0f);
  player.eye_height =
      player.eye_height + (targetEyeHeight - player.eye_height) * crouchBlend;

  if (player.jump_queued && player.grounded) {
    player.velocity.y = request.tuning.jump_speed;
    player.grounded = false;
  }
  player.jump_queued = false;

  if (PtLength(request.wish_move) > 1.0e-5f) {
    float speed = request.tuning.walk_speed;
    if (player.crouching) {
      speed = request.tuning.crouch_speed;
    } else if (player.running) {
      speed = request.tuning.run_speed;
    }
    if (!player.grounded) {
      speed *= request.tuning.air_control_scale;
    }
    const auto desiredDelta =
        PtMul(PtNormalize(request.wish_move), speed * request.dt_seconds);
    const auto horizontalDelta = ResolveFpsHorizontalDeltaForPlayer(
        pickables, player.feet_position, desiredDelta, request.tuning.radius,
        request.tuning.skin, player.eye_height);
    player.feet_position = PtAdd(player.feet_position, horizontalDelta);
    player.current_speed =
        PtLength(horizontalDelta) / std::max(request.dt_seconds, 1.0e-5f);
  } else {
    player.current_speed = 0.0f;
  }

  if (!player.grounded) {
    player.velocity.y -= request.tuning.gravity * request.dt_seconds;
  } else if (player.velocity.y < 0.0f) {
    player.velocity.y = 0.0f;
  }
  player.feet_position.y += player.velocity.y * request.dt_seconds;

  const float fallDistance = std::max(0.0f, oldFeet.y - player.feet_position.y);
  const float groundProbeStartY = std::max(oldFeet.y, player.feet_position.y) +
                                  request.tuning.step_height +
                                  request.tuning.skin;
  const float verticalProbe = (groundProbeStartY - player.feet_position.y) +
                              request.tuning.step_height +
                              request.tuning.skin * 2.0f;
  const auto groundOrigin = vkpt::pathtracer::Vec3{
      player.feet_position.x, groundProbeStartY, player.feet_position.z};
  const auto ground =
      TraceFpsGround(pickables, groundOrigin, verticalProbe, 0.62f);
  // The downward probe starts above both the old and new feet positions so fast
  // falls still land on thin triangles crossed during this fixed step.
  const bool sweptThroughGround =
      ground.hit &&
      ground.position.y >=
          player.feet_position.y - request.tuning.skin * 2.0f &&
      ground.position.y <= oldFeet.y + request.tuning.step_height +
                               fallDistance + request.tuning.skin;
  if (ground.hit && player.velocity.y <= 0.0f &&
      (player.feet_position.y <= ground.position.y +
                                     request.tuning.step_height +
                                     request.tuning.skin ||
       sweptThroughGround)) {
    player.feet_position.y = ground.position.y;
    player.velocity.y = 0.0f;
    player.grounded = true;
  } else {
    player.grounded = false;
  }

  FpsMovementResult result{};
  result.sequence = request.sequence;
  result.collision_revision = request.collision_revision;
  result.player = player;
  const auto newEye = eye_position(player);
  result.pose_changed = PtLength(PtSub(newEye, oldEye)) > 1.0e-4f ||
                        std::fabs(player.eye_height - oldEyeHeight) > 1.0e-4f;
  result.state_changed = oldGrounded != player.grounded ||
                         oldCrouching != player.crouching ||
                         oldRunning != player.running;
  return result;
}

FpsCollisionWorker::FpsCollisionWorker()
    : m_pickables(std::make_shared<const std::vector<ViewportPickable>>()) {
  m_thread = std::jthread([this](std::stop_token stop) {
    try {
      run(stop);
    } catch (const std::exception& ex) {
      try {
        vkpt::log::Logger::instance().log(
            vkpt::log::Severity::Error,
            "viewport",
            "fps collision worker exception",
            {{"error", ex.what()}});
      } catch (...) {
      }
      std::scoped_lock lock(m_mutex);
      m_busy = false;
      m_pending.reset();
    } catch (...) {
      try {
        vkpt::log::Logger::instance().log(
            vkpt::log::Severity::Error,
            "viewport",
            "fps collision worker non-standard exception");
      } catch (...) {
      }
      std::scoped_lock lock(m_mutex);
      m_busy = false;
      m_pending.reset();
    }
  });
}

FpsCollisionWorker::~FpsCollisionWorker() { stop(); }

void FpsCollisionWorker::stop() {
  if (!m_thread.joinable()) {
    return;
  }
  m_thread.request_stop();
  m_cv.notify_all();
  m_thread.join();
}

void FpsCollisionWorker::set_pickables(
    std::vector<ViewportPickable> pickables) {
  std::scoped_lock lock(m_mutex);
  m_pickables = std::make_shared<const std::vector<ViewportPickable>>(
      std::move(pickables));
  ++m_revision;
  m_pending.reset();
  m_result.reset();
}

std::uint64_t FpsCollisionWorker::collision_revision() const {
  std::scoped_lock lock(m_mutex);
  return m_revision;
}

void FpsCollisionWorker::submit(FpsMovementRequest request) {
  {
    std::scoped_lock lock(m_mutex);
    request.collision_revision = m_revision;
    m_pending = request;
  }
  m_cv.notify_one();
}

bool FpsCollisionWorker::has_work() const {
  std::scoped_lock lock(m_mutex);
  return m_busy || m_pending.has_value();
}

void FpsCollisionWorker::discard_pending_results() {
  std::scoped_lock lock(m_mutex);
  m_pending.reset();
  m_result.reset();
}

std::optional<FpsMovementResult> FpsCollisionWorker::take_latest_result() {
  std::scoped_lock lock(m_mutex);
  if (!m_result) {
    return std::nullopt;
  }
  auto result = *m_result;
  m_result.reset();
  return result;
}

void FpsCollisionWorker::run(std::stop_token stop) {
  while (!stop.stop_requested()) {
    FpsMovementRequest request{};
    std::shared_ptr<const std::vector<ViewportPickable>> pickables;
    {
      std::unique_lock lock(m_mutex);
      m_cv.wait(lock, [&]() {
        return stop.stop_requested() || m_pending.has_value();
      });
      if (stop.stop_requested()) {
        break;
      }
      request = *m_pending;
      m_pending.reset();
      pickables = m_pickables;
      m_busy = true;
    }

    static const auto kEmptyPickables = std::vector<ViewportPickable>{};
    const auto &collisionPickables = pickables ? *pickables : kEmptyPickables;
    const auto solveStart = std::chrono::steady_clock::now();
    auto result = SolveFpsMovement(collisionPickables, request);
    result.solve_ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - solveStart)
                          .count();

    {
      std::scoped_lock lock(m_mutex);
      m_result = result;
      m_busy = false;
    }
  }
}
} // namespace vkpt::app

#endif
