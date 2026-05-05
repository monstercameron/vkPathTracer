# D3D12/DXR Algorithm Graph Notes

Scratchpad for pass 3. This file is intentionally raw: commands, run tables, measurements, and short analysis live here.

Scope:
- D3D12 compute path tracer CPU BVH builder used by the GPU traversal shader.
- Static BVH traversal shader variants compiled through `PT_D3D12_SHADER_TRAVERSAL`.
- DXR remains in scope as backend scaffolding, but this pass focuses on code-level algorithm decisions in the compute path because pass 2 already found DXR slower on this Cornell benchmark.

Decision variables:
- `PT_D3D12_BVH_LEAF_SIZE`: 1, 2, 4, 8, 16.
- `PT_D3D12_BVH_BUCKETS`: 4, 8, 16 for SAH.
- `PT_D3D12_BVH_SPLIT_MODE`: `sah`, `median`.
- `PT_D3D12_SHADER_TRAVERSAL`: `baseline`, `bounds_helper`, `near_order`.


## Run 2026-05-05T16:44:06.2701982-04:00
ptbench=C:\Users\Cam\Desktop\vkPathTracer\build\d3d12-optimization\bin\ptbench.exe
scene=C:\Users\Cam\Desktop\vkPathTracer\assets\scenes\cornell_native.json resolution=64x64 spp=2 repeats=2

### Summary
baseline=bvh_sah_leaf4_bucket8 paths=14811116.43 build_ms=2861.595
best_bvh=bvh_median_leaf16 paths=15944192.19 build_ms=3047.124 speedup=1.0765
best_final=bvh_median_leaf16 paths=15944192.19 build_ms=3047.124 speedup_vs_baseline=1.0765
edges=22 failed=0

name,status,paths_per_sec,render_ms,build_ms,image_hash
bvh_median_leaf16,ok,15944192.19,4.11,3047.124,260c2b0ddbec004e
bvh_median_leaf16,ok,15944192.19,4.11,3047.124,260c2b0ddbec004e
bvh_sah_leaf16_bucket8,ok,15865951.49,4.135,2921.443,260c2b0ddbec004e
bvh_sah_leaf16_bucket16,ok,15861504.54,4.132,2828.687,260c2b0ddbec004e
bvh_sah_leaf16_bucket4,ok,15695399.32,4.176,2818.074,260c2b0ddbec004e
bvh_sah_leaf2_bucket4,ok,15322861.39,4.277,3055.297,2857195804cc8957
bvh_sah_leaf8_bucket8,ok,15168019.81,4.335,2845.811,f30f8bdd7c9fcb22
bvh_median_leaf16_shader_near_order,ok,15040408.14,4.368,2950.327,260c2b0ddbec004e
bvh_sah_leaf2_bucket8,ok,15005888.35,4.373,2962.763,2857195804cc8957
bvh_median_leaf8,ok,14919987.24,4.407,2828.09,e98c425025e24d00
bvh_sah_leaf2_bucket16,ok,14823626.71,4.442,2943.926,2857195804cc8957
bvh_sah_leaf4_bucket8,ok,14811116.43,4.44,2861.595,2857195804cc8957
bvh_sah_leaf4_bucket16,ok,14773773.45,4.442,2863.461,2857195804cc8957
bvh_median_leaf16_shader_bounds_helper,ok,14643766.86,4.49,3012.293,260c2b0ddbec004e
bvh_sah_leaf8_bucket16,ok,14395833.82,4.559,2846.185,f30f8bdd7c9fcb22
bvh_sah_leaf8_bucket4,ok,14252042.29,4.627,2918.275,f30f8bdd7c9fcb22
bvh_median_leaf4,ok,13692884.69,4.786,2777.088,8b846226d5d01976
bvh_sah_leaf4_bucket4,ok,13501664.33,4.923,2837.788,2857195804cc8957
bvh_sah_leaf1_bucket16,ok,13326440.38,4.973,3045.049,2857195804cc8957
bvh_median_leaf1,ok,13138544.15,4.988,2886.348,e98c425025e24d00
bvh_median_leaf2,ok,13121495.83,4.995,2786.957,e98c425025e24d00
bvh_sah_leaf1_bucket4,ok,13065815.46,5.053,3410.333,2857195804cc8957
bvh_sah_leaf1_bucket8,ok,12957201.99,5.058,2962.762,2857195804cc8957

Post-run default decision:
- Raw render winner: `bvh_median_leaf16`, 15.944M paths/sec, 1.0765x versus the old `sah leaf4 bucket8` baseline, but build time regressed to 3047.124 ms.
- Balanced default: `bvh_sah_leaf16_bucket16`, 15.862M paths/sec, 1.071x versus baseline, with build time improved to 2828.687 ms. This keeps the SAH builder rather than making median split the global default from a single Cornell-sized scene.
- Applied default in code: `PT_D3D12_BVH_LEAF_SIZE` fallback 16, `PT_D3D12_BVH_BUCKETS` fallback 16, split fallback remains `sah`.
- Shader traversal variants were fully explored under the best raw BVH candidate; both `bounds_helper` and `near_order` lost to baseline traversal on this scene.


