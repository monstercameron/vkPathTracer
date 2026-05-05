# Refined plan: C++23 native pathtracer prototype and benchmark platform

This version keeps the existing plan’s intent intact: **C++23 first, Clang/LLVM first, native desktop backends first, WebGPU/WASM as a web target, not a JavaScript rewrite**. The uploaded plan already establishes the correct foundation: C++23, Clang/LLVM, D3D12/Vulkan/Metal/WebGPU, ECS, deterministic scheduling, Jolt, Lua, asset import, path tracing, scene schema, shader variants, benchmark output, and reproducibility. 

The older WebGL project should remain the **feature DNA**: progressive accumulation, interactive scene editing, many material modes, physics, SDF/primitive creation, benchmark scenes, debug views, PNG export, scene JSON, baseline comparison, rolling benchmark metrics, and browser demo usability. Its README explicitly identifies WebGL as the active renderer, WebGPU as planned, and lists shader/material options, physics, primitive creation, scene panels, benchmark exports, asset-loader scaffolding, Sponza/Suzanne references, and benchmark scenes. ([GitHub][1])

The new project should be framed as:

> A **C++23 / Clang-native, benchmark-ready 3D pathtracer demo platform** that preserves the interactive/material/scene/benchmark spirit of the earlier WebGL project while replacing the JS/WebGL runtime with a deterministic native C++ architecture and backend adapters for Vulkan, D3D12, Metal, and WebGPU/WASM.

Clang documents `-std=c++23` support, but also notes C++23 support is feature-by-feature rather than something to blindly assume across all standard-library combinations; therefore C++23 remains the baseline, while C++2c/C++26 features stay behind explicit gates. ([Clang][2])

---

# 1. Project identity

## 1.1 Product goal

Build a prototype that can operate in two equally important modes:

```text
Interactive demo mode:
  A native, visually rich, editable pathtracing demo with scene controls,
  materials, lights, cameras, physics, scripting, debug views, and exports.

Benchmark mode:
  A deterministic, headless-capable render benchmark harness that produces
  reproducible machine-readable results, image artifacts, reference diffs,
  backend metadata, shader hashes, scene hashes, and performance metrics.
```

## 1.2 Core promise

The project should prove that the earlier browser pathtracer concept can become a more serious native C++ rendering/benchmark platform without losing the interactive “demo toy” charm.

```text
Old project:
  JavaScript + WebGL browser pathtracing benchmark/demo.

New project:
  C++23 + Clang/LLVM native pathtracing benchmark/demo,
  with WebGPU/WASM as one deployment target.
```

## 1.3 Non-goals for the prototype

The prototype should not try to become a full engine immediately.

```text
Not required for first benchmark-ready prototype:
  full production game engine
  full offline renderer
  physically perfect spectral renderer
  full USD authoring environment
  complete editor replacement for DCC tools
  multi-GPU production scheduler
  full hardware ray tracing parity across APIs
  all material packs implemented at once
```

The prototype should instead become a **cleanly structured benchmarkable renderer** with enough editor/demo features to feel like the native successor to the old project.

---

# 2. Architectural principles

## 2.1 C++ source of truth

```text
Primary runtime:
  C++23.

Core engine:
  C++.

Scene runtime:
  C++.

Renderer:
  C++.

Benchmark harness:
  C++.

Editor/demo shell:
  C++ native shell first.

Web target:
  C++ compiled to WebAssembly through Emscripten,
  with WebGPU/WGSL backend support.
```

Emscripten is an LLVM/Clang-based toolchain that compiles C/C++ into WebAssembly and can run the result in browsers and other Wasm runtimes, so it fits the C++-first web deployment model. ([Emscripten][3])

## 2.2 Control before features

Before adding many shaders and materials, define the control structures:

```text
application lifecycle
mode switching
scene lifecycle
ECS phase schedule
resource lifetime
asset import lifecycle
render-frame lifecycle
pathtracing pass lifecycle
shader variant lifecycle
benchmark run lifecycle
editor command lifecycle
physics/script/animation transform arbitration
diagnostics and failure routing
```

The project should feel feature-rich, but the feature richness must sit on stable control surfaces.

## 2.3 Descriptors and handles over direct ownership

High-level API structure should prefer:

```text
descriptors
handles
stable IDs
registries
facades
command buffers
explicit lifetime states
structured diagnostics
```

Avoid leaking backend-specific objects into scene, material, editor, benchmark, or script APIs.

## 2.4 Determinism is a mode, not a vague goal

The system should explicitly distinguish:

```text
Strict deterministic mode:
  benchmark, tests, reference renders.

Interactive deterministic mode:
  fixed-step update with stable command ordering,
  but live editing is allowed.

Free interactive mode:
  editor/demo convenience over strict reproducibility.
```

Jolt’s documentation states deterministic simulation requires simulation-modifying APIs to be called in the same order and the same binary code to be used; it also provides a CMake option for cross-platform deterministic behavior with a performance cost. This should be reflected in physics benchmark policy rather than assumed automatically. ([Jrouwe][4])

---

# 3. High-level module map

## 3.1 Primary modules

```text
Core
Platform
Application
Diagnostics
Serialization
Schema
Assets
Scene
ECS
Animation
Physics
Scripting
Renderer
PathTracer
Materials
Lights
Camera
FrameGraph
ShaderSystem
Benchmark
Editor
Tools
WebRuntime
```

## 3.2 Module dependency direction

```text
Core
  <- Platform
  <- Serialization
  <- Diagnostics
  <- Assets
  <- Scene
  <- ECS
  <- Renderer
  <- Physics
  <- Scripting
  <- Animation
  <- Benchmark
  <- Editor

Scene/ECS
  -> Assets
  -> Serialization
  -> Diagnostics
  -> Core

Renderer
  -> Assets
  -> Materials
  -> Lights
  -> Camera
  -> ShaderSystem
  -> FrameGraph
  -> Core

PathTracer
  -> Renderer
  -> Materials
  -> Lights
  -> Camera
  -> Scene render proxy
  -> Benchmark counters

Editor
  -> Scene
  -> ECS
  -> Assets
  -> Renderer
  -> Benchmark
  -> Scripting
  -> Physics
  -> Animation

Benchmark
  -> Scene
  -> Renderer
  -> PathTracer
  -> Assets
  -> Serialization
  -> Diagnostics
```

## 3.3 Hard separation rule

```text
Scene code must not know Vulkan/D3D12/Metal/WebGPU internals.
Material descriptors must not know native GPU object types.
Benchmark code must not depend on editor UI.
Renderer backend code must not own scene identity.
Physics must not own ECS identity.
Scripts must not mutate world state directly outside scheduled command phases.
Editor commands must be replayable, serializable, and diagnosable.
```

---

# 4. Control structures

This is the most important refinement. The project needs formal control structures so every feature has a place to attach.

---

## 4.1 Application lifecycle control

The application should be driven by a single lifecycle contract.

