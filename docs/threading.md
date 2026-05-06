Yes — your atomicity correction is the missing piece. After re-checking the current source, the final proposal should be stricter than “make CPU honest.” It should be:

> **Transform updates are transactions. If a transform update is rejected, unsupported, or requires forbidden fallback work, no committed render state may change.**

That transaction boundary must include **RenderCoordinator’s scene mirror**, **CPU tracer state**, **D3D12 scene/GPU-instance mirrors**, and **acceleration structures**.

## Source-confirmed corrections

The current `RTInstanceTransformUpdate` payload is TRS-only: entity/index/flags/revision plus `translation`, `rotation`, and `scale`. So the public API should keep TRS for now, and backends that need matrices should derive/cache them internally. `RTInstance` also already carries dynamic flags, local geometry ranges, and TRS, which is enough foundation for CPU dynamic BLAS/TLAS later. ([GitHub][1])

The current interfaces are too weak: `IRayAccelerator` has no instance-transform update API, and `IPathTracer::update_instance_transforms()` returns only `bool`, even though its comment says `false` means the backend needs a full scene snapshot or acceleration rebuild. ([GitHub][1])

The current `RenderCoordinator` fast path is **not atomic**. It applies `ApplyInstanceTransformUpdates(scene, updates)` to the coordinator’s local scene mirror before calling the backend. If the backend then fails or requires fallback, the coordinator scene mirror has already moved ahead of the committed acceleration state. ([GitHub][2])

That is worse because `ApplyInstanceTransformUpdates()` does more than metadata: it mutates instance TRS/flags/revision and, when local vertex ranges exist, rebakes transformed vertices into `scene.vertices`. That function should not be called speculatively on a fast transform path. ([GitHub][3])

The scalar CPU path has the same atomicity problem. It mutates `m_scene` first, then either returns external accelerator build status without actually updating that accelerator, or calls `build_or_update_acceleration()` and returns success. That is both a hidden rebuild and a possible scene/accelerator consistency bug. ([GitHub][4])

D3D12 is architecturally closer to correct, but it also needs transaction staging. Its transform-update path mutates `m_sceneData` and `m_gpuInsts`, builds/updates dynamic acceleration state, and uploads. If the upload/TLAS path fails, those CPU mirrors may already have changed. The good news is that D3D12 already has a better pattern in `update_scene_delta()`: it creates a `nextScene`, applies into the copy, uploads, and only commits after success. Reuse that pattern for transform updates. ([GitHub][5])

## Final contract

Use this as the central rule in docs and code comments:

```text
Transform motion is transactional.

If an update returns an applied status:
  coordinator scene mirror, backend scene mirror, instance buffers,
  and dynamic acceleration state are all committed to the same transform revision.

If an update returns unsupported/failed/needs forbidden fallback:
  coordinator scene mirror, backend scene mirror, instance buffers,
  and acceleration structures remain unchanged.

Non-applied statuses must be side-effect free.
```

This eliminates the stale-state class where “render scene says object moved, BVH still says object did not.”

## Final API shape

### 1. Add reason and fallback policy to transform commands

`post_instance_transforms()` needs to carry policy. A vector alone is not enough.

```cpp
enum class RenderUpdateReason : std::uint8_t {
  Unknown,
  PhysicsMotion,
  AnimationMotion,
  EditorGizmoMotion,
  ScriptTransformMotion,
  CameraMotion,
  MaterialEdit,
  LightEdit,
  StructuralSceneEdit,
  SceneLoad,
  ExplicitUserReload,
  LegacyUnknown
};

enum class TransformFallbackPolicy : std::uint8_t {
  NoFallback,
  AllowDynamicAcceleration,
  AllowFullStaticAccelerationBuild,
  AllowFullSceneReload
};

struct InstanceTransformUpdateOptions {
  RenderUpdateReason reason = RenderUpdateReason::Unknown;
  TransformFallbackPolicy fallback_policy = TransformFallbackPolicy::NoFallback;

  bool reset_accumulation = true;
  bool coalesce = true;
  bool allow_partial = false;

  vkpt::core::FrameIndex source_frame = 0;
  const char* source_system = nullptr;
};

struct InstanceTransformCommand {
  std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
  InstanceTransformUpdateOptions options;
};
```