## Run 2026-05-05T17:10:49-04:00

Code-level D3D12/DXR shader cache pass.

Changes tested:
- Added GPU-side packed triangle data buffer for D3D12/DXR scaffolding.
- DXR closest-hit now reads material and triangle edges from `TriDataBuf` instead of gathering `IndexBuf` + `VertBuf` + `TriMatBuf`.
- Compute shader packed path tested as `PT_D3D12_PACKED_TRIANGLES`.
- Rejected `Buffer<float4>`/`R32G32B32A32_FLOAT` variant from the previous scratch run: smoke was hash-stable, but render was slower (`2.9241 ms` vs `2.7271 ms` at 32x32/spp1).
- Shrunk packed triangle record from 16 floats to 12 floats:
  - 0..2 = v0
  - 3 = material index
  - 4..6 = e1
  - 7 = double-sided flag
  - 8..10 = e2
  - 11 = padding
- Deferred material and normal work until after a confirmed hit in the compute packed path.
- Dynamic-instance occlusion now has a packed any-hit function that skips normal transforms.

Smoke:
- `d3d12-compute`, default env, 32x32/spp1: ok, hash `63b75bafe0f7656b`, diagnostics include `compute_packed_triangle_intersections=false`.
- `d3d12-dxr`, default env, 32x32/spp1: ok, hash `1235c5f4e12523e0`, diagnostics include `dxr_packed_triangle_closest_hit=true`.

64x64, spp=2, repeats=3, BVH leaf=16 bucket=16 sah, traversal=baseline:

name,median_render_ms,median_build_ms,median_paths_per_sec,hash
packed_off_rpp1,2.9302,2922.7016,2795713.603167,75e2a575ae37e9e7
packed_on_rpp1,3.0709,3022.8781,2667621.869810,75e2a575ae37e9e7
packed_off_rpp8,4.3763,3003.9696,14975207.366954,260c2b0ddbec004e
packed_on_rpp8,4.2851,3093.0127,15293925.462650,260c2b0ddbec004e

128x128, spp=2, repeats=2, same env:

name,avg_render_ms,avg_build_ms,avg_paths_per_sec,hash
packed_off_rpp1,2.90795,2901.5224,11276294.872415,6f87a0ff78e382ab
packed_on_rpp1,2.89710,2935.88245,11310948.926087,6f87a0ff78e382ab
packed_off_rpp8,5.04725,2871.58895,51940240.155044,87aa595ba9ed632f
packed_on_rpp8,5.35045,2865.1048,49566317.270000,87aa595ba9ed632f

Decision:
- Compute packed triangles are mixed: small 64x64/rpp8 wins, 64x64/rpp1 loses, larger 128x128/rpp1 is roughly neutral/slightly up, larger 128x128/rpp8 loses. Default compute path stays unpacked (`PT_D3D12_PACKED_TRIANGLES` fallback false).
- Keep the compute packed path as an opt-in experiment because it is hash-stable and may win on some scenes/workloads, but it is not the graph winner for the default path.
- Keep the DXR packed closest-hit path because DXR intersection already resolved the triangle; the shader only needs material and edges for shading, so removing closest-hit vertex/index gathers is the cache-correct direction. DXR smoke passes after root/descriptor table expansion to t7.
- Shader is not "vectorized" across rays in a wave/SIMD packet sense. It uses HLSL `float3` vector ALU inside one path per invocation. The cache optimization that held up is not float4 vector loads; it is reducing and delaying memory loads on hit shaders and preserving the measured BVH defaults.

Follow-up code update:
- Default compute no longer builds/uploads full packed triangle data. It uploads a 12-float dummy `TriDataBuf` only so the expanded descriptor table remains valid.
- Full packed triangle data is built only when `m_preferDxr` is true or `PT_D3D12_PACKED_TRIANGLES=1`.
- If DXR is enabled after a compute-only scene upload, the scene upload is invalidated so DXR cannot shade from dummy triangle data.
- Added `QtPanelPropertyKind::Enum` because the current editor panel changes referenced it and it blocked rebuilding `ptbench`.

Validation after update:
- Build: `cmake --build build\d3d12-optimization --target ptbench --config Release` passed.
- Default compute smoke: ok, `compute_packed_triangle_intersections=false`, hash matched opt-in packed compute in this run.
- Opt-in compute packed smoke: ok with `PT_D3D12_PACKED_TRIANGLES=1`.
- DXR smoke: ok, `dxr_packed_triangle_closest_hit=true`.