```text
Application lifecycle:
  Configure
  CreateServices
  RegisterModules
  SelectMode
  SelectBackend
  LoadAssets
  LoadScene
  CreateRuntimeWorld
  Warmup
  Run
  ExportArtifacts
  Shutdown
```

## 4.2 Application modes

```text
DemoMode
  interactive pathtracing viewport
  camera controls
  material/light controls
  scene presets
  benchmark panel
  exports

EditorMode
  scene graph
  inspector
  gizmos
  asset browser
  material editor
  command history
  timeline
  script attachment
  physics authoring

BenchmarkMode
  headless-capable
  deterministic
  fixed seed
  fixed scene hash
  fixed sample schedule
  structured result export

ValidationMode
  scene schema validation
  shader variant validation
  asset import validation
  material mapping validation
  backend capability validation

ReferenceMode
  CPU or stable renderer reference image generation
  golden-image update workflow
  diff artifact generation

WebMode
  browser canvas
  WebGPU/WGSL profile
  reduced/declared capability set
  browser-safe asset and export flow
```

## 4.3 Mode transition rules

```text
Demo -> Benchmark:
  freeze scene
  resolve pending editor commands
  disable live UI mutation
  optionally disable scripts/physics
  record deterministic seed and scene hash

Editor -> Demo:
  apply pending commands
  validate scene
  instantiate runtime objects
  enable live accumulation reset logic

Demo -> Editor:
  preserve runtime state if allowed
  pause benchmark counters
  expose scene graph and material handles

Benchmark -> Demo:
  release strict timing ownership
  restore viewport controls
  retain last benchmark artifacts

Any mode -> Shutdown:
  flush diagnostics
  finish or cancel resource uploads
  serialize crash-safe editor recovery state
```

---

# 5. Main loop control

## 5.1 Universal frame lifecycle

```text
FrameBegin
  poll platform events
  update timers
  start diagnostics scope
  acquire backend frame token

InputPhase
  collect input
  normalize input events
  route to UI/editor/camera/script

CommandPhase
  collect editor commands
  collect script commands
  collect benchmark commands
  collect async asset completion commands

FixedUpdatePhase
  run fixed-step physics
  run fixed-step scripts
  sample fixed-step animations

VariableUpdatePhase
  update camera
  update UI state
  update non-fixed scripts
  update animation playback state
  update benchmark controller

TransformAssemblyPhase
  resolve physics/animation/script/editor transform writes
  rebuild transform hierarchy
  emit transform dirty ranges

SceneMutationApplyPhase
  apply deferred ECS command buffers
  update entity/component lifetime states
  resolve asset/material bindings

RenderPreparationPhase
  build render scene proxy
  collect dirty GPU resources
  schedule uploads
  determine accumulation reset
  build frame graph

RenderSubmitPhase
  submit path tracing passes
  submit denoiser/resolve/debug passes
  submit editor overlays when enabled

PresentOrExportPhase
  present viewport
  export image/metadata if requested
  record benchmark counters

FrameEnd
  retire transient resources
  flush per-frame diagnostics
  update rolling metrics
```

## 5.2 Benchmark frame lifecycle

Benchmark mode should use a stricter variant.

```text
BenchmarkConfigure
  parse run descriptor
  validate backend capability
  validate scene
  validate shader variants
  resolve output artifact paths

BenchmarkLoad
  load scene
  load assets
  build canonical scene
  build acceleration structures
  freeze mutation
  record hashes

BenchmarkWarmup
  run fixed warmup frame count or duration
  discard warmup timing unless configured otherwise

BenchmarkMeasure
  run fixed-SPP, fixed-frame, fixed-duration, or threshold mode
  collect GPU timing
  collect CPU timing
  collect memory and resource counters

BenchmarkResolve
  finalize accumulation
  write PNG/EXR
  write diff artifacts when reference exists
  write JSON/CSV/metadata

BenchmarkFinalize
  validate artifact schema
  print summary
  return pass/fail code
```

## 5.3 Web frame lifecycle

Web mode must acknowledge browser constraints without becoming JS-driven.

```text
WebBootstrap
  initialize wasm module
  request WebGPU adapter/device
  validate required features/limits
  bind canvas surface

WebFrame
  receive browser event batch
  call C++ frame tick
  submit WebGPU work
  update browser-visible status/export hooks

WebAssetFlow
  browser fetch or packaged asset access
  async asset staging
  explicit failure diagnostics
  reduced memory budget policy
```

The WebGPU shader path should use WGSL because the W3C WGSL specification defines WGSL as the shader language used by WebGPU applications, with draw and dispatch pipeline entry points. ([W3C][5])

---

# 6. ECS and scene control

## 6.1 Scene ownership layers

```text
SceneDocument
  serialized authoring representation
  stable IDs
  schema version
  asset references
  benchmark metadata
  editor metadata

SceneRuntime
  active loaded scene
  ECS registry
  transform hierarchy
  runtime caches
  script bindings
  physics bindings
  animation bindings

RenderSceneProxy
  renderer-facing immutable snapshot
  canonical mesh/material/light/camera data
  acceleration-structure build inputs

BenchmarkSceneSnapshot
  frozen deterministic render state
  stable hashes
  asset and shader manifests
  sample seed contract
```

## 6.2 Entity identity rules

```text
Every authored scene object has:
  stable scene ID
  stable entity UUID
  optional human-readable name
  optional asset source reference
  optional editor metadata

Runtime entity handles:
  may be compact/transient
  must map back to stable authored ID
  must not be serialized as raw memory identity
```

## 6.3 ECS component families

```text
IdentityComponent
TransformComponent
HierarchyComponent
VisibilityComponent
SelectionComponent
CameraComponent
LightComponent
MeshRendererComponent
SDFPrimitiveComponent
MaterialOverrideComponent
TextureBindingComponent
PhysicsBodyComponent
PhysicsShapeComponent
ScriptComponent
AnimationControllerComponent
AnimatedTransformComponent
MorphTargetWeightsComponent
BenchmarkTagComponent
DebugDrawComponent
EditorLockComponent
MetadataComponent
```

## 6.4 ECS phase schedule

```text
PreFrame
  frame constants
  input collection
  external event ingestion

EditorInput
  gizmos
  selection
  command construction

ScriptEarly
  on_update command collection
  no direct mutation

AnimationSample
  timelines
  clip sampling
  morph target sampling
  material/light parameter tracks

PhysicsFixed
  fixed-step simulation
  trigger/collision events
  physics state output

ScriptFixed
  on_fixed_update
  collision/trigger callbacks

TransformAssembly
  merge transform writers
  rebuild hierarchy
  emit dirty transform events

SceneCommandApply
  apply deferred commands
  create/destroy entities
  attach/detach components
  update material/light/camera fields

RenderExtract
  build render proxy
  produce material/light/camera/geometry dirty sets

UploadSchedule
  schedule resource uploads
  apply resource budget rules

PostFrame
  diagnostics
  lifetime retirements
```