Use strict defaults for motion:

```cpp
TransformFallbackPolicy DefaultTransformPolicy(RenderUpdateReason reason) {
  switch (reason) {
    case RenderUpdateReason::PhysicsMotion:
    case RenderUpdateReason::AnimationMotion:
    case RenderUpdateReason::EditorGizmoMotion:
    case RenderUpdateReason::ScriptTransformMotion:
      return TransformFallbackPolicy::AllowDynamicAcceleration;

    case RenderUpdateReason::StructuralSceneEdit:
    case RenderUpdateReason::SceneLoad:
    case RenderUpdateReason::ExplicitUserReload:
      return TransformFallbackPolicy::AllowFullSceneReload;

    default:
      return TransformFallbackPolicy::NoFallback;
  }
}
```

The legacy overload should **not** silently allow full rebuilds:

```cpp
void RenderCoordinator::post_instance_transforms(
    std::vector<RTInstanceTransformUpdate> updates) {
  PT_LOG_WARN(
      "legacy post_instance_transforms() used without reason/policy; "
      "defaulting to NoFallback");

  post_instance_transforms(
      std::move(updates),
      InstanceTransformUpdateOptions{
        .reason = RenderUpdateReason::LegacyUnknown,
        .fallback_policy = TransformFallbackPolicy::NoFallback,
        .source_system = "legacy"
      });
}
```

A migration-friendly `AllowFullStaticAccelerationBuild` default preserves the exact hidden-FPS-cliff class you are trying to remove.

### 2. Replace boolean result with a work-class result

```cpp
enum class InstanceTransformUpdateStatus : std::uint8_t {
  AppliedMetadataOnly,
  AppliedInstanceBufferOnly,
  AppliedDynamicAccelUpdate,

  BlockedNeedsFullStaticAccelRebuild,
  BlockedNeedsFullSceneReload,

  Unsupported,
  Failed
};

struct InstanceTransformUpdateResult {
  InstanceTransformUpdateStatus status =
      InstanceTransformUpdateStatus::Unsupported;

  std::uint32_t requested_count = 0;
  std::uint32_t applied_count = 0;

  double validate_ms = 0.0;
  double upload_ms = 0.0;
  double dynamic_accel_ms = 0.0;
  double full_rebuild_ms = 0.0;

  const char* message = nullptr;

  bool applied() const {
    return status == InstanceTransformUpdateStatus::AppliedMetadataOnly ||
           status == InstanceTransformUpdateStatus::AppliedInstanceBufferOnly ||
           status == InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate;
  }
};
```

### 3. Prefer plan/apply over one mutating call

This is the cleanest way to enforce policy before mutation.

```cpp
struct InstanceTransformUpdatePlan {
  InstanceTransformUpdateStatus status =
      InstanceTransformUpdateStatus::Unsupported;

  std::uint32_t requested_count = 0;
  std::uint32_t matched_count = 0;

  const char* message = nullptr;

  bool can_apply_without_full_fallback() const {
    return status == InstanceTransformUpdateStatus::AppliedMetadataOnly ||
           status == InstanceTransformUpdateStatus::AppliedInstanceBufferOnly ||
           status == InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate;
  }
};

class IPathTracer {
public:
  virtual InstanceTransformUpdatePlan plan_instance_transform_update(
      std::span<const RTInstanceTransformUpdate> updates,
      const InstanceTransformUpdateOptions& options) const = 0;

  virtual InstanceTransformUpdateResult apply_instance_transform_update(
      std::span<const RTInstanceTransformUpdate> updates,
      const InstanceTransformUpdateOptions& options) = 0;
};
```

