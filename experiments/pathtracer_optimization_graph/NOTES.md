# Path Tracer Optimization Graph Notes

Date: 2026-05-05

## Intent

Create a new example folder that treats path tracer optimization as a graph search. Nodes are implementation or configuration choices. Edges are measured experiments. The run is complete only when every declared edge has an artifact and a decision.

## Graph Contract

CPU math edges:

- `cpu.normalize.sqrt_div -> cpu.normalize.fast_rsqrt_nr`
- `cpu.fresnel.std_pow5 -> cpu.fresnel.mul_pow5`
- `cpu.material.pow2 -> cpu.material.mul_pow2`
- `cpu.triangle.compute_edges -> cpu.triangle.cached_edges`
- `cpu.aabb.divide_per_axis -> cpu.aabb.precompute_inv_dir`
- `cpu.camera.basis_per_ray -> cpu.camera.precomputed_basis`
- `cpu.rng.splitmix64 -> cpu.rng.pcg32_contract_candidate`

CPU renderer scaffolding edges:

- `ptbench.cpu_scalar -> ptbench.cpu_tiled_auto`
- `ptbench.cpu_tiled_auto -> ptbench.thread_sweep`
- `ptbench.cpu_tiled_auto -> ptbench.tile_sweep`
- `ptbench.scalar_packet -> ptbench.simd_packet`

GPU/scaffolding edges:

- `gpu.shader.wg8x8 -> gpu.shader.wg16x8`
- `gpu.shader.wg8x8 -> gpu.shader.wg8x16`
- `gpu.shader.wg8x8 -> gpu.shader.wg16x16`
- `gpu.shader.pow_builtin -> gpu.shader.pow_small_int_mul`
- `gpu.backend.simulated_or_real -> gpu.backend.shader_matrix`
- `gpu.backend.simulated_or_real -> gpu.backend.memory_pressure`
- `ptbench.cpu_scalar -> ptbench.gpu_compute`

## Current Run Log

- Created this example folder.
- Added `math_microbench.cpp` for CPU math edge timing.
- Added `run_graph.ps1` to build/run all declared edges and write graph artifacts.
- Results are written to `experiments/pathtracer_optimization_graph/results/`.

## Completed Edge Sweep: Existing Debug ptbench

Command:

```powershell
.\experiments\pathtracer_optimization_graph\run_graph.ps1
```

Summary artifact:

- `experiments/pathtracer_optimization_graph/results/graph_summary.json`
- `experiments/pathtracer_optimization_graph/results/graph.dot`

Outcome:

- All 18 declared edges were explored.
- CPU math winners: precomputed AABB inverse direction (`1.386979x`) and cached triangle edges (`1.178625x`).
- CPU tiled auto beat CPU scalar in the existing Debug build (`1.099900x`).
- Best thread sweep in that run: 4 workers.
- Best tile sweep in that run: 32 rows.
- SIMD packet sweep kept scalar because the existing build was not compiled with AVX2.
- GPU runtime edge was explored but rejected for decision-making because the existing `build/default/bin/ptbench.exe` reported `cpu_simd_mode=simulated-vulkan`.
- GPU shader static edge accepted small-integer power lowering: SPIR-V `Pow` mentions dropped from 8 to 4.

## Completed Edge Sweep: Vulkan Release ptbench

Build:

```powershell
cmake -S . -B build\optimization-graph-vulkan -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=C:/Program Files/LLVM/bin/clang++.exe" "-DCMAKE_RC_COMPILER=C:/Program Files/LLVM/bin/llvm-rc.exe" -DPT_ENABLE_VULKAN=ON -DPT_ENABLE_CPU_RAYTRACER=ON -DPT_ENABLE_AVX2=ON -DPT_ENABLE_BENCHMARK=ON
cmake --build build\optimization-graph-vulkan --target ptbench --config Release
```

Run:

```powershell
.\experiments\pathtracer_optimization_graph\run_graph.ps1 -Ptbench build\optimization-graph-vulkan\bin\ptbench.exe -OutDir experiments\pathtracer_optimization_graph\results_vulkan
```

Summary artifact:

- `experiments/pathtracer_optimization_graph/results_vulkan/graph_summary.json`
- `experiments/pathtracer_optimization_graph/results_vulkan/graph.dot`

Outcome:

- All 18 declared edges were explored again.
- Real GPU path was used: Intel(R) Arc(TM) B580 Graphics, discrete, 12118 MB VRAM.
- GPU compute render edge beat Release CPU scalar by `1.154193x` at `96x96`, `spp=1`; render time was `5.993800 ms`, build/upload time was `500.119800 ms`.
- CPU math winners: precomputed AABB inverse direction (`1.643175x`) and cached triangle edges (`1.525218x`).
- RNG PCG32 was slightly faster (`1.039908x`) but marked `candidate_requires_rng_contract_update`, not a drop-in change.
- CPU tiled auto lost to CPU scalar for the direct scalar-vs-auto run (`0.769376x`), but the dedicated thread sweep found 8 workers best (`2.2805x` vs one worker).
- Best tile sweep in the Vulkan Release run: 64 rows.
- SIMD packet sweep still kept scalar because the `simd-sweep` target reported AVX2 unavailable despite the build using `PT_ENABLE_AVX2`; this edge needs follow-up in the SIMD compile-detection path.
- GPU workgroup variants all compiled, but no runtime winner was selected from static SPIR-V size alone.
- GPU small-integer power lowering remained the static shader winner: `Pow` mentions dropped from 8 to 4.