## 6.5 ECS determinism rules

```text
Systems declare read/write intent.
Systems run in declared phase order.
Entities are visited by stable sort key in deterministic modes.
World mutation is deferred through command buffers.
Command buffers are merged by phase, source, timestamp/frame index, and stable command ID.
Conflicts are reported before mutation.
```

---

# 7. Transform arbitration control

This is critical because physics, animation, scripts, and editor tools all want to affect transforms.

## 7.1 Transform write sources

```text
AuthoredTransform
EditorTransform
ScriptTransform
AnimationTransform
PhysicsTransform
ParentHierarchyTransform
CameraRigTransform
BenchmarkOverrideTransform
```

## 7.2 Transform authority model

Use explicit authority policies per entity.

```text
StaticAuthored
  authored transform only unless editor changes it

EditorControlled
  editor commands win while selected/manipulated

PhysicsControlled
  physics state writes final local/world transform

AnimationControlled
  animation samples local transform channels

ScriptControlled
  script may request transform changes through commands

HybridControlled
  declared blend/priority policy required

BenchmarkFrozen
  no live transform changes after snapshot
```

## 7.3 Default priority order

```text
Benchmark freeze
Physics
Animation
Script
Editor
Authored transform
Parent hierarchy
```

For editor ergonomics, active editor gizmo manipulation may temporarily override normal priority, but the override must be visible in diagnostics and must reset accumulation.

## 7.4 Transform conflict diagnostics

```text
Conflict examples:
  script and physics both write world position
  animation and editor both write local rotation
  parent deleted while child receives physics update
  benchmark-frozen entity receives runtime mutation

Required diagnostic fields:
  entity ID
  component ID
  phase
  writer A
  writer B
  selected authority policy
  chosen result
  whether accumulation reset occurred
```

---

# 8. Scene schema control

## 8.1 Scene document sections

```text
schema
metadata
settings
assets
materials
textures
samplers
geometry
sdf_primitives
entities
transforms
cameras
lights
physics
scripts
animations
editor
benchmark
exports
```

## 8.2 Schema governance

```text
Every document has schema_version.
Every migration is explicit.
Unknown required fields block loading.
Unknown optional fields generate diagnostics.
Deprecated fields migrate or warn.
Lossy migrations record compatibility notes.
Benchmark scenes must preserve full metadata.
```

## 8.3 Scene validation gates

```text
Schema validation
Asset reference validation
Material compatibility validation
Texture/sampler validation
Light unit validation
Camera/exposure validation
Physics shape validation
Script binding validation
Animation track validation
Backend capability validation
Benchmark reproducibility validation
```

---

# 9. High-level API structure

These are planned API surfaces and contracts, not implementation details.

---

## 9.1 Core API

```text
EngineDesc
  application name
  version
  build profile
  enabled modules
  deterministic mode
  logging policy
  memory policy

Engine
  create services
  register modules
  select mode
  run frame
  request shutdown
  query diagnostics
  query build metadata

ServiceRegistry
  logging
  filesystem
  timing
  jobs
  diagnostics
  assets
  renderer
  physics
  scripting
  benchmark
```

## 9.2 Module API

```text
IModule
  module name
  module version
  dependencies
  capabilities
  startup
  shutdown
  diagnostics

ModuleRegistry
  register module
  resolve dependencies
  validate versions
  enable/disable module
  report module graph
```

## 9.3 Platform API

```text
IPlatform
  initialize
  poll events
  create window
  create file services
  create timing services
  create clipboard services
  shutdown

IWindow
  title
  size
  framebuffer size
  DPI scale
  focus state
  fullscreen state
  surface handle for backend adapter

IInput
  keyboard state
  mouse state
  touch state
  gamepad state
  normalized event stream

IFileSystem
  read asset
  write artifact
  enumerate directories
  resolve virtual paths
  expose platform-specific diagnostics
```

## 9.4 Application mode API

```text
IApplicationMode
  name
  required modules
  allowed mutation policy
  startup
  enter
  tick
  leave
  shutdown

ModeController
  current mode
  pending transition
  transition validation
  transition diagnostics
```

## 9.5 Scene API

```text
SceneDocument
  serialized source of truth

SceneRuntime
  active ECS/runtime state

SceneLoader
  load document
  validate document
  migrate document
  instantiate runtime

SceneSaver
  serialize document
  export snapshot
  export benchmark scene

SceneHasher
  hash document
  hash canonical runtime state
  hash benchmark snapshot
```

## 9.6 ECS API

```text
EntityRegistry
  create entity
  destroy entity
  lookup stable ID
  lookup runtime handle

ComponentStorage
  attach component
  remove component
  query component
  validate component lifetime

SystemScheduler
  register system
  declare phase
  declare read/write intents
  run phase
  report conflicts

WorldCommandBuffer
  create entity command
  destroy entity command
  set component command
  add/remove component command
  transform command
  material command
  light command
  camera command
```

## 9.7 Asset API

```text
AssetId
  stable URN-style identity

AssetRef
  typed reference to asset

AssetRegistry
  register source
  resolve asset
  query asset metadata
  query import diagnostics

AssetImportRequest
  source URI
  format hint
  import policy
  normalization policy
  target feature set

AssetImportResult
  imported assets
  compatibility warnings
  lossy conversions
  generated materials
  generated textures
  generated meshes
  generated animations
```

## 9.8 Importer API

```text
IAssetImporter
  supported extensions
  supported features
  import policy requirements
  validate source
  import source
  produce diagnostics

ImporterFeatureSet
  mesh
  materials
  textures
  cameras
  lights
  animations
  skinning
  morph targets
  custom attributes
```

## 9.9 Renderer backend API

```text
IRenderBackend
  backend kind
  adapter/device info
  feature set
  limits
  initialize
  create resources
  create pipelines
  submit frame
  readback
  resize
  shutdown

BackendKind
  Vulkan
  D3D12
  Metal
  WebGPU
  CPUReference

BackendCapabilitySet
  compute support
  storage buffers
  storage textures
  subgroup/wave features
  timestamp queries
  texture format support
  bindless-like support
  ray tracing support
  memory budget reporting
```

## 9.10 Render resource API

```text
BufferHandle
TextureHandle
SamplerHandle
PipelineHandle
DescriptorTableHandle
RenderTargetHandle
AccelerationHandle
ReadbackHandle

ResourceDesc
  name
  usage
  size/format
  lifetime
  residency priority
  debug label
```

## 9.11 Frame graph API

```text
FrameGraph
  declare pass
  declare resource reads
  declare resource writes
  declare dependencies
  validate hazards
  produce backend submission plan

FramePass types:
  upload
  compute
  raster
  resolve
  copy
  readback
  debug
```

## 9.12 Shader system API

