# Threading and motion transaction model

This document describes the current render/physics threading model and the
motion update contract. It is intentionally strict about transform updates:
motion must not advance one render mirror while leaving another mirror stale.

## Current threading model

The UI thread owns editor interaction, document mutation, Qt state, and command
publication.

`RenderCoordinator` owns the background render worker. UI code posts commands
through a mutex-protected pending-command mailbox. The render worker drains
commands, mutates its private scene mirror and tracer, renders samples, and
publishes display frames through `FrameHandoff`.

`FrameHandoff` is a single-slot mailbox. Publishing replaces an unacquired
frame and records it as dropped. Acquiring moves the newest frame to the UI.

The physics backend is serialized through a dedicated worker wrapper. Public
physics calls enqueue work and wait for the worker result, so backend physics
state is not touched concurrently with simulation.

Viewport FPS collision has a separate `std::jthread` worker with a pending
request/latest result mailbox. It is independent from render motion updates.

## Motion transaction contract

Transform motion is transactional.

If an update returns an applied status, these mirrors must represent the same
transform revision:

- `RenderCoordinator` scene mirror
- backend scene mirror
- instance buffer mirrors
- dynamic acceleration structures

If an update is rejected, unsupported, failed, or requires fallback work that
policy forbids, none of those committed render-state mirrors may advance.

Non-applied statuses must be side-effect free from the caller's point of view.
Fallback paths that temporarily load a candidate scene must restore the previous
committed snapshot before reporting failure.

## Motion API

Transform commands carry both intent and fallback policy:

- `RenderUpdateReason`
- `TransformFallbackPolicy`
- `InstanceTransformUpdateOptions`
- `InstanceTransformCommand`

The legacy `post_instance_transforms(vector)` overload exists only for
compatibility. It logs a warning and uses `LegacyUnknown + NoFallback`.

Backends expose:

- `plan_instance_transform_update(...)`
- `apply_instance_transform_update(...)`

The coordinator plans first, checks policy second, applies the backend
transaction third, and commits its scene mirror last.

## Current backend behavior

`D3D12GpuPathTracer` stages dynamic transform updates before commit. It copies
scene data, packed instance data, dynamic BVH data, and DXR instance
descriptors, uploads from staged data, and commits backend mirrors only after
the upload/TLAS path succeeds. Successful dynamic motion reports
`AppliedDynamicAccelUpdate`.

`ScalarCpuPathTracer` is honest about current limitations. Without a dynamic
CPU instance TLAS, transform motion plans as
`BlockedNeedsFullStaticAccelRebuild`. The transactional apply path mutates
CPU scene metadata only after an accelerator-level update succeeds.

`VulkanGpuPathTracer` currently treats transform updates as full scene-buffer
rebuild work. It reports that classification through the plan/apply API. Its
legacy boolean path applies into a candidate scene and restores the previous
scene if buffer recreation fails.

## Remaining gaps

The old boolean `update_instance_transforms(...)` virtual still exists for
compatibility. New code should use plan/apply. Long term, remove or restrict
the boolean API so direct callers cannot bypass the transaction policy.

The full static/full scene fallback path in `RenderCoordinator` uses the
current tracer mutation API. On failure it now attempts to restore the previous
committed scene, but a stronger future design would build fallback state in an
isolated tracer or expose a staged backend rebuild API.

The transform apply helper is still broad. Fast paths should continue using
metadata-only apply. A future cleanup should split:

- resolve/validate updates
- metadata-only apply
- full-rebuild apply with CPU vertex rebake

CPU dynamic BLAS/TLAS remains future work. Once implemented, CPU motion should
return `AppliedDynamicAccelUpdate` for dynamic instance bounds/TLAS updates
instead of requiring full static acceleration rebuilds.

Scene hashing and revisions are still mixed. Transform-only motion should not
invalidate structural scene hashes.

World transform recompute still clears the full cache. It should become
dirty-subtree recompute so moving one leaf does not recompute the whole world.

## Policy expectations

Per-frame motion uses:

- physics: `PhysicsMotion + AllowDynamicAcceleration`
- animation: `AnimationMotion + AllowDynamicAcceleration`
- editor gizmo drag: `EditorGizmoMotion + AllowDynamicAcceleration`
- script transform motion: `ScriptTransformMotion + AllowDynamicAcceleration`

These paths must not silently perform full/static acceleration rebuilds or full
scene reloads. Manual scene load/reload paths may use `AllowFullSceneReload`.

Qt physics must not auto-reload the whole edited scene when a physics transform
publish is blocked by policy. It should leave the preview marked stale/blocked
until the backend supports the requested dynamic motion path or the user takes
an explicit reload action.

## Regression tests to add

1. CPU rejected update is atomic.
2. CPU external accelerator cannot fake transform success through `build_info()`.
3. RenderCoordinator rejection leaves generation, sample, and scene unchanged.
4. RenderCoordinator fallback failure restores the previous committed snapshot.
5. D3D12 upload failure leaves scene, instance mirrors, and dynamic BVH/TLAS
   mirrors unchanged.
6. D3D12 dynamic update reports `AppliedDynamicAccelUpdate`.
7. Qt physics blocked motion does not call `qtReloadEditedScene("physics simulation")`.
8. Transform-only changes do not alter structural scene hash.
9. Dirty transform recompute touches only the changed subtree.