Add the same concept to `IRayAccelerator`:

```cpp
class IRayAccelerator {
public:
  virtual InstanceTransformUpdatePlan plan_instance_transform_update(
      const RTSceneData& scene,
      std::span<const RTInstanceTransformUpdate> updates,
      const InstanceTransformUpdateOptions& options) const {
    return {
      .status = InstanceTransformUpdateStatus::Unsupported,
      .requested_count = static_cast<std::uint32_t>(updates.size()),
      .message = "accelerator does not support instance transform updates"
    };
  }

  virtual InstanceTransformUpdateResult apply_instance_transform_update(
      const RTSceneData& scene,
      std::span<const RTInstanceTransformUpdate> updates,
      const InstanceTransformUpdateOptions& options) {
    return {
      .status = InstanceTransformUpdateStatus::Unsupported,
      .requested_count = static_cast<std::uint32_t>(updates.size()),
      .message = "accelerator does not support instance transform updates"
    };
  }

  // Existing build/intersect/build_info/reset remain.
};
```

A one-method API can work only if it guarantees: **non-applied statuses leave state unchanged**. Plan/apply makes that guarantee easier to test.

## RenderCoordinator transaction model

The coordinator should stop calling `ApplyInstanceTransformUpdates(scene, updates)` before backend acceptance. The current helper mutates and may rebake vertices, so it belongs only in validated commit/fallback paths, not speculative planning. ([GitHub][3])

Use this flow:

```cpp
void RenderCoordinator::apply_instance_transform_command(
    IPathTracer& tracer,
    const InstanceTransformCommand& command) {
  const auto& updates = command.updates;
  const auto& options = command.options;

  auto resolved = ResolveInstanceTransformUpdates(scene, updates);
  if (!resolved.ok()) {
    stats.instance_transform_failed++;
    return;
  }

  const auto plan =
      tracer.plan_instance_transform_update(updates, options);

  if (!PolicyAllows(plan.status, options.fallback_policy)) {
    stats.instance_transform_policy_rejections++;

    PT_LOG_WARN(
        "rejected transform update slow path",
        {
          {"reason", ToString(options.reason)},
          {"policy", ToString(options.fallback_policy)},
          {"planned_status", ToString(plan.status)},
          {"updates", std::to_string(updates.size())},
          {"source", options.source_system ? options.source_system : ""}
        });

    mark_render_state_stale(
        RenderStaleReason::TransformUpdateRejectedByPolicy);

    // Critical: no mutation of coordinator scene, tracer, generation, or sample.
    return;
  }

  if (plan.can_apply_without_full_fallback()) {
    const auto result =
        tracer.apply_instance_transform_update(updates, options);

    if (!result.applied()) {
      stats.instance_transform_failed++;

      // Critical: backend contract says failed/non-applied means no mutation.
      mark_render_state_stale(RenderStaleReason::TransformUpdateApplyFailed);
      return;
    }

    // Commit coordinator mirror only after backend commit succeeds.
    ApplyInstanceTransformMetadataOnly(scene, resolved.value());

    ++generation;
    sample = 0;

    if (options.reset_accumulation) {
      tracer.reset_accumulation();
    }

    RecordTransformResultMetrics(result);
    return;
  }

  if (plan.status ==
          InstanceTransformUpdateStatus::BlockedNeedsFullStaticAccelRebuild &&
      options.fallback_policy >=
          TransformFallbackPolicy::AllowFullStaticAccelerationBuild) {
    // Build next state first. Commit only after full rebuild succeeds.
    RTSceneData next_scene = scene;
    ApplyInstanceTransformUpdatesForFullRebuild(next_scene, resolved.value());

    if (!tracer.load_scene_snapshot(next_scene)) {
      mark_render_state_stale(RenderStaleReason::FullRebuildFallbackFailed);
      return;
    }

    if (!tracer.build_or_update_acceleration()) {
      mark_render_state_stale(RenderStaleReason::FullRebuildFallbackFailed);
      return;
    }

    scene = std::move(next_scene);
    ++generation;
    sample = 0;
    tracer.reset_accumulation();

    stats.instance_transform_full_static_accel_fallbacks++;
    return;
  }

  if (plan.status ==
          InstanceTransformUpdateStatus::BlockedNeedsFullSceneReload &&
      options.fallback_policy >= TransformFallbackPolicy::AllowFullSceneReload) {
    // Only explicit scene-load/reload style policies should arrive here.
    reload_scene_from_latest_authoritative_snapshot();
    stats.instance_transform_full_scene_fallbacks++;
    return;
  }
}
```