```text
ShaderLibrary
  register shader family
  register shader entry points
  register shared include/module dependencies

ShaderVariantDesc
  backend
  stage
  material family
  feature flags
  defines/overrides
  resource layout
  target profile

ShaderManifest
  source hash
  variant hash
  backend hash
  compiler flags
  generated artifact metadata
  compatibility notes

ShaderCache
  query
  store
  invalidate
  explain miss
```

CMake Presets should be used as the build profile contract because CMake explicitly supports checked-in `CMakePresets.json` for project-wide configure/build/test settings and local `CMakeUserPresets.json` for developer-specific settings. ([CMake][6])

## 9.13 Pathtracer API

```text
PathTracer
  load render scene
  select integrator
  select material registry
  select light sampler
  configure film
  reset accumulation
  render sample batch
  resolve film
  export counters

RTSceneData
  canonical geometry
  canonical materials
  canonical lights
  camera state
  environment state
  acceleration metadata

IntegratorSettings
  max depth
  sample count
  seed
  NEE policy
  MIS policy
  Russian roulette policy
  firefly clamp policy
  caustics policy
  delta material policy

FilmSettings
  resolution
  color space
  exposure
  white balance
  tone map
  gamma/output transform
  accumulation range
```

## 9.14 Material API

```text
MaterialDesc
  material family
  scalar parameters
  vector/color parameters
  texture bindings
  sampler bindings
  layering policy
  transparency policy
  emission policy
  compatibility notes

MaterialRegistry
  register material family
  register presets
  resolve material
  validate backend support
  validate energy/sampling policy
  map imported material

MaterialInstance
  base material
  parameter overrides
  texture overrides
  shader variant key
```

## 9.15 Light API

```text
LightDesc
  type
  transform
  color
  intensity
  unit
  shape
  size/radius/profile
  temperature/CCT
  visibility policy
  sampling weight

LightTypes
  point
  spot
  directional
  sphere
  rectangle
  disk
  line
  portal
  mesh emissive
  environment
  blackbody emitter
```

## 9.16 Camera API

```text
CameraDesc
  transform
  projection
  focal length
  sensor size
  FOV
  aperture
  focus distance
  shutter
  ISO
  exposure compensation
  white balance
  motion blur policy
  saved shot slots

CameraController
  orbit
  FPS
  scripted
  turntable
  benchmark path
  editor camera
```

## 9.17 Physics API

```text
IPhysicsWorld
  create body
  destroy body
  step fixed time
  query collisions
  query raycasts
  report events
  extract transforms

IPhysicsBody
  body type
  mass
  velocity
  material
  gravity scale
  sleeping policy
  transform authority policy

IPhysicsShape
  sphere
  box
  capsule
  cylinder
  convex hull
  triangle mesh
  compound
  trigger
```

## 9.18 Script API

```text
ScriptRuntime
  load script
  bind entity script
  expose safe context
  run lifecycle event
  collect commands
  report errors

ScriptContext
  entity handle
  time
  scene query
  command writer
  asset handles
  typed parameters
  diagnostics sink
```

## 9.19 Animation API

```text
Timeline
  clips
  tracks
  curves
  events
  playback range

AnimationClip
  duration
  frame rate metadata
  transform tracks
  morph tracks
  material tracks
  light tracks
  camera tracks

AnimationController
  play
  pause
  seek
  loop
  blend
  time scale
  fixed-step sample policy
```

## 9.20 Benchmark API

```text
BenchmarkRunDesc
  scene
  backend
  resolution
  spp
  duration
  seed
  warmup
  output directory
  reference image
  tolerance policy

BenchmarkController
  validate
  load
  warmup
  measure
  resolve
  export
  summarize

BenchmarkResult
  timing
  throughput
  memory
  backend metadata
  build metadata
  scene hash
  shader hash
  image hash
  reference diff
```

## 9.21 Editor API

```text
EditorController
  selection
  panels
  gizmos
  command stack
  inspector bindings
  scene graph
  asset browser
  benchmark panel

EditorCommand
  name
  target IDs
  apply
  undo
  merge policy
  serialization metadata
  diagnostics

EditorPanels
  scene graph
  inspector
  material editor
  light editor
  camera editor
  physics editor
  script editor
  animation timeline
  performance overlay
  benchmark panel
  debug views
```

---

# 10. Renderer control structure

## 10.1 Backend selection control

```text
Backend selection order:
  explicit CLI/config selection
  platform-preferred backend
  capability-matching backend
  fallback backend
  CPU reference for validation only
```

## 10.2 Backend capability validation

```text
Required capabilities for pathtracing baseline:
  compute dispatch
  storage buffers
  storage textures or equivalent output path
  readback/copy
  timestamp or fallback CPU timing
  sufficient buffer size
  sufficient texture size
  required texture formats
  shader variant compatibility
```

## 10.3 Backend profiles

```text
VulkanProfile
  first serious native GPU path
  Windows/Linux primary path
  macOS optional through portability layer only if chosen

D3D12Profile
  Windows native path
  future DXR path

MetalProfile
  macOS native path
  Apple GPU path

WebGPUProfile
  browser/WASM path
  WGSL first-class shader profile
  reduced feature requirements
  explicit memory/capability downgrade policy

CPUReferenceProfile
  correctness and reference output only
  not performance target
```

## 10.4 Render frame pass sequence

```text
PrepareFrameConstants
UploadDirtyResources
BuildOrUpdateAcceleration
GenerateCameraRays
TraceAndShade
SampleLights
AccumulateFilm
DenoiseOptional
ToneMapResolve
DebugVisualizeOptional
EditorOverlayOptional
ReadbackOptional
PresentOrExport
```

## 10.5 Accumulation reset rules

Accumulation must reset when any of these change:

```text
camera transform
camera lens/exposure settings
resolution
integrator settings
material parameters
light parameters
geometry
SDF primitive parameters
texture bindings
environment
tone-mapping mode, if output-space accumulation is affected
shader variant
physics/script/animation state, if visible
```

Accumulation should not reset for:

```text
editor panel movement
selection outline, unless rendered into beauty buffer
benchmark panel visibility
diagnostic UI
non-render-affecting metadata changes
```

---

# 11. Pathtracing control structure

## 11.1 Pathtracer pipeline stages

```text
SceneCanonicalization
  flatten renderable scene into stable render data

AccelerationPreparation
  build/refit acceleration metadata

SampleScheduling
  determine pixel/sample/tile order
  derive RNG counters

RayGeneration
  camera/lens sampling
  motion blur sampling if enabled

Traversal
  mesh/SDF/instance intersection path

Shading
  material evaluation
  BSDF sampling
  emission handling
  medium handling when enabled

LightSampling
  direct-light sampling
  environment sampling
  MIS weight calculation

PathContinuation
  throughput update
  Russian roulette
  depth accounting
  eta stack handling

FilmAccumulation
  linear HDR accumulation
  auxiliary buffer accumulation

Resolve
  denoise
  exposure
  tone map
  gamma/output transform
```

## 11.2 Integrator feature requirements

