I think the FPS cliff is **not caused by “world-space movement” itself**. It is caused by the architecture treating a moving physics object like a broad scene mutation. One body moves, then the engine appears to cascade into ECS-wide transform invalidation, full scene extraction/hash work, and often full acceleration rebuilds.

I’m basing this on source inspection, not a profiler capture, but the code paths are strong enough to explain the behavior.

## What is probably happening

### 1. Physics sync scans the whole ECS every tick

`BuildPhysicsBodySyncList()` reserves against `world.all_entities().size()` and iterates every entity, then copies the cached `world_transform` into the physics sync record when available. That means even if one physics object moves, the sync stage starts with an **O(total entities)** scan. ([GitHub][1])

That is not necessarily fatal by itself, but it is the first sign that movement is not being processed as a narrow delta.

### 2. Physics calls are serialized through a worker and block the caller

`ThreadedPhysicsWorld` wraps calls like `sync_from_scene_world()`, `step_fixed()`, and `extract_transform_writes()` through `run_on_worker()`, and `run_on_worker()` waits on `future.get()`. So the calling thread can pay multiple hard sync points per frame: sync, step, extract. ([GitHub][1])

This means the architecture is not really “physics runs independently.” It is closer to:

```text
main/render/update thread
  -> build full physics sync list
  -> block on physics worker sync
  -> block on physics worker step
  -> block on physics worker write extraction
```

That can easily amplify the cost of movement.

### 3. A physics transform write marks ECS transforms dirty recursively

`SceneWorld::set_transform()` stores the transform and calls `mark_dirty_recursive(id)`. The recursive dirtying erases cached world transforms for the entity and all children. ([GitHub][2]) ([GitHub][3])

That makes sense for hierarchical transforms, but the next stage is the expensive part.

### 4. World transform recomputation clears the entire cache

`recompute_world_transforms()` clears `m_worldTransforms`, reserves for the full entity order, then iterates the whole entity list. Because the cache was just cleared, every transformed entity effectively becomes a cache miss. ([GitHub][2])

This is a major problem.

A single moving physics object should cause:

```text
recompute that object
recompute its children
maybe update its render instance
```

The current path looks more like:

```text
one object moved
clear all cached world transforms
iterate all entities
recompute all transforms
```

That alone can tank FPS in a scene with many entities.

### 5. Render extraction appears to rebuild a complete scene snapshot

`build_snapshot()` loops through the world’s entity order, extracts renderables/lights/materials/camera, and includes world transform data in the snapshot/hash blob. Then `extract_render_scene()` calls `build_snapshot()` and copies full render scene data out into `RTSceneData`. ([GitHub][2]) ([GitHub][2]) ([GitHub][2]) ([GitHub][2])

This is likely the big architectural bug: **world transform changes are being treated as scene snapshot changes**.

If a physics body moves every frame, the world translation in the scene hash changes every frame. That can make the system believe the scene changed structurally, even though only an instance transform changed.

### 6. RenderCoordinator has an instance-transform path, but full-scene rebuilds are still reachable

`RenderCoordinator` has `post_instance_transforms()`, which is the right direction. But `post_scene()` resets pending deltas and transform updates, and full scene commands call `load_scene_snapshot()`, `build_or_update_acceleration()`, and `reset_accumulation()`. ([GitHub][4]) ([GitHub][4]) ([GitHub][4])

So if the physics movement path posts a full scene instead of an instance transform batch, every physics step can trigger:

```text
copy whole RTSceneData
rebuild acceleration
reset accumulation
```

That is exactly the kind of thing that would cause a dramatic FPS collapse.

### 7. Even the CPU “instance transform” path rebuilds the accelerator

This is probably the most important smoking gun for the CPU tracer: `ScalarCpuPathTracer::update_instance_transforms()` applies the instance transform updates, then calls `build_or_update_acceleration()` unless an external accelerator is present. `build_or_update_acceleration()` calls `accelerator->build(m_scene, ...)`, which is a full accelerator build. ([GitHub][5]) ([GitHub][5])

