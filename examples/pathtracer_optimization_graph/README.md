# Path Tracer Optimization Graph

This example is a graph-driven performance lab for the path tracing algorithm and its supporting scaffolding.

The graph runner explores a finite set of CPU math, CPU render scaffolding, and GPU shader/backend edges. Each edge produces an artifact under `results/`, then `graph_summary.json` and `graph.dot` record which path is currently best.

## Run

From the repository root:

```powershell
.\examples\pathtracer_optimization_graph\run_graph.ps1
```

Optional parameters:

```powershell
.\examples\pathtracer_optimization_graph\run_graph.ps1 `
  -Ptbench .\build\default\bin\ptbench.exe `
  -Scene assets\scenes\cornell_native.json `
  -Resolution 96x96 `
  -Spp 1
```

## What The Graph Covers

- CPU math micro-edges: normalization, Fresnel pow5, small integer powers, triangle intersection caching, AABB inverse direction caching, camera basis precompute, and RNG contract candidates.
- CPU renderer scaffolding: scalar run, tiled run, thread sweep, tile sweep, SIMD sweep.
- GPU shader/backend scaffolding: GLSL workgroup variants, small-power shader variants, SPIR-V static instruction counts, shader matrix, backend availability, and memory pressure probe.

The GPU timing edge is only conclusive when `ptbench` is built with the real GPU path. If the active binary uses the simulated Vulkan backend, the graph still explores the edge but marks the runtime result as simulated.