```text
camera ray generation
depth of field
motion blur policy
path throughput
PDF bookkeeping
next-event estimation
MIS
Russian roulette
delta material handling
dielectric eta stack
emissive hit handling
environment lighting
firefly suppression policy
max-depth control
sample dimension protocol
deterministic RNG counter protocol
```

## 11.3 Render data requirements

```text
mesh vertices
indices
normals
tangents
UV sets
vertex colors
skinning weights
morph deltas
instances
SDF primitives
materials
textures
samplers
lights
camera
environment
medium volumes
benchmark metadata
```

---

# 12. Shader system plan

## 12.1 Shader family categories

```text
Pathtracing compute shaders
Debug visualization shaders
Resolve/post shaders
Editor overlay shaders
Raster fallback shaders
Resource preprocessing shaders
Denoiser shaders
Benchmark/readback utility shaders
```

## 12.2 Pathtracing shader families

```text
Ray generation
Primary ray setup
Camera/lens sampling
Path state initialization
BVH traversal
SDF intersection
Triangle intersection
Instance transform traversal
Material evaluation
BSDF sampling
Direct light sampling
Environment sampling
Shadow ray testing
Path continuation
Film accumulation
Auxiliary G-buffer accumulation
```

## 12.3 Material shader families

```text
Lambert diffuse
Glossy
Mirror/specular
GGX metallic
GGX dielectric
Glass/refraction
Clearcoat
Subsurface approximation
Velvet
Rubber
Charcoal
X-Ray
Spectral approximation
Volumetric medium
Procedural materials
Layered materials
Stylized/toon
Emission/blackbody
```

## 12.4 SDF shader families

```text
SDF sphere
SDF box
SDF rounded box
SDF cylinder
SDF cone/frustum
SDF capsule
SDF ellipsoid
SDF torus
SDF disk/plane
SDF triangle/wedge/prism
SDF metaballs
SDF Mandelbulb/fractal
SDF CSG union
SDF CSG intersection
SDF CSG subtraction
SDF smooth union/blend
SDF deformation/noise modifiers
```

## 12.5 Lighting shaders

```text
Point light sampling
Spot light sampling
Directional light sampling
Sphere light sampling
Rectangle light sampling
Disk light sampling
Line light sampling
Portal light sampling
Mesh emissive sampling
Environment light sampling
Blackbody emitter evaluation
Color temperature conversion
Photometric intensity conversion
```

## 12.6 Denoising and temporal shaders

```text
Temporal accumulation
TAA resolve
History reprojection
Variance estimation
Albedo/normal guided filter
Simple temporal denoiser
A/B denoiser comparison path
Debug denoise channels
```

## 12.7 Post/film shaders

```text
Exposure
White balance
ACES tone map
Filmic tone map
Linear tone map
Gamma/output transform
Bloom
Glare
Depth of field resolve, if separated
Motion blur resolve, if separated
False-color debug output
Heatmap diff output
```

## 12.8 Debug visualization shaders

```text
Beauty
Albedo
Normals
Depth
World position
Material ID
Object ID
UV
Roughness
Metallic
Emission
Throughput
Sample count
Variance
BVH depth
SDF distance
Light contribution
Denoised output
Difference heatmap
NaN/Inf highlight
Overdraw/resource diagnostic
```

## 12.9 Editor/raster support shaders

```text
Fallback raster mesh
Wireframe
Bounding boxes
Transform gizmos
Light gizmos
Camera frustum
Selection outline
Physics debug draw
Skeleton debug draw
Collider debug draw
Grid/floor
Axis markers
Texture/material preview
```

## 12.10 Shader variant policy

Each shader variant is keyed by:

```text
backend
shader family
entry point
material family
integrator features
scene feature flags
texture feature flags
denoiser flags
debug flags
compiler/profile flags
resource layout version
```

Each shader variant must produce:

```text
source hash
variant hash
backend target
entry point
resource layout
feature requirements
cache key
compile diagnostics
compatibility notes
```

---

# 13. Material feature inventory

The material plan should preserve the breadth of the current plan and the older repo, but stage it into benchmark-safe tiers.

---

## 13.1 Material Pack 1: benchmark core

These are required for the first benchmark-ready prototype.

```text
Diffuse / Lambert
Emissive
Mirror
Specular
Glossy
GGX rough conductor
GGX rough dielectric
Metallic PBR
Dielectric / glass
Clearcoat
Normal-mapped PBR
Alpha mask
Environment emissive response
```

## 13.2 Material Pack 2: native successor identity

These preserve the old project’s “shader playground” feel.

```text
Velvet
Charcoal
Rubber
X-Ray
Subsurface approximation
Spectral glass approximation
Procedural material
SDF fractal material
Volumetric shafts
Caustics-inspired material response
Thin-film / iridescent
Retroreflector
Voronoi cracks
Diffraction grating
Anisotropic GGX
Blackbody emission
Fire plasma
Toon surface
Bokeh/motion-blur stress material
```

## 13.3 Material Pack 3: PBR expansion

```text
Plastic
Fabric / cloth
Porcelain / ceramic
Paint
Anisotropic metal
Iridescent / thin-film
Car paint
Wet surface
Frosted glass
Dirty glass
Wet paint
Corrosion / oxidation
Stone
Concrete
Plaster
Water / fluid surface
Ice / crystal
Stylized diffuse
```

## 13.4 Material Pack 4: advanced physical/stylized materials

```text
Skin
Wax
Marble scattering
Hair / fur lobes
Energy-conserving layered materials
Volumetric medium
Mud
Sand
Terra/earth
Brushed metal
Ground metal
Two-layer skin profile
Fire/sparkle emission
Light-emitting textiles
Holographic coatings
```

## 13.5 Material Pack 5: optional visual-density backlog

```text
Paper
Cardboard
Cardstock
Resin
Epoxy
Glossy varnish
Gemstone
Faceted dielectric
Dispersion material
Smoke
Chromatic dust
Marbleized composite
Granite composite
Foliage proxy
Pearl / lustre
Ink + paper wetness
Frosted acrylic
Translucent polymer
Ash-scattering charcoal
Rust progression
```

## 13.6 Material admission rule for benchmark scenes

A material may enter an official benchmark scene only when it has:

```text
declared parameter schema
declared sampling/PDF behavior
declared energy behavior
declared fallback behavior
declared backend support
reference output
debug view support
material-specific validation scene
```

Materials may exist in demo/editor mode before becoming benchmark-approved, but benchmark output must label them as experimental if used.

---

# 14. Geometry and primitive feature inventory

## 14.1 Mesh geometry

```text
Triangle mesh
Indexed mesh
Instanced mesh
Static mesh
Animated/skinned mesh
Morph target mesh
Imported model mesh
Mesh with per-material triangle ranges
Mesh with vertex colors
Mesh with UV0/UV1
Mesh with tangents
```

## 14.2 SDF and procedural primitives