The important structural change is this:

```text
Before:
  mutate coordinator scene
  mutate backend
  maybe rebuild/fallback

After:
  validate
  plan
  policy-check
  apply backend transaction
  commit coordinator mirror only after backend success
```

## Add separate apply helpers

The current `ApplyInstanceTransformUpdates()` is too broad for fast motion because it can rebake vertices. Split it:

```cpp
struct ResolvedInstanceTransformUpdate {
  std::uint32_t instance_index = kInvalidRTInstanceIndex;
  const RTInstanceTransformUpdate* update = nullptr;
};

std::expected<std::vector<ResolvedInstanceTransformUpdate>, TransformUpdateError>
ResolveInstanceTransformUpdates(
    const RTSceneData& scene,
    std::span<const RTInstanceTransformUpdate> updates);

bool ApplyInstanceTransformMetadataOnly(
    RTSceneData& scene,
    std::span<const ResolvedInstanceTransformUpdate> updates);

bool ApplyInstanceTransformUpdatesForFullRebuild(
    RTSceneData& scene,
    std::span<const ResolvedInstanceTransformUpdate> updates);
```

`ApplyInstanceTransformMetadataOnly()` should update only:

```text
RTInstance.translation
RTInstance.rotation
RTInstance.scale
RTInstance.flags
RTInstance.transform_revision
```

`ApplyInstanceTransformUpdatesForFullRebuild()` may rebake `scene.vertices` from `local_vertices/local_indices`, because that path is explicitly rebuilding full/static acceleration.

This is essential because D3D12 and future CPU dynamic TLAS should not need CPU vertex rebakes for rigid motion.

## CPU backend: honest and atomic immediately

Given the current CPU BVH flattens all instances/triangles into one primitive array and builds one BVH, the current CPU accelerator cannot cheaply update one rigid instance transform. Until dynamic BLAS/TLAS exists, CPU transform updates should plan as needing a full/static acceleration rebuild. ([GitHub][6])

Immediate CPU behavior:

```cpp
InstanceTransformUpdatePlan ScalarCpuPathTracer::plan_instance_transform_update(
    std::span<const RTInstanceTransformUpdate> updates,
    const InstanceTransformUpdateOptions& options) const {
  if (!m_configured || !m_has_scene) {
    return {
      .status = InstanceTransformUpdateStatus::Failed,
      .requested_count = static_cast<std::uint32_t>(updates.size()),
      .message = "CPU tracer not configured or no scene loaded"
    };
  }

  const auto resolved = ResolveInstanceTransformUpdates(m_scene, updates);
  if (!resolved) {
    return {
      .status = InstanceTransformUpdateStatus::Failed,
      .requested_count = static_cast<std::uint32_t>(updates.size()),
      .message = "invalid instance transform update"
    };
  }

  const IRayAccelerator* accelerator =
      m_external_accelerator ? m_external_accelerator : m_accelerator.get();

  if (accelerator) {
    const auto accel_plan =
        accelerator->plan_instance_transform_update(m_scene, updates, options);

    if (accel_plan.can_apply_without_full_fallback()) {
      return accel_plan;
    }

    if (accel_plan.status != InstanceTransformUpdateStatus::Unsupported) {
      return accel_plan;
    }
  }

  return {
    .status = InstanceTransformUpdateStatus::BlockedNeedsFullStaticAccelRebuild,
    .requested_count = static_cast<std::uint32_t>(updates.size()),
    .matched_count = static_cast<std::uint32_t>(resolved->size()),
    .message = "CPU accelerator lacks dynamic instance TLAS support"
  };
}
```