## Best Path So Far

- CPU algorithm/scaffolding path: cached triangle edge data plus precomputed AABB inverse direction are the strongest safe math/scaffolding wins.
- GPU path: use the real Vulkan compute tracer when built with `PT_ENABLE_VULKAN=ON`; keep workgroup size at 8x8 until runtime timing is added for generated shader variants; lower small integer shader powers where visual parity is confirmed.
- Do not replace CPU normalization with the fast inverse square-root variant; it was slower on this compiler/CPU despite acceptable error.
- Do not switch RNG globally without a deterministic/rendering contract update.

## Applied Changes From Best Path

- Wired `ptbench` so a Vulkan-enabled build instantiates `vkpt::gpu::VulkanGpuPathTracer` for `--renderer-path gpu-compute` instead of falling back to the simulated scalar path.
- Applied cached triangle edge data to the CPU BVH accelerator primitive payload.
- Applied per-ray inverse direction reuse to CPU BVH AABB slab tests.

## Post-Change Verification

Run:

```powershell
.\experiments\pathtracer_optimization_graph\run_graph.ps1 -Ptbench build\optimization-graph-vulkan\bin\ptbench.exe -OutDir experiments\pathtracer_optimization_graph\results_vulkan_after
```

Summary artifact:

- `experiments/pathtracer_optimization_graph/results_vulkan_after/graph_summary.json`
- `experiments/pathtracer_optimization_graph/results_vulkan_after/graph.dot`

Outcome:

- All 18 declared edges were explored after applying the selected CPU hot-path changes.
- CPU scalar render improved from `6.918000 ms` before the hot-path patch to `5.602900 ms` after it on the same `96x96`, `spp=1`, Release/Vulkan-enabled build.
- Real Vulkan compute render improved from `5.993800 ms` before the hot-path patch run to `4.618700 ms` in the post-change run; the GPU runtime edge beat CPU scalar by `1.213090x`.
- GPU build/upload time dropped from `500.119800 ms` to `91.611000 ms` between the two Vulkan runs, likely due to driver/pipeline cache warm-up; render time is the more useful comparison for the path trace loop.
- Post-change graph still selects AABB inverse direction (`1.485549x`) and cached triangle edges (`1.330802x`) as safe CPU math/scaffolding winners.
- Best post-change thread sweep: 2 workers (`1.0671x` vs one worker).
- Best post-change tile sweep: 64 rows.
- SIMD packet sweep still reports scalar as best because the sweep reports AVX2 unavailable; this remains a scaffolding follow-up.

## Final Shader-Aware Edge Sweep

Applied after the post-change run:

- Lowered constant small-integer powers in the live Vulkan GLSL shader for preview horizon/rim, retro material effect, and Fresnel terms.
- Mirrored the same exact `2.0`, `5.0`, and `6.0` multiply lowering in the D3D12 compute and DXR HLSL shaders.
- Updated the graph runner so the shader edge records `already_applied` when the live shader has no remaining small-integer `pow(...)` source calls.

Verification:

```powershell
cmake --build build\optimization-graph-vulkan --target ptbench --config Release
& 'C:\VulkanSDK\1.4.341.1\Bin\dxc.exe' -T cs_6_0 -E main src\shaders\gpu\pathtrace_cs.hlsl -Fo $env:TEMP\pathtrace_cs_main.dxil
& 'C:\VulkanSDK\1.4.341.1\Bin\dxc.exe' -T cs_6_0 -E tonemap_main src\shaders\gpu\pathtrace_cs.hlsl -Fo $env:TEMP\pathtrace_cs_tonemap.dxil
& 'C:\VulkanSDK\1.4.341.1\Bin\dxc.exe' -T lib_6_3 src\shaders\gpu\pathtrace_rt.hlsl -Fo $env:TEMP\pathtrace_rt.dxil
.\experiments\pathtracer_optimization_graph\run_graph.ps1 -Ptbench build\optimization-graph-vulkan\bin\ptbench.exe -OutDir experiments\pathtracer_optimization_graph\results_vulkan_shader_after
```

Summary artifact:

- `experiments/pathtracer_optimization_graph/results_vulkan_shader_after/graph_summary.json`
- `experiments/pathtracer_optimization_graph/results_vulkan_shader_after/graph.dot`

Outcome:

- All 18 declared edges were explored in the final run.
- Final CPU scalar render: `5.461300 ms` at `96x96`, `spp=1`.
- Final Vulkan compute render: `4.526600 ms`, `1.206491x` faster than CPU scalar in the same run.
- Final GPU device: Intel(R) Arc(TM) B580 Graphics, `cpu_simd_mode=vulkan-compute`.
- Final safe CPU math winners: AABB inverse direction (`1.684685x`) and cached triangle edges (`1.115451x`).
- Camera basis microbench crossed the threshold (`1.022863x`), but the renderer path already supplies precomputed camera basis vectors.
- Shader small-integer power edge status: `ok`, decision `already_applied`, with `small_int_source_mentions=0`.
- Workgroup variants all compiled with identical SPIR-V size in the static pass; no runtime workgroup winner is selected by this graph yet.
- Best final thread sweep: 2 workers (`1.0976x` vs one worker).
- Best final tile sweep: 64 rows.
- SIMD packet sweep still reports scalar as best because AVX2 remains unavailable to that benchmark path.