```text
Sphere
Box
Rounded box
Cylinder
Cone
Frustum
Capsule
Ellipsoid
Torus
Disk
Plane
Triangle
Wedge
Prism
Metaball
Mandelbulb
SDF fractal
CSG union
CSG subtraction
CSG intersection
Smooth blend
Visible area light geometry
```

## 14.3 Scene object controls

```text
rename
duplicate
delete
hide/show
lock/unlock
select
multi-select
transform
parent/child relationship
material assignment
shader/material preset assignment
physics body assignment
script attachment
animation binding
benchmark tag
```

---

# 15. Lighting feature inventory

## 15.1 Light types

```text
Point
Spot
Directional
Sphere area
Rectangle area
Disk area
Line area
Portal
Mesh emissive
Environment sky
Open sky
Cornell-style box light
Blackbody emitter
Visible emissive object
```

## 15.2 Light parameters

```text
position
rotation
scale
intensity
unit
color
color temperature
CCT preset
radius
size
shape profile
falloff
spot angle
softness
visibility to camera
visibility to diffuse
visibility to specular
sampling weight
```

## 15.3 Photometric controls

```text
lumens
candela
lux
radiance
exposure compensation
color temperature
white point
scene-referred intensity scale
```

---

# 16. Camera and film feature inventory

## 16.1 Camera controls

```text
FOV
focal length
sensor size
aperture
focus distance
depth of field
ISO
shutter speed
f-stop
white balance
exposure compensation
motion blur
saved camera shots
orbit mode
FPS mode
scripted path
turntable path
benchmark path
```

## 16.2 Film/output controls

```text
render scale
exact render dimensions
720p preset
1080p preset
4K preset
linear HDR accumulation
PNG export
EXR export
sequence export
turntable export
light-sweep export
benchmark clip export
tone map selection
gamma/output transform
metadata sidecar
```

---

# 17. Asset pipeline plan

## 17.1 Import format tiers

### Tier 1: prototype-critical

```text
glTF / GLB
OBJ / MTL
PNG
JPEG / JPG
EXR
```

### Tier 2: expanded compatibility

```text
STL
PLY
3MF
KTX2
DDS
TIFF
Basis-compatible compressed textures
```

### Tier 3: advanced scene interchange

```text
USD
USDC
USDZ
X3D
MaterialX-style material mapping
```

## 17.2 Asset classes

```text
MeshAsset
TextureAsset
MaterialAsset
SamplerAsset
SceneAsset
AnimationAsset
SkeletonAsset
PhysicsCookedShapeAsset
ShaderAsset
BenchmarkSceneAsset
```

## 17.3 Importer requirements

Every importer must declare:

```text
supported file extensions
supported feature set
lossy conversion possibilities
coordinate system policy
unit scale policy
winding policy
material mapping policy
texture mapping policy
animation mapping policy
fallback material policy
diagnostics severity policy
```

## 17.4 Material import mapping

Incoming materials should map to the engine’s `MaterialDesc`.

Required source channels:

```text
base color
normal
roughness
metallic
occlusion
emissive
alpha/transparency
alpha cutoff
clearcoat
sheen
anisotropy
IOR
transmission
texture transforms
UV transforms
packed texture channels
normal map convention
```

## 17.5 Asset cache policy

```text
Stable asset IDs
Hash-based deduplication
Import diagnostics persistence
Material reuse by hash
Texture reuse by hash
Mesh canonicalization hash
Shader/material binding cache key
Hot-reload invalidation
Reimport compatibility notes
```

---

# 18. Physics plan

## 18.1 Physics role

Physics belongs primarily to:

```text
interactive demo scenes
editor-authoring scenes
special physics benchmark scenes
scene stress demos
```

Physics should be disabled by default for pure render benchmarks.

## 18.2 Physics features

```text
rigid bodies
static bodies
dynamic bodies
kinematic bodies
trigger shapes
collision events
raycasts
broadphase queries
debug draw
gravity
mass
friction
restitution
sleeping policy
fixed timestep
substeps
```

## 18.3 Physics shape support

```text
sphere
box
capsule
cylinder
convex hull
compound shape
static triangle mesh
trigger volume
```

## 18.4 Physics benchmark labeling

Every benchmark result must indicate:

```text
physics_enabled
physics_engine
fixed_dt
substeps
deterministic_mode
cross_platform_deterministic
body_count
shape_count
collision_event_count
physics_step_time
```

---

# 19. Scripting plan

## 19.1 Script role

Lua scripting should support:

```text
scene-specific behavior
light animation
camera motion
material parameter animation
trigger events
demo interactions
benchmark scripted sweeps, when explicitly allowed
```

## 19.2 Script lifecycle

```text
on_load
on_spawn
on_enable
on_disable
on_update
on_fixed_update
on_late_update
on_collision
on_trigger
on_animation_event
on_animation_loop
on_keyframe_reached
on_destroy
on_unload
```

## 19.3 Script control rules

```text
Scripts do not mutate ECS state directly.
Scripts emit commands.
Commands are scheduled into deterministic phases.
Scripts have stable execution order.
Scripts are sandboxed by default.
Benchmark mode disables scripts unless explicitly allowed.
Script errors produce structured diagnostics.
```

---

# 20. Animation plan

## 20.1 Animation features

```text
keyframe transform animation
skinned animation
morph target animation
material parameter animation
light parameter animation
camera animation
timeline clips
looping
trimming
scrubbing
track mute/solo
clip blending
play/pause/seek
time scale
animation events
```

## 20.2 Animation determinism

```text
fixed-delta sampling for benchmark mode
stable track order
stable interpolation policy
explicit unsupported-channel diagnostics
reduced-motion/disable-animation benchmark switch
```

## 20.3 Animation data model

```text
Timeline
Clip
Track
Curve
Keyframe
Event
Skeleton
Skin
JointPalette
MorphTargetWeights
AnimationController
```

---

# 21. Editor and demo controls

## 21.1 Editor panels

```text
Scene graph
Inspector
Transform panel
Material panel
Light panel
Camera panel
Physics panel
Script panel
Animation timeline
Asset browser
Benchmark panel
Performance overlay
Debug view selector
Output/export panel
```

## 21.2 Editor actions

```text
create primitive
create light
create camera
import asset
assign material
duplicate entity
rename entity
delete entity
hide/show
lock/unlock
parent/unparent
move/rotate/scale
edit material parameter
edit light parameter
edit camera parameter
attach script
edit script parameters
attach physics body
edit animation track
save scene
load scene
clone scene
run benchmark
export image
export benchmark result
```

## 21.3 Command stack requirements

```text
undo
redo
grouped transactions
action coalescing
bounded history
crash-safe recovery
command diagnostics
command serialization metadata
dirty state tracking
```

## 21.4 Interaction controls inherited from old project spirit