Apply should mutate only when the plan is applicable:

```cpp
InstanceTransformUpdateResult ScalarCpuPathTracer::apply_instance_transform_update(
    std::span<const RTInstanceTransformUpdate> updates,
    const InstanceTransformUpdateOptions& options) {
  const auto plan = plan_instance_transform_update(updates, options);

  if (!plan.can_apply_without_full_fallback()) {
    return {
      .status = plan.status,
      .requested_count = plan.requested_count,
      .message = plan.message
    };
  }

  IRayAccelerator* accelerator =
      m_external_accelerator ? m_external_accelerator : m_accelerator.get();

  if (!accelerator) {
    return {
      .status = InstanceTransformUpdateStatus::Unsupported,
      .requested_count = static_cast<std::uint32_t>(updates.size()),
      .message = "no CPU accelerator available"
    };
  }

  const auto result =
      accelerator->apply_instance_transform_update(m_scene, updates, options);

  if (!result.applied()) {
    // Contract: accelerator did not mutate.
    return result;
  }

  const auto resolved = ResolveInstanceTransformUpdates(m_scene, updates);
  if (!resolved) {
    return {
      .status = InstanceTransformUpdateStatus::Failed,
      .requested_count = static_cast<std::uint32_t>(updates.size()),
      .message = "failed to resolve updates after accelerator commit"
    };
  }

  // Commit CPU tracer scene mirror only after accelerator commit succeeds.
  ApplyInstanceTransformMetadataOnly(m_scene, resolved.value());
  m_accel_info = accelerator->build_info();

  return result;
}
```

Also fix the current external-accelerator behavior. Returning `m_external_accelerator->build_info().built` is not a transform update. If the external accelerator has no transform-update API, CPU should return `Unsupported` or `BlockedNeedsFullStaticAccelRebuild`, not success. ([GitHub][4])

## CPU backend: fast later

Once the honest API is in place, implement CPU dynamic acceleration using the data already present in `RTInstance` and scene conversion:

```text
Static CPU BVH:
  static/global flattened triangles
  rebuilt only on structural/geometry changes

Dynamic geometry BLAS:
  local-space geometry from scene.local_vertices/local_indices
  built per mesh/geometry

Dynamic instance TLAS:
  one proxy per dynamic instance
  world AABB derived from TRS
  refit/rebuild only dynamic instance bounds
```

The current scene conversion already records dynamic flags, physics-controlled flags, local vertex/index ranges, and TRS for dynamic instances, so this is compatible with the existing data model. ([GitHub][7])

CPU dynamic update status should then become:

```cpp
InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate
```

where “dynamic accel update” means dynamic TLAS/BVH refit or dynamic-only rebuild, not full/static acceleration rebuild.

## D3D12 backend: classify correctly and make atomic

D3D12 should return:

```cpp
InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate
```

because it updates dynamic instance data and dynamic GPU acceleration/TLAS-style structures, not the full/static scene acceleration. ([GitHub][5])

But make it transactional. Current transform update mutates mirrors before all work has succeeded. Instead, copy the scene-delta pattern already present in D3D12:

```cpp
InstanceTransformUpdateResult D3D12GpuPathTracer::apply_instance_transform_update(
    std::span<const RTInstanceTransformUpdate> updates,
    const InstanceTransformUpdateOptions& options) {
  if (!m_sceneUploaded || !m_instanceBuffer || !m_dynamicTransformsEnabled) {
    return {
      .status = InstanceTransformUpdateStatus::Unsupported,
      .requested_count = static_cast<std::uint32_t>(updates.size()),
      .message = "D3D12 dynamic transforms unavailable"
    };
  }

  auto nextScene = m_sceneData;
  auto nextGpuInsts = m_gpuInsts;

  auto resolved = ResolveInstanceTransformUpdates(nextScene, updates);
  if (!resolved) {
    return {
      .status = InstanceTransformUpdateStatus::Failed,
      .requested_count = static_cast<std::uint32_t>(updates.size()),
      .message = "invalid D3D12 transform update"
    };
  }

  ApplyInstanceTransformMetadataOnly(nextScene, resolved.value());
  PackUpdatedGpuInstances(nextScene, resolved.value(), nextGpuInsts);

  DynamicBvhBuildResult nextDynamicBvh =
      BuildDynamicInstanceBvhStaged(nextGpuInsts);

  if (!nextDynamicBvh.ok()) {
    return {
      .status = InstanceTransformUpdateStatus::Failed,
      .requested_count = static_cast<std::uint32_t>(updates.size()),
      .message = "failed to build staged dynamic BVH"
    };
  }

  if (!UploadStagedDynamicTransformState(
          nextGpuInsts,
          nextDynamicBvh,
          options)) {
    return {
      .status = InstanceTransformUpdateStatus::Failed,
      .requested_count = static_cast<std::uint32_t>(updates.size()),
      .message = "failed to upload staged dynamic transform state"
    };
  }

  // Commit only after all staged work succeeds.
  m_sceneData = std::move(nextScene);
  m_gpuInsts = std::move(nextGpuInsts);
  m_gpuDynamicBvh = std::move(nextDynamicBvh.bvh);

  invalidate_temporal_history();

  return {
    .status = InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate,
    .requested_count = static_cast<std::uint32_t>(updates.size()),
    .applied_count = static_cast<std::uint32_t>(resolved->size()),
    .message = "D3D12 dynamic transform update committed"
  };
}
```

This mirrors the existing `update_scene_delta()` philosophy: mutate a copy, upload, then commit. ([GitHub][5])

## Qt physics path: stop policy-forbidden reloads

The Qt physics path now has a delta route, but it can still fall back to `qtReloadEditedScene("physics simulation")` if transform publishing fails. For `PhysicsMotion`, that should no longer be an automatic fallback. The current path steps physics, extracts writes, builds pending transform updates, tries to publish them, and reloads edited scene when publish fails. ([GitHub][8])

Change the Qt publish call to pass reason/policy:

```cpp
qtPublishInstanceTransformUpdates(
    std::move(updates),
    InstanceTransformUpdateOptions{
      .reason = RenderUpdateReason::PhysicsMotion,
      .fallback_policy = TransformFallbackPolicy::AllowDynamicAcceleration,
      .reset_accumulation = true,
      .source_system = "qt-physics"
    });
```

Then handle rejected/blocked results like this:

```text
PhysicsMotion + AppliedDynamicAccelUpdate:
  normal, continue.

PhysicsMotion + BlockedNeedsFullStaticAccelRebuild:
  do not reload scene automatically.
  mark render preview stale or show “CPU backend lacks fast dynamic transform support.”

PhysicsMotion + BlockedNeedsFullSceneReload:
  do not reload scene automatically.

Manual reload / scene load:
  may use AllowFullSceneReload.
```

That prevents the old FPS cliff from reappearing through the UI fallback path.

## ECS and animation movement path

The same policy model should be used for animation and editor movement.

```text
Animation:
  writes local transforms
  reason = AnimationMotion
  fallback_policy = AllowDynamicAcceleration

Physics:
  writes world transforms
  reason = PhysicsMotion
  fallback_policy = AllowDynamicAcceleration

Editor gizmo drag:
  usually writes world transforms
  reason = EditorGizmoMotion
  fallback_policy = AllowDynamicAcceleration while dragging

Script transform motion:
  explicit local/world transform choice
  reason = ScriptTransformMotion
  fallback_policy = AllowDynamicAcceleration by default
```