So even if you correctly use `post_instance_transforms()`, the CPU path may still rebuild the BVH every time a physics object moves.

That means the current architecture has two possible slow paths:

```text
Bad path:
physics moved -> extract full scene -> post_scene -> full acceleration rebuild

Still-bad CPU path:
physics moved -> post_instance_transforms -> ScalarCpuPathTracer::update_instance_transforms -> full acceleration rebuild
```

### 8. There may also be a physics feedback loop

The Jolt sync path compares the current physics pose to the ECS transform. If the pose changed, kinematic bodies are moved, and dynamic bodies can be forcibly repositioned, have linear/angular velocity reset, sleep timer reset, and be activated. ([GitHub][1])

For dynamic bodies, this is the wrong ownership model. A dynamic physics body should not be driven back from ECS every frame. Physics should own it after creation, except for explicit teleports. Otherwise you can get:

```text
physics simulates body
ECS receives body transform
next frame ECS sync sends transform back to physics
physics treats it as external correction
body wakes / velocity resets / more work
```

That can cause both performance problems and bad simulation behavior.

---

# Root cause summary

The movement itself is cheap. The expensive part is the cascade:

```text
Physics body moves in world space
  ↓
Physics writes transform into ECS
  ↓
ECS marks transform dirty
  ↓
World transform cache is cleared and recomputed globally
  ↓
Scene snapshot/extraction/hash sees changed world transform
  ↓
Render side receives full scene or transform update
  ↓
Acceleration structure is rebuilt
  ↓
Accumulation is reset
  ↓
FPS tanks
```

The architecture needs to distinguish **structural scene changes** from **runtime motion**.

Right now, transform motion is too entangled with full scene extraction and full acceleration rebuilds.

---

# Better architecture

## 1. Split the scene into three separate domains

Do not let one `SceneWorld` snapshot represent everything equally.

Use three explicit domains:

```cpp
struct SceneStructure {
  // Rarely changes.
  // Entity creation/destruction.
  // Mesh assignment.
  // Material assignment.
  // Light creation/destruction.
  // Physics body creation/destruction.
  uint64_t structural_revision;
};

struct TransformStore {
  // Changes often.
  // Local transforms.
  // World transforms.
  // Parent/child links.
  // Dirty transform set.
  uint64_t transform_revision;
};

struct RenderResidentScene {
  // Renderer-owned persistent resources.
  // Mesh BLAS handles.
  // Material buffers.
  // Entity -> instance index map.
  // Instance transform buffer.
  uint64_t uploaded_structural_revision;
  uint64_t uploaded_transform_revision;
};
```

The critical rule:

```text
Moving an object increments transform_revision only.
It must not increment structural_revision.
```

Structural updates are expensive. Transform updates are expected every frame.

## 2. Introduce explicit transform-space APIs

The current `set_transform()` name is too ambiguous. Physics and editor gizmos usually produce world-space transforms. Animation usually produces local-space transforms. Stored ECS transforms appear to be local-space.

Use this instead:

```cpp
enum class TransformSpace {
  Local,
  World
};

SceneWriteResult set_local_transform(
    EntityId entity,
    const TransformComponent& local,
    TransformAuthority authority);

SceneWriteResult set_world_transform(
    EntityId entity,
    const TransformComponent& world,
    TransformAuthority authority);
```

`set_world_transform()` should convert world to local if the entity has a parent:

```cpp
local = inverse(parent_world) * desired_world;
```

Then physics writeback should always call:

```cpp
world.set_world_transform(
    entity,
    physics_world_transform,
    TransformAuthority::PhysicsControlled);
```

This fixes correctness and makes the dirty propagation explicit.

## 3. Replace global transform recompute with dirty-subtree recompute

Do not clear the entire world transform cache in `recompute_world_transforms()`.

Use a dirty queue:

```cpp
class TransformStore {
public:
  void mark_dirty(EntityId entity);
  void recompute_dirty();

private:
  std::vector<EntityId> dirty_roots;
  std::vector<TransformComponent> local;
  std::vector<WorldTransform> world;
  std::vector<EntityId> parent;
  std::vector<std::vector<EntityId>> children;
};
```

Pseudo-flow:

```cpp
void TransformStore::mark_dirty(EntityId entity) {
  if (!dirty[entity]) {
    dirty[entity] = true;
    dirty_roots.push_back(entity);
  }
}

void TransformStore::recompute_dirty() {
  sort_roots_parent_before_child(dirty_roots);

  for (EntityId root : dirty_roots) {
    recompute_subtree(root);
  }

  dirty_roots.clear();
}
```

A single moving object should touch:

```text
the object
its descendants
its render instance
its physics proxy if needed
```

It should not touch every entity in the scene.

## 4. Physics should not sync every entity every frame

Replace `BuildPhysicsBodySyncList(world)` with a persistent physics body registry and dirty command queue.

Current model:

```text
Every frame:
  scan all ECS entities
  build full sync list
  send full sync list to physics
```

Better model:

```text
On body creation:
  PhysicsCreateBodyCommand

On body destruction:
  PhysicsDestroyBodyCommand

On collider/mass/mode change:
  PhysicsUpdateBodyConfigCommand

On kinematic/editor target move:
  PhysicsSetKinematicTargetCommand

On dynamic body simulation:
  physics publishes transform delta
```

Example:

```cpp
struct PhysicsCommandBuffer {
  std::vector<CreateBody> creates;
  std::vector<DestroyBody> destroys;
  std::vector<UpdateBodyConfig> config_updates;
  std::vector<SetKinematicTarget> kinematic_targets;
  std::vector<TeleportDynamicBody> teleports;
};
```

Dynamic body ownership should be:

```text
Initial spawn: ECS -> physics
Runtime simulation: physics -> ECS/render
Explicit teleport: ECS -> physics
Normal per-frame sync: no ECS -> physics pose write
```

This avoids the Jolt feedback loop where ECS poses are pushed back into dynamic bodies and can wake/reset them.

## 5. Collapse physics worker calls into one tick

Right now the wrapper can impose multiple blocking calls per frame. Replace:

```cpp
physics.sync_from_scene_world(world);
physics.step_fixed(config);
auto writes = physics.extract_transform_writes();
```

with:

```cpp
PhysicsTickResult result = physics.tick({
  .commands = physics_commands,
  .fixed_dt = fixed_dt,
  .max_substeps = max_substeps
});
```

Internally:

```cpp
PhysicsTickResult PhysicsWorld::tick(const PhysicsTickInput& input) {
  apply_commands(input.commands);
  step_fixed(input.fixed_dt);
  return collect_changed_transforms();
}
```

That reduces cross-thread barriers.

Even better, make it double-buffered:

```text
Frame N:
  main thread submits physics commands for tick N
  physics worker runs tick N
  main thread applies completed result from tick N - 1
```

That changes the frame pipeline to:

```text
Main thread:
  apply previous physics results
  update transforms
  submit render transform deltas
  submit next physics tick

Physics thread:
  process commands
  step simulation
  write results into output buffer
```

This avoids stalling the main/render thread on `future.get()` every frame.

## 6. Render should receive motion deltas, not scene snapshots

The physics path should never call full `extract_render_scene()` or `post_scene()` for ordinary body movement.

Instead:

```cpp
std::vector<RTInstanceTransformUpdate> updates;

for (const PhysicsTransformWrite& write : physics_writes) {
  world.set_world_transform(write.entity, write.transform, TransformAuthority::PhysicsControlled);

  if (auto instance = render_mapping.find_instance(write.entity)) {
    updates.push_back({
      .entity_id = write.entity,
      .instance_index = instance.index,
      .transform = world.world_transform(write.entity)
    });
  }
}

render_coordinator.post_instance_transforms(std::move(updates));
```

Then enforce this invariant:

```text
Physics movement may post instance transforms.
Physics movement may not post a full scene.
```

Add a debug assert/log:

```cpp
if (scene_update_reason == SceneUpdateReason::PhysicsMotion) {
  PT_ASSERT(false && "physics motion must use instance transform updates, not post_scene");
}
```

## 7. Fix CPU acceleration so transform updates do not rebuild the full BVH

This is the second major fix. The current scalar CPU tracer applies instance transform updates and then rebuilds the acceleration structure. ([GitHub][5])

You need a real dynamic-instance acceleration model.

### Current CPU-style model appears effectively like this

```text
RTSceneData contains world-space render primitives
CPU accelerator builds over the scene
Transform changes require rebuilding acceleration
```

### Better CPU model

Use a two-level acceleration structure:

```text
Mesh BLAS:
  Built once per mesh/geometry.
  Object-space triangles.
  Does not change when the instance moves.

Instance TLAS:
  One node/proxy per render instance.
  Stores world transform, inverse transform, world AABB.
  Refit or rebuild only over instances when transforms change.
```

Data structure:

```cpp
struct CpuMeshBlas {
  MeshId mesh;
  std::vector<Triangle> object_space_triangles;
  Bvh object_space_bvh;
};

struct CpuInstance {
  EntityId entity;
  MeshId mesh;
  Mat4 world_from_object;
  Mat4 object_from_world;
  Aabb world_bounds;
};

struct CpuInstanceTlas {
  std::vector<CpuInstance> instances;
  Bvh instance_bvh;
};
```

Ray traversal:

```cpp
bool trace_ray(const Ray& world_ray) {
  for (InstanceHit candidate : tlas.intersect(world_ray)) {
    Ray object_ray = transform_ray(candidate.object_from_world, world_ray);
    blas[candidate.mesh].intersect(object_ray);
  }
}
```

Transform update:

```cpp
bool CpuAccelerator::update_instance_transforms(span<RTInstanceTransformUpdate> updates) {
  for (const auto& update : updates) {
    CpuInstance& instance = instances[update.instance_index];

    instance.world_from_object = update.world_matrix;
    instance.object_from_world = inverse(update.world_matrix);
    instance.world_bounds = transform_aabb(
        blas[instance.mesh].object_bounds,
        instance.world_from_object);
  }

  tlas.refit_or_rebuild_changed_instances(updates);
  return true;
}
```

Important: this should **not** rebuild mesh BLAS.

For small dynamic counts, rebuilding only the TLAS over instances is acceptable. For larger scenes, refit the TLAS bottom-up. Either option is much cheaper than rebuilding a triangle-level BVH every physics tick.

## 8. Separate static and dynamic render acceleration

A practical renderer structure:

```text
Static world:
  static meshes
  static SDFs
  static lights
  static BLAS/TLAS
  rarely rebuilt

Dynamic world:
  moving mesh instances
  moving SDFs
  moving lights
  small dynamic TLAS / instance buffer
  updated every frame
```

Then ray traversal becomes:

```cpp
hit_static = trace_static_accel(ray);
hit_dynamic = trace_dynamic_accel(ray);
return nearest(hit_static, hit_dynamic);
```

This gives you a fast path even before building a perfect refit system.

## 9. Scene hashes should not include transform motion

Right now snapshot construction includes world transform data in the scene blob/hash path. ([GitHub][2])

Split the hashes:

```cpp
struct SceneVersions {
  uint64_t structural_hash; // entity topology, mesh IDs, material IDs, light existence
  uint64_t material_hash;   // material parameter changes
  uint64_t transform_epoch; // increments for motion
};
```

Then:

```text
structural_hash changed -> full scene/resource update
material_hash changed   -> material/light delta update
transform_epoch changed -> instance transform update only
```

Do not use a full snapshot hash to decide whether ordinary physics movement requires full render extraction.

---

# Recommended frame pipeline

Use this frame structure:

```text
1. Input / editor / scripts / animation
   - produce transform intents and physics commands

2. Apply pre-physics ECS commands
   - local/world transform writes
   - kinematic targets
   - explicit teleports
   - body creation/destruction

3. Recompute only dirty transforms
   - no global cache clear

4. Submit physics tick
   - dirty body commands only
   - no full ECS scan
   - preferably async/double-buffered

5. Apply completed physics results
   - world-space transform writes
   - skip sleeping bodies and epsilon-small changes

6. Recompute dirty transforms again
   - only affected physics bodies/subtrees

7. Submit render updates
   - structural changes -> rare full scene update
   - material/light changes -> delta update
   - transform changes -> instance transform batch

8. Render thread
   - update instance buffer / TLAS
   - reset accumulation
   - do not rebuild full scene unless structural revision changed
```

---

# Immediate triage plan

## Step 1: Add instrumentation before changing behavior

Add timers/counters around these specific calls:

```cpp
BuildPhysicsBodySyncList
ThreadedPhysicsWorld::run_on_worker
SceneWorld::set_transform
SceneWorld::mark_dirty_recursive
SceneWorld::recompute_world_transforms
SceneWorld::build_snapshot
SceneWorld::extract_render_scene
RenderCoordinator::post_scene
RenderCoordinator::post_instance_transforms
ScalarCpuPathTracer::update_instance_transforms
ScalarCpuPathTracer::build_or_update_acceleration
```

Also log these per second:

```text
full_scene_posts_per_second
instance_transform_batches_per_second
instance_transform_count_per_second
bvh_full_rebuilds_per_second
world_transform_recompute_count
world_transform_entities_recomputed
physics_entities_scanned
physics_transform_writes
physics_worker_block_ms
```

The smoking gun will probably be one of these:

```text
post_scene called every physics frame
build_snapshot called every physics frame
build_or_update_acceleration called every physics frame
recompute_world_transforms recomputing all entities every physics frame
```

## Step 2: Stop full scene posts for physics motion

Add a hard separation:

```cpp
enum class SceneUpdateReason {
  StructuralChange,
  MaterialChange,
  LightChange,
  CameraChange,
  PhysicsMotion,
  EditorTransformMotion
};
```

Then disallow:

```cpp
post_scene(..., SceneUpdateReason::PhysicsMotion)
```

Physics motion should produce only:

```cpp
post_instance_transforms(...)
```

## Step 3: Fix the CPU transform update path

Right now this is not enough:

```cpp
post_instance_transforms(...)
```

because the scalar CPU path can still call full acceleration rebuild after applying instance transforms. ([GitHub][5])

Add this interface:

```cpp
class IRayAccelerator {
public:
  virtual bool build(const RTSceneData& scene, bool deterministic) = 0;

  virtual bool update_instance_transforms(
      std::span<const RTInstanceTransformUpdate> updates) {
    return false;
  }

  virtual bool supports_fast_instance_updates() const {
    return false;
  }
};
```

Then change `ScalarCpuPathTracer::update_instance_transforms()`:

```cpp
bool ScalarCpuPathTracer::update_instance_transforms(
    const std::vector<RTInstanceTransformUpdate>& updates) {
  if (!m_configured || !m_has_scene) {
    return false;
  }

  if (!ApplyInstanceTransformUpdates(m_scene, updates)) {
    return false;
  }

  IRayAccelerator* accelerator =
      m_external_accelerator ? m_external_accelerator : m_accelerator.get();

  if (accelerator && accelerator->update_instance_transforms(updates)) {
    m_accel_info = accelerator->build_info();
    return true;
  }

  // Temporary fallback only, not acceptable for physics-driven motion.
  return build_or_update_acceleration();
}
```

Then implement a real `update_instance_transforms()` in the CPU BVH accelerator.

## Step 4: Make physics body sync event-driven

Replace the full sync list with dirty body commands:

```cpp
struct PhysicsWorldDelta {
  std::vector<PhysicsCreateBody> creates;
  std::vector<PhysicsDestroyBody> destroys;
  std::vector<PhysicsUpdateShape> shape_updates;
  std::vector<PhysicsSetKinematicTarget> kinematic_targets;
  std::vector<PhysicsTeleportBody> teleports;
};
```

Only send dynamic body transforms into physics when explicitly teleporting. Otherwise dynamic transforms flow from physics to ECS/render.

## Step 5: Fix transform recompute

Change:

```cpp
m_worldTransforms.clear();
for entity in all_entities:
  recompute...
```

to:

```cpp
for dirty_root in dirty_roots:
  recompute_subtree(dirty_root)
```

This should be one of the first code changes because it benefits editor movement, animation, physics, and rendering.

---

# Target architecture

The final structure I would aim for:

```text
SceneWorld
  Owns entity/component data.
  Does not rebuild render scenes on motion.

TransformSystem
  Owns local/world transforms.
  Maintains dirty sets.
  Produces TransformDeltaBatch.

PhysicsSystem
  Owns physics body handles.
  Consumes PhysicsCommandBuffer.
  Produces PhysicsTransformWriteBatch.
  Does not scan all ECS every frame.

RenderSyncSystem
  Maintains entity -> render instance mapping.
  Converts TransformDeltaBatch into RTInstanceTransformUpdate.
  Converts structural changes into rare full scene updates.

RenderCoordinator
  Receives:
    - full scene update only for structural changes
    - material/light deltas for parameter changes
    - instance transform batches for motion

RayAccelerator
  Owns persistent static/dynamic acceleration.
  Supports fast instance transform updates.
```

The key principle:

```text
Motion is not structure.
```

As soon as the engine enforces that rule, physics movement should stop causing catastrophic frame-time spikes.

## The most important fixes, in order

1. **Instrument full-scene updates and BVH rebuilds.** Confirm whether physics movement is causing `post_scene()`, `build_snapshot()`, or `build_or_update_acceleration()` every frame.

2. **Route physics movement through `post_instance_transforms()`, never `post_scene()`.**

3. **Implement real CPU accelerator instance updates.** Do not rebuild the full BVH for a moving physics object.

4. **Stop clearing all world transforms in `recompute_world_transforms()`.** Recompute only dirty subtrees.

5. **Stop scanning all ECS entities for physics sync.** Use physics body command buffers and dirty body lists.

6. **Stop pushing dynamic body poses from ECS back into Jolt every frame.** Physics should own dynamic bodies after creation, except explicit teleports.

7. **Split structural scene revision from transform revision.** Transform changes should not change the structural scene hash.

My strongest recommendation: fix the render/BVH update path first. Even with perfect ECS dirty tracking, FPS will still tank if `ScalarCpuPathTracer::update_instance_transforms()` rebuilds the accelerator every time a physics body moves.

[1]: https://github.com/monstercameron/vkPathTracer/blob/main/src/physics/PhysicsWorld.cpp "vkPathTracer/src/physics/PhysicsWorld.cpp at main · monstercameron/vkPathTracer · GitHub"
[2]: https://github.com/monstercameron/vkPathTracer/blob/main/src/scene/SceneWorldExtraction.cpp "vkPathTracer/src/scene/SceneWorldExtraction.cpp at main · monstercameron/vkPathTracer · GitHub"
[3]: https://github.com/monstercameron/vkPathTracer/blob/main/src/scene/SceneWorld.cpp "vkPathTracer/src/scene/SceneWorld.cpp at main · monstercameron/vkPathTracer · GitHub"
[4]: https://github.com/monstercameron/vkPathTracer/blob/main/src/render/RenderCoordinator.cpp "vkPathTracer/src/render/RenderCoordinator.cpp at main · monstercameron/vkPathTracer · GitHub"
[5]: https://github.com/monstercameron/vkPathTracer/blob/main/src/pathtracer/ScalarCpuPathTracer.cpp "vkPathTracer/src/pathtracer/ScalarCpuPathTracer.cpp at main · monstercameron/vkPathTracer · GitHub"