```text
orbit camera
FPS camera
pause camera
pause frames
pause rays at convergence
fullscreen
panel-preserving fullscreen
quality presets
debug views
benchmark sequence
save PNG
copy JSON results
local baseline comparison
scene presets
benchmark scenes
primitive creation menu
```

---

# 22. Benchmark system plan

## 22.1 Benchmark modes

```text
Fixed SPP
  render exact samples per pixel

Timed throughput
  render for fixed duration after warmup

Frame count
  render fixed number of frames

Quality threshold
  render until error threshold is reached

Scene sequence
  run all benchmark scenes

Interactive benchmark
  measure live viewport behavior

Physics + render benchmark
  explicitly includes deterministic physics stepping

Scripted benchmark
  explicitly includes deterministic script events
```

## 22.2 Benchmark scene pack

```text
Cornell Native
  diffuse GI, color bleeding, area light correctness

SDF Complexity
  SDF primitives, blends, fractals, analytic/procedural geometry

Material Gauntlet
  core and experimental material coverage

Sponza Lite / Atrium
  imported mesh, BVH stress, texture/material mapping

Shadow Study
  soft shadows, light size, penumbra behavior

Mirror Room
  specular paths and recursion behavior

Glass Lab
  dielectric/refraction/caustic-adjacent stress

Volumetric Fog Corridor
  medium/fog demo and performance stress

Neon Room
  emissive materials and bloom/glare output

Physics Chaos
  separate physics + render benchmark category

Particle Fluid
  optional stress scene, not baseline

Caustic Pool
  advanced/experimental scene

Motion Blur Stress
  camera/object motion path

Shader Gauntlet
  all material families and debug views
```

## 22.3 Required benchmark metrics

```text
backend
adapter/device name
driver/runtime
compiler
compiler flags
build type
git hash
scene hash
shader hash
asset hash
resolution
samples per pixel
max depth
ray budget
light count
material count
primitive count
triangle count
instance count
BVH node count
texture memory estimate
GPU memory estimate
CPU frame time
GPU frame time
upload time
BVH build/refit time
samples/sec
paths/sec
path vertices/sec
active rays/sec
p50 frame time
p95 frame time
p99 frame time
mean error vs reference
max error vs reference
image hash
```

## 22.4 Required benchmark artifacts

```text
results.json
results.csv
metadata.json
scene_snapshot.json
shader_manifest.json
asset_manifest.json
beauty.png
beauty.exr
reference.exr, when available
diff_heatmap.png, when available
debug_channels, optional
score_card.png, optional
```

## 22.5 Benchmark acceptance criteria

A run is benchmark-valid only when:

```text
scene schema is valid
backend capability requirements are met
shader variants are valid
assets are resolved
scene hash is recorded
shader hash is recorded
seed is recorded
sample order policy is recorded
resolution is recorded
timing policy is recorded
output artifacts pass schema validation
```

---

# 23. Dependency plan

## 23.1 Build/toolchain dependencies

```text
CMake
Ninja or platform generator
Clang/LLVM
clang-cl on Windows
clang++ on Linux/macOS
Emscripten for WebAssembly/WebGPU target
CTest for validation runs
Git for source/build metadata
```

## 23.2 Graphics/backend dependencies

```text
Vulkan SDK
Windows SDK / D3D12
Xcode / Metal SDK
WebGPU browser/runtime support
WGSL shader path
Optional native WebGPU abstraction layer, if selected later
```

## 23.3 Physics dependency

```text
Jolt Physics
```

Policy:

```text
enabled for desktop demo/editor
optional/reduced for web
disabled by default for pure render benchmark
deterministic mode explicitly configured
```

## 23.4 Scripting dependency

```text
Lua runtime or Lua C API integration wrapper
```

Policy:

```text
sandboxed by default
optional compile flag
disabled by default for strict benchmark mode
```

## 23.5 Asset/import dependencies

```text
tinygltf or equivalent for glTF/GLB
tinyobjloader or equivalent for OBJ/MTL
stb_image or equivalent for PNG/JPEG
tinyexr or equivalent for EXR
KTX/libktx/gli or equivalent for KTX2/DDS/compressed formats
meshoptimizer for optional mesh optimization/canonicalization
Assimp-class fallback only if needed for broad parity
OpenUSD path for USD/USDC/USDZ where feasible
```

## 23.6 Shader/tooling dependencies

```text
DXC, if D3D12/HLSL path requires it
SPIR-V tooling, if Vulkan shader pipeline requires it
WGSL validation tooling, if WebGPU path requires it
Metal shader tooling, if Metal path requires it
shader reflection tooling per backend
```

## 23.7 UI/editor dependency policy

The plan should allow either:

```text
native immediate-mode editor UI
custom retained-mode UI
platform-specific shell with shared editor model
```

But the editor model must not depend on UI implementation. The `EditorCommand`, `SceneGraphModel`, `InspectorModel`, and `BenchmarkPanelModel` should remain UI-toolkit independent.

---

# 24. Build and configuration plan

## 24.1 Required CMake options

```text
PT_ENABLE_VULKAN
PT_ENABLE_D3D12
PT_ENABLE_METAL
PT_ENABLE_WEBGPU
PT_ENABLE_CPU_REFERENCE

PT_ENABLE_JOLT
PT_ENABLE_LUA
PT_ENABLE_EDITOR
PT_ENABLE_BENCHMARK
PT_ENABLE_ASSET_IMPORT
PT_ENABLE_ANIMATION
PT_ENABLE_PROFILING
PT_ENABLE_TESTS

PT_STRICT_DETERMINISM
PT_CROSS_PLATFORM_DETERMINISM
PT_ENABLE_CXX2C_EXPERIMENTS
```

## 24.2 Required preset families

```text
desktop-clang-debug
desktop-clang-release
desktop-clang-benchmark

windows-clangcl-d3d12-debug
windows-clangcl-d3d12-release

linux-clang-vulkan-debug
linux-clang-vulkan-release

macos-clang-metal-debug
macos-clang-metal-release

web-emscripten-webgpu-debug
web-emscripten-webgpu-release

headless-benchmark-release
tools-release
validation-release
```

## 24.3 Build profile requirements

```text
Debug
  assertions
  diagnostics
  validation
  editor-friendly behavior

Release
  optimized demo runtime
  normal diagnostics

Benchmark
  deterministic settings
  stable metadata
  controlled logging
  no editor-only overhead unless requested

Validation
  shader/asset/schema/backend checks

Web
  reduced features
  explicit memory budgets
  browser-safe export behavior
```

---

# 25. Reproducibility plan

## 25.1 Deterministic render key

```text
scene hash
asset hash
shader hash
backend ID
build ID
compiler ID
seed
frame index
pixel index
sample index
dimension
path depth
path ID
```

## 25.2 Reproducibility metadata

```text
compiler
compiler flags
standard mode
target OS
backend
adapter
driver/runtime
build type
git hash
module versions
dependency versions
enabled features
deterministic flags
physics deterministic flags
shader variant hashes
scene and asset hashes
```