The ECS should output `TransformDeltaBatch`, and render sync should convert deltas to `RTInstanceTransformUpdate` without building a full scene snapshot.

The next structural ECS fixes remain valid:

1. Replace ambiguous `set_transform()` hot-path use with explicit `set_local_transform()` and `set_world_transform()`.
2. Replace global transform recompute with dirty-subtree recompute. The current recompute path clears the whole world-transform cache and loops entity order, which is exactly what one moving object should not do. ([GitHub][9])
3. Split scene hashes/revisions so transform motion does not alter structural invalidation. Current snapshot hashing includes transform translation in the blob, so transform-only motion can look structural to snapshot consumers. ([GitHub][9])

## Precise rebuild terminology

Use these terms consistently:

```text
Instance buffer update:
  Updates TRS/matrix/flags/material/index data for instances.
  Allowed for per-frame motion.

Dynamic acceleration update:
  Dynamic instance bounds update, dynamic TLAS/BVH refit,
  or dynamic-only TLAS/BVH rebuild.
  Allowed for physics, animation, editor drag, and script motion.

Full/static acceleration rebuild:
  Rebuilds the main static/global triangle BVH, static BLAS,
  or full-scene acceleration structure.
  Forbidden for per-frame motion unless explicit policy allows it.

Full scene reload:
  Replaces RTSceneData/load_scene_snapshot/post_scene path.
  Forbidden for per-frame motion unless explicit policy allows it.

CPU vertex rebake:
  Writes transformed local geometry into scene.vertices.
  Treat as full/static rebuild-class work, not fast motion work.
```

D3D12 dynamic TLAS/BVH work is allowed motion work. CPU’s current full flattened BVH rebuild is not.

## Tests that will catch the real regressions

Add these first:

```text
1. CPU rejected update is atomic
   Backend: scalar CPU without dynamic TLAS.
   Reason: PhysicsMotion.
   Policy: AllowDynamicAcceleration.
   Expected:
     plan = BlockedNeedsFullStaticAccelRebuild.
     coordinator scene unchanged.
     tracer m_scene unchanged.
     accelerator build_info/build count unchanged.
     no load_scene_snapshot().
     no build_or_update_acceleration().

2. CPU external accelerator is not fake success
   External accelerator has build_info().built = true but no transform API.
   Expected:
     transform update returns Unsupported or BlockedNeedsFullStaticAccelRebuild.
     no m_scene mutation.

3. RenderCoordinator rejection is atomic
   Invalid or policy-forbidden transform update.
   Expected:
     scene generation unchanged.
     sample unchanged.
     committed scene mirror unchanged.
     stale reason recorded.

4. D3D12 upload failure is atomic
   Inject failure after staging dynamic BVH but before upload/commit.
   Expected:
     m_sceneData unchanged.
     m_gpuInsts unchanged.
     dynamic BVH/TLAS mirror unchanged.
     result = Failed.

5. D3D12 dynamic update classification
   Move one dynamic physics-controlled instance.
   Expected:
     result = AppliedDynamicAccelUpdate.
     no full scene reload.
     no full/static acceleration rebuild.

6. No CPU vertex rebake on fast transform path
   PhysicsMotion transform update.
   Expected:
     ApplyInstanceTransformMetadataOnly called.
     ApplyInstanceTransformUpdatesForFullRebuild not called.

7. Qt physics does not auto-reload on CPU blocked motion
   CPU backend lacks dynamic TLAS.
   Physics body moves.
   Expected:
     policy rejection or stale preview.
     qtReloadEditedScene("physics simulation") not called automatically.

8. Scene hash split
   Transform-only motion.
   Expected:
     structural_hash unchanged.
     transform_revision increments.

9. Dirty transform recompute
   10,000 entities, move one leaf.
   Expected:
     recomputed world transforms = 1 plus descendants, not all entities.
```

## Final implementation order

1. **Add result/status/options types.** Include `RenderUpdateReason`, `TransformFallbackPolicy`, `InstanceTransformUpdatePlan`, and `InstanceTransformUpdateResult`.

2. **Change `post_instance_transforms()` to carry policy.** Legacy overload defaults to `NoFallback` and logs loudly.

3. **Split transform apply helpers.** Add resolve/validate, metadata-only apply, and full-rebuild apply. Stop using the current broad `ApplyInstanceTransformUpdates()` on speculative fast paths.

4. **Make `RenderCoordinator` transactional.** Plan first, policy-check second, backend apply third, coordinator scene commit last.

5. **Make scalar CPU honest and atomic.** No mutation when dynamic update is unsupported. No hidden `build_or_update_acceleration()` inside transform update. No fake success from external accelerator `build_info()`.

6. **Make D3D12 transform update staged/atomic.** Copy the existing `update_scene_delta()` style: apply to copies, upload/build dynamic state, commit mirrors only on success.

7. **Update Qt physics publishing.** Physics motion gets `AllowDynamicAcceleration`; no automatic full scene reload on blocked CPU dynamic motion.

8. **Implement CPU dynamic BLAS/TLAS.** Use existing dynamic flags and local geometry ranges. Then CPU can return `AppliedDynamicAccelUpdate`.

9. **Split scene revisions/hashes.** Transform motion changes transform revision, not structural hash.

10. **Replace global transform recompute.** Dirty-subtree recompute only.

The decisive upgrade is this:

```text
Backends report what work is required.
RenderCoordinator decides whether that work is legal.
No state changes until the legal path is known.
No rejected path mutates anything.
```

That turns motion handling from a hidden side-effect chain into an explicit, enforceable transaction.

[1]: https://github.com/monstercameron/vkPathTracer/blob/main/src/pathtracer/PathTracer.h "vkPathTracer/src/pathtracer/PathTracer.h at main · monstercameron/vkPathTracer · GitHub"
[2]: https://github.com/monstercameron/vkPathTracer/blob/main/src/render/RenderCoordinator.cpp "vkPathTracer/src/render/RenderCoordinator.cpp at main · monstercameron/vkPathTracer · GitHub"
[3]: https://github.com/monstercameron/vkPathTracer/blob/main/src/pathtracer/PathTracer.cpp "vkPathTracer/src/pathtracer/PathTracer.cpp at main · monstercameron/vkPathTracer · GitHub"
[4]: https://github.com/monstercameron/vkPathTracer/blob/main/src/pathtracer/ScalarCpuPathTracer.cpp "vkPathTracer/src/pathtracer/ScalarCpuPathTracer.cpp at main · monstercameron/vkPathTracer · GitHub"
[5]: https://github.com/monstercameron/vkPathTracer/blob/main/src/gpu/D3D12GpuPathTracer.Scene.cpp "vkPathTracer/src/gpu/D3D12GpuPathTracer.Scene.cpp at main · monstercameron/vkPathTracer · GitHub"
[6]: https://github.com/monstercameron/vkPathTracer/blob/main/src/pathtracer/CpuBvhAccelerator.cpp "vkPathTracer/src/pathtracer/CpuBvhAccelerator.cpp at main · monstercameron/vkPathTracer · GitHub"
[7]: https://github.com/monstercameron/vkPathTracer/blob/main/src/pathtracer/SceneConversion.cpp "vkPathTracer/src/pathtracer/SceneConversion.cpp at main · monstercameron/vkPathTracer · GitHub"
[8]: https://github.com/monstercameron/vkPathTracer/blob/main/src/app/AppRuntimeQtDockSyncAndPhysics.inc "vkPathTracer/src/app/AppRuntimeQtDockSyncAndPhysics.inc at main · monstercameron/vkPathTracer · GitHub"
[9]: https://github.com/monstercameron/vkPathTracer/blob/main/src/scene/SceneWorldExtraction.cpp "vkPathTracer/src/scene/SceneWorldExtraction.cpp at main · monstercameron/vkPathTracer · GitHub"