## 25.3 Determinism policies

```text
fixed sample order in benchmark mode
fixed tile order in benchmark mode
fixed ECS system order
stable entity iteration
deferred mutation
fixed physics timestep
explicit script dispatch order
explicit animation sampling policy
cross-backend tolerance policy
floating-point mode declaration
```

---

# 26. Diagnostics and error control

## 26.1 Diagnostic categories

```text
Build
Backend
Shader
Scene schema
Asset import
Material mapping
Texture decode
Resource residency
Physics
Scripting
Animation
Benchmark
Editor command
Performance
Determinism
```

## 26.2 Diagnostic severity

```text
Info
Warning
CompatibilityWarning
PerformanceWarning
Error
BlockingError
Fatal
```

## 26.3 Required diagnostic outputs

```text
human-readable console output
editor diagnostics panel
benchmark metadata diagnostics section
startup failure report
copyable error stack
artifact validation report
shader compile report
asset import report
```

---

# 27. Milestone plan

## Phase 0 — Toolchain and project contract

```text
C++23 / Clang policy
CMake presets
feature flags
module registry
diagnostics foundation
build metadata
dependency policy
test/validation shell
```

Exit gate:

```text
A native C++ executable reports build metadata, enabled modules,
compiler, standard mode, and selected backend profile.
```

---

## Phase 1 — Core, platform, scene, ECS, control structures

```text
Core services
Platform abstraction
Application modes
Scene schema v1
ECS registry
System phase scheduler
Command buffers
Transform hierarchy
Diagnostics
Headless mode
Basic window mode
```

Exit gate:

```text
A scene can load, validate, instantiate, update deterministically,
and serialize/export a stable scene snapshot without rendering.
```

---

## Phase 2 — Renderer abstraction and first pathtracer

```text
Renderer backend API
Vulkan first backend
CPU reference path for tiny scenes
Shader manifest
Resource handles
Frame graph
Pathtracer domain API
Film/output path
PNG/EXR export
Core material pack
Core light pack
Core SDF primitive pack
```

Exit gate:

```text
A C++ Vulkan build renders a deterministic pathtraced image
and exports metadata.
```

---

## Phase 3 — Benchmark harness

```text
Benchmark CLI
Warmup/measurement phases
Fixed-SPP mode
Timed mode
Reference comparison
Diff heatmap
JSON/CSV output
Scene/hash/shader metadata
Backend/device metadata
```

Exit gate:

```text
The project can run repeatable native benchmark scenes and emit
complete artifacts.
```

---

## Phase 4 — WebGPU/WASM target

```text
Emscripten build
WebGPU backend
WGSL shader variants
Browser canvas binding
Reduced web benchmark profile
Web capability diagnostics
Web export path
```

Exit gate:

```text
The same scene schema can run through the C++/WASM/WebGPU path
with declared feature limits.
```

---

## Phase 5 — Cross-backend expansion

```text
D3D12 backend
Metal backend
Backend capability matrix
Shader layout parity
Backend tolerance policies
Pipeline cache policy
```

Exit gate:

```text
At least two desktop graphics APIs run the same benchmark scene pack.
```

---

## Phase 6 — Physics, scripting, animation

```text
Jolt integration
Physics facade
Fixed-step simulation
Lua scripting
Script lifecycle
Animation clips/timelines
Transform arbitration
Debug draw
```

Exit gate:

```text
Interactive demo scenes can use physics/scripts/animation while benchmark
mode can freeze, disable, or explicitly label those systems.
```

---

## Phase 7 — Asset import and material expansion

```text
glTF/GLB
OBJ/MTL
PNG/JPEG/EXR
Material mapping
Texture/sampler policy
Sponza-class scene ingestion
Material Pack 2
Material Pack 3 subset
Import diagnostics
```

Exit gate:

```text
An imported mesh-heavy scene can be pathtraced and benchmarked
with stable asset/material metadata.
```

---

## Phase 8 — Editor-lite and scene creator

```text
Scene graph
Inspector
Material editor
Light editor
Camera editor
Transform gizmos
Benchmark panel
Debug views
Undo/redo
Scene save/load/clone/replay
Script attachment panel
Animation timeline preview
```

Exit gate:

```text
The native app feels like the richer successor to the old browser demo:
interactive, editable, pathtraced, exportable, and benchmarkable.
```

---

## Phase 9 — Advanced rendering and finalization

```text
Advanced material packs
Denoiser upgrades
Volumetric features
USD/USDC/USDZ expansion
Hardware ray tracing path
Multi-GPU discovery
Full benchmark scene battery
Regression thresholds
Release candidate packaging
```

Exit gate:

```text
The prototype is benchmark-ready, demo-ready, and structured for future
backend/material/editor growth.
```

---

# 28. Prototype definition of done

The prototype is ready when it can run:

```text
ptbench run
  scene: benchmark scene
  backend: Vulkan or another complete backend
  resolution: fixed
  samples per pixel: fixed
  seed: fixed
  output: artifact directory
```

And produce:

```text
beauty.png
beauty.exr
results.json
results.csv
metadata.json
scene_snapshot.json
shader_manifest.json
asset_manifest.json
optional reference.exr
optional diff_heatmap.png
```

With these guarantees:

```text
C++23-native runtime
Clang/LLVM toolchain metadata
backend/device metadata
scene hash
asset hash
shader hash
deterministic seed
declared sample ordering
declared material set
declared light set
declared integrator settings
declared timing mode
machine-readable benchmark results
image export
diagnostics export
```

---

# 29. Final positioning statement

Use this as the refined top-level project statement:

> Build a C++23 / Clang-first, cross-platform 3D pathtracing prototype and benchmark platform. The project is a native high-performance successor to the existing WebGL pathtracer, preserving its interactive scene/material/lighting/benchmark identity while replacing the JavaScript/WebGL runtime with a deterministic C++ engine architecture. Desktop targets use Vulkan, D3D12, and Metal; the web target uses Emscripten/WebAssembly and WebGPU/WGSL. The plan prioritizes control structures, API contracts, scene/schema determinism, shader/material registries, benchmark artifacts, reproducible rendering, and a clean editor/demo mode before expanding into deeper physics, scripting, animation, asset import, USD, hardware ray tracing, and advanced materials.

[1]: https://github.com/monstercameron/pathtracer "GitHub - monstercameron/pathtracer: Demo here! · GitHub"
[2]: https://clang.llvm.org/cxx_status.html "Clang - C++ Programming Language Status"
[3]: https://emscripten.org/docs/introducing_emscripten/about_emscripten.html "About Emscripten — Emscripten 5.0.8-git (dev) documentation"
[4]: https://jrouwe.github.io/JoltPhysics/ "Jolt Physics: Jolt Physics"
[5]: https://www.w3.org/TR/WGSL/ "WebGPU Shading Language"
[6]: https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html "cmake-presets(7) — CMake 4.3.2 Documentation"
