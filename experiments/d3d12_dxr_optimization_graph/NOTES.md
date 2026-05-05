# D3D12/DXR optimization graph notebook

Date: 2026-05-05

Scope: D3D12 compute, DXR dispatch, and CPU scene/scaffolding work that feeds those paths.

Working rule: treat every code-level decision point discovered in this scoped path as a graph edge. For each edge, run every direction that can execute, record failures as data, and use measured build/render/throughput results to choose the next path.

Initial decision inventory from code inspection:

- `ptbench` run path: CPU scalar, D3D12 compute, D3D12 DXR.
- D3D12 compute batching: `PT_D3D12_RAYS_PER_PIXEL` = 1, 2, 4, 8.
- D3D12 readback cadence: finite render final-only readback vs forced every-sample readback.
- D3D12 CPU scene packing: dynamic instance transform path enabled vs disabled.
- DXR backend: compute shader traversal vs hardware `DispatchRays`.
- DXR acceleration structure build preference: `fast_trace`, `fast_build`, `none`.
- DXR CPU scene packing: dynamic instance transform path enabled vs disabled.
- Shader compiler/static path: D3D12 compute shader compiles, tonemap shader compiles, DXR library compiles.
- Shader source math path: exact small-integer powers should already be lowered; remaining `pow` calls should be gamma, variable exponent, or non-integer visual effects.

Implementation notes:

- `ptbench run` needed D3D12/DXR wiring before measurements could be made.
- The D3D12 tracer now exposes benchmark knobs through environment variables:
  - `PT_D3D12_RAYS_PER_PIXEL`
  - `PT_D3D12_FORCE_READBACK_EVERY_SAMPLE`
  - `PT_D3D12_DYNAMIC_INSTANCE_TRANSFORMS`
  - `PT_D3D12_DXR_BUILD_MODE`

Raw run log starts below.

Analysis scratch, current median run:

- Final median graph run: `2026-05-05T16:30:08-04:00`, `64x64`, `spp=2`, `repeats=3`.
- All 14 declared D3D12/DXR edges explored.
- D3D12 compute vs CPU scalar: take D3D12 compute. Median samples/sec speedup `3.637872x`; median render `1.6897 ms` vs CPU `6.0917 ms`.
- D3D12 compute vs DXR: keep compute. DXR dispatch is real (`cpu_simd_mode=d3d12-dxr`) but median samples/sec is `0.221761x` of D3D12 compute for this scene/resolution.
- D3D12 rays per dispatch: rpp2, rpp4, rpp8 all increase actual path throughput vs rpp1. Median path throughput: rpp1 `5.091M/s`, rpp2 `8.302M/s`, rpp4 `13.795M/s`, rpp8 `19.743M/s`. Best explored direction: rpp8 for throughput, with lower nominal samples/sec because each dispatch does more rays than the public `--spp` count.
- Readback every sample: keep final-only. Median path throughput ratio `0.943366x`.
- Dynamic instance transform packing off for compute: keep dynamic-on. Median path throughput ratio `0.921170x`.
- DXR AS build mode: keep `fast_trace`. `fast_build` had median build ratio `0.868603x` vs fast_trace and essentially equal/slightly lower render throughput (`0.996060x`). `none` was also worse on build and render.
- DXR dynamic instance transform packing off: keep dynamic-on. Median path throughput ratio `1.031400x`, below the 5% decision threshold.
- Shader static checks: compute main, compute tonemap, and DXR library all compile with `dxc`; exact small-integer pow source count is `0`.
- Current best path for this graph: D3D12 compute, `PT_D3D12_RAYS_PER_PIXEL=8`, final-only readback, dynamic instance transforms enabled, DXR kept available but not selected for this Cornell workload.


## Run 2026-05-05T16:21:40.1491462-04:00
ptbench=C:\Users\Cam\Desktop\vkPathTracer\build\d3d12-optimization\bin\ptbench.exe
scene=C:\Users\Cam\Desktop\vkPathTracer\assets\scenes\cornell_native.json resolution=64x64 spp=2

summary=C:\Users\Cam\Desktop\vkPathTracer\experiments\d3d12_dxr_optimization_graph\results\graph_summary.json
dot=C:\Users\Cam\Desktop\vkPathTracer\experiments\d3d12_dxr_optimization_graph\results\graph.dot
all_declared_edges_explored=True edge_count=14
run cpu_scalar status=failed mode=scalar build_ms=0.141800 render_ms=6.108100 paths_per_sec=5,337,830.094
run d3d12_compute_dynamic_off status=failed mode=d3d12-compute build_ms=3,125.540500 render_ms=2.411400 paths_per_sec=3,397,196.649
run d3d12_compute_readback_every status=failed mode=d3d12-compute build_ms=3,051.516300 render_ms=2.116000 paths_per_sec=3,871,455.577
run d3d12_compute_rpp1 status=failed mode=d3d12-compute build_ms=2,926.828700 render_ms=2.226500 paths_per_sec=3,679,317.314
run d3d12_compute_rpp2 status=failed mode=d3d12-compute build_ms=2,911.507000 render_ms=1.959500 paths_per_sec=8,361,316.662
run d3d12_compute_rpp4 status=failed mode=d3d12-compute build_ms=3,229.768800 render_ms=2.491800 paths_per_sec=13,150,333.093
run d3d12_compute_rpp8 status=failed mode=d3d12-compute build_ms=2,976.564800 render_ms=3.282500 paths_per_sec=19,965,270.373
run d3d12_dxr_build_none status=failed mode=d3d12-dxr build_ms=5,092.287200 render_ms=8.010100 paths_per_sec=1,022,708.830
run d3d12_dxr_dynamic_off status=failed mode=d3d12-dxr build_ms=5,874.022500 render_ms=8.214300 paths_per_sec=997,285.222
run d3d12_dxr_fast_build status=failed mode=d3d12-dxr build_ms=5,328.490200 render_ms=7.246600 paths_per_sec=1,130,461.182
run d3d12_dxr_fast_trace status=failed mode=d3d12-dxr build_ms=4,836.758900 render_ms=7.752100 paths_per_sec=1,056,745.914
edge edge_backend_cpu_to_d3d12_compute status=failed decision=keep_baseline speedup=0.689291 reason=
edge edge_backend_d3d12_compute_to_dxr status=failed decision=keep_or_fallback_compute speedup=0.287212 reason=mode=d3d12-dxr
edge edge_compute_rpp_1_to_2 status=failed decision=take_candidate speedup=2.272518 reason=
edge edge_compute_rpp_1_to_4 status=failed decision=take_candidate speedup=3.574123 reason=
edge edge_compute_rpp_1_to_8 status=failed decision=take_candidate speedup=5.426352 reason=
edge edge_compute_readback_final_to_every status=failed decision=take_candidate speedup=1.052221 reason=
edge edge_compute_dynamic_instances_on_to_off status=failed decision=keep_dynamic_on speedup=0.923323 reason=
edge edge_dxr_fast_trace_to_fast_build status=failed decision=take_candidate speedup=1.069757 reason=
edge edge_dxr_fast_trace_to_none status=failed decision=keep_fast_trace speedup=0.967791 reason=
edge edge_dxr_dynamic_instances_on_to_off status=failed decision=keep_dynamic_on speedup=0.943732 reason=
edge edge_shader_compute_main_compile status=failed decision=blocked speedup=0.000000 reason=dxc exit 1
edge edge_shader_compute_tonemap_compile status=failed decision=blocked speedup=0.000000 reason=dxc exit 1
edge edge_shader_dxr_library_compile status=failed decision=blocked speedup=0.000000 reason=dxc exit 1
edge edge_shader_small_int_pow_scan status=ok decision=already_applied speedup=0.000000 reason=small_int_pow_mentions=0

## Run 2026-05-05T16:23:12.9306571-04:00
ptbench=C:\Users\Cam\Desktop\vkPathTracer\build\d3d12-optimization\bin\ptbench.exe
scene=C:\Users\Cam\Desktop\vkPathTracer\assets\scenes\cornell_native.json resolution=64x64 spp=2

summary=C:\Users\Cam\Desktop\vkPathTracer\experiments\d3d12_dxr_optimization_graph\results\graph_summary.json
dot=C:\Users\Cam\Desktop\vkPathTracer\experiments\d3d12_dxr_optimization_graph\results\graph.dot
all_declared_edges_explored=True edge_count=14
run cpu_scalar status=ok mode=scalar build_ms=0.114500 render_ms=6.518700 paths_per_sec=5,001,610.751
run d3d12_compute_dynamic_off status=ok mode=d3d12-compute build_ms=2,754.778300 render_ms=2.053700 paths_per_sec=3,988,898.086
run d3d12_compute_readback_every status=ok mode=d3d12-compute build_ms=3,032.073600 render_ms=1.879200 paths_per_sec=4,359,301.831
run d3d12_compute_rpp1 status=ok mode=d3d12-compute build_ms=2,950.775800 render_ms=1.717100 paths_per_sec=4,770,834.547
run d3d12_compute_rpp2 status=ok mode=d3d12-compute build_ms=2,770.741400 render_ms=2.346400 paths_per_sec=6,982,611.660
run d3d12_compute_rpp4 status=ok mode=d3d12-compute build_ms=2,720.888900 render_ms=2.674600 paths_per_sec=12,251,551.634
run d3d12_compute_rpp8 status=ok mode=d3d12-compute build_ms=2,722.379300 render_ms=3.648600 paths_per_sec=17,961,958.011
run d3d12_dxr_build_none status=ok mode=d3d12-dxr build_ms=5,087.928500 render_ms=7.664900 paths_per_sec=1,068,768.020
run d3d12_dxr_dynamic_off status=ok mode=d3d12-dxr build_ms=4,671.446800 render_ms=7.893000 paths_per_sec=1,037,881.667
run d3d12_dxr_fast_build status=ok mode=d3d12-dxr build_ms=4,629.494300 render_ms=7.667400 paths_per_sec=1,068,419.542
run d3d12_dxr_fast_trace status=ok mode=d3d12-dxr build_ms=4,775.086800 render_ms=7.684300 paths_per_sec=1,066,069.779
edge edge_backend_cpu_to_d3d12_compute status=ok decision=keep_baseline speedup=0.953860 reason=
edge edge_backend_d3d12_compute_to_dxr status=ok decision=keep_or_fallback_compute speedup=0.223456 reason=mode=d3d12-dxr
edge edge_compute_rpp_1_to_2 status=ok decision=take_candidate speedup=1.463604 reason=
edge edge_compute_rpp_1_to_4 status=ok decision=take_candidate speedup=2.568010 reason=
edge edge_compute_rpp_1_to_8 status=ok decision=take_candidate speedup=3.764951 reason=
edge edge_compute_readback_final_to_every status=ok decision=keep_final_only speedup=0.913740 reason=
edge edge_compute_dynamic_instances_on_to_off status=ok decision=keep_dynamic_on speedup=0.836101 reason=
edge edge_dxr_fast_trace_to_fast_build status=ok decision=keep_fast_trace speedup=1.002204 reason=
edge edge_dxr_fast_trace_to_none status=ok decision=keep_fast_trace speedup=1.002531 reason=
edge edge_dxr_dynamic_instances_on_to_off status=ok decision=keep_dynamic_on speedup=0.973559 reason=
edge edge_shader_compute_main_compile status=ok decision=compiled speedup=0.000000 reason=
edge edge_shader_compute_tonemap_compile status=ok decision=compiled speedup=0.000000 reason=
edge edge_shader_dxr_library_compile status=ok decision=compiled speedup=0.000000 reason=
edge edge_shader_small_int_pow_scan status=ok decision=already_applied speedup=0.000000 reason=small_int_pow_mentions=0

## Run 2026-05-05T16:24:52.3872876-04:00
ptbench=C:\Users\Cam\Desktop\vkPathTracer\build\d3d12-optimization\bin\ptbench.exe
scene=C:\Users\Cam\Desktop\vkPathTracer\assets\scenes\cornell_native.json resolution=64x64 spp=2

summary=C:\Users\Cam\Desktop\vkPathTracer\experiments\d3d12_dxr_optimization_graph\results\graph_summary.json
dot=C:\Users\Cam\Desktop\vkPathTracer\experiments\d3d12_dxr_optimization_graph\results\graph.dot
all_declared_edges_explored=True edge_count=14
run cpu_scalar status=ok mode=scalar build_ms=0.125000 render_ms=5.911900 paths_per_sec=5,514,978.264
run d3d12_compute_dynamic_off status=ok mode=d3d12-compute build_ms=2,851.193400 render_ms=1.650200 paths_per_sec=4,964,246.758
run d3d12_compute_readback_every status=ok mode=d3d12-compute build_ms=2,891.910200 render_ms=1.674700 paths_per_sec=4,891,622.380
run d3d12_compute_rpp1 status=ok mode=d3d12-compute build_ms=3,021.183000 render_ms=1.716300 paths_per_sec=4,773,058.323
run d3d12_compute_rpp2 status=ok mode=d3d12-compute build_ms=2,976.844200 render_ms=2.524100 paths_per_sec=6,491,026.504
run d3d12_compute_rpp4 status=ok mode=d3d12-compute build_ms=2,766.248500 render_ms=2.612900 paths_per_sec=12,540,854.989
run d3d12_compute_rpp8 status=ok mode=d3d12-compute build_ms=2,663.823300 render_ms=3.291900 paths_per_sec=19,908,259.668
run d3d12_dxr_build_none status=ok mode=d3d12-dxr build_ms=6,517.645000 render_ms=85.585200 paths_per_sec=95,717.484
run d3d12_dxr_dynamic_off status=ok mode=d3d12-dxr build_ms=10,634.628600 render_ms=8.286100 paths_per_sec=988,643.632
run d3d12_dxr_fast_build status=ok mode=d3d12-dxr build_ms=4,492.139600 render_ms=8.132500 paths_per_sec=1,007,316.323
run d3d12_dxr_fast_trace status=ok mode=d3d12-dxr build_ms=5,149.294800 render_ms=7.441000 paths_per_sec=1,100,927.295
edge edge_backend_cpu_to_d3d12_compute status=ok decision=take_candidate speedup=3.444561 reason=
edge edge_backend_d3d12_compute_to_dxr status=ok decision=keep_or_fallback_compute speedup=0.230654 reason=mode=d3d12-dxr
edge edge_compute_rpp_1_to_2 status=ok decision=take_candidate speedup=1.359930 reason=
edge edge_compute_rpp_1_to_4 status=ok decision=take_candidate speedup=2.627425 reason=
edge edge_compute_rpp_1_to_8 status=ok decision=take_candidate speedup=4.170965 reason=
edge edge_compute_readback_final_to_every status=ok decision=take_candidate speedup=1.024840 reason=
edge edge_compute_dynamic_instances_on_to_off status=ok decision=take_candidate speedup=1.040056 reason=
edge edge_dxr_fast_trace_to_fast_build status=ok decision=keep_fast_trace speedup=1.146290 reason=render_speedup=0.9149707961887
edge edge_dxr_fast_trace_to_none status=ok decision=keep_fast_trace speedup=0.790055 reason=render_speedup=0.0869426022256245
edge edge_dxr_dynamic_instances_on_to_off status=ok decision=keep_dynamic_on speedup=0.898010 reason=
edge edge_shader_compute_main_compile status=ok decision=compiled speedup=0.000000 reason=
edge edge_shader_compute_tonemap_compile status=ok decision=compiled speedup=0.000000 reason=
edge edge_shader_dxr_library_compile status=ok decision=compiled speedup=0.000000 reason=
edge edge_shader_small_int_pow_scan status=ok decision=already_applied speedup=0.000000 reason=small_int_pow_mentions=0

## Run 2026-05-05T16:26:22.5270637-04:00
ptbench=C:\Users\Cam\Desktop\vkPathTracer\build\d3d12-optimization\bin\ptbench.exe
scene=C:\Users\Cam\Desktop\vkPathTracer\assets\scenes\cornell_native.json resolution=64x64 spp=2

summary=C:\Users\Cam\Desktop\vkPathTracer\experiments\d3d12_dxr_optimization_graph\results\graph_summary.json
dot=C:\Users\Cam\Desktop\vkPathTracer\experiments\d3d12_dxr_optimization_graph\results\graph.dot
all_declared_edges_explored=True edge_count=14
run cpu_scalar status=ok mode=scalar build_ms=0.117400 render_ms=5.545000 paths_per_sec=5,879,891.794
run d3d12_compute_dynamic_off status=ok mode=d3d12-compute build_ms=3,026.134400 render_ms=15.388500 paths_per_sec=532,345.583
run d3d12_compute_readback_every status=ok mode=d3d12-compute build_ms=2,944.904100 render_ms=15.510600 paths_per_sec=528,154.939
run d3d12_compute_rpp1 status=ok mode=d3d12-compute build_ms=3,020.304900 render_ms=2.634000 paths_per_sec=3,110,098.709
run d3d12_compute_rpp2 status=ok mode=d3d12-compute build_ms=3,356.726800 render_ms=2.771300 paths_per_sec=5,912,026.847
run d3d12_compute_rpp4 status=ok mode=d3d12-compute build_ms=3,382.438600 render_ms=15.660500 paths_per_sec=2,092,398.072
run d3d12_compute_rpp8 status=ok mode=d3d12-compute build_ms=3,084.453000 render_ms=16.042900 paths_per_sec=4,085,046.968
run d3d12_dxr_build_none status=ok mode=d3d12-dxr build_ms=4,856.633300 render_ms=7.340500 paths_per_sec=1,116,000.272
run d3d12_dxr_dynamic_off status=ok mode=d3d12-dxr build_ms=4,946.757700 render_ms=13.452000 paths_per_sec=608,980.077
run d3d12_dxr_fast_build status=ok mode=d3d12-dxr build_ms=4,871.852700 render_ms=20.205300 paths_per_sec=405,438.177
run d3d12_dxr_fast_trace status=ok mode=d3d12-dxr build_ms=5,550.666000 render_ms=11.533200 paths_per_sec=710,297.229
edge edge_backend_cpu_to_d3d12_compute status=ok decision=take_candidate speedup=2.105163 reason=
edge edge_backend_d3d12_compute_to_dxr status=ok decision=keep_or_fallback_compute speedup=0.228384 reason=mode=d3d12-dxr
edge edge_compute_rpp_1_to_2 status=ok decision=take_candidate speedup=1.900913 reason=
edge edge_compute_rpp_1_to_4 status=ok decision=keep_baseline speedup=0.672775 reason=
edge edge_compute_rpp_1_to_8 status=ok decision=take_candidate speedup=1.313478 reason=
edge edge_compute_readback_final_to_every status=ok decision=keep_final_only speedup=0.169819 reason=
edge edge_compute_dynamic_instances_on_to_off status=ok decision=keep_dynamic_on speedup=0.171167 reason=
edge edge_dxr_fast_trace_to_fast_build status=ok decision=keep_fast_trace speedup=1.139334 reason=render_speedup=0.570800730501237
edge edge_dxr_fast_trace_to_none status=ok decision=take_candidate_build speedup=1.142904 reason=render_speedup=1.57117362577414
edge edge_dxr_dynamic_instances_on_to_off status=ok decision=keep_dynamic_on speedup=0.857360 reason=
edge edge_shader_compute_main_compile status=ok decision=compiled speedup=0.000000 reason=
edge edge_shader_compute_tonemap_compile status=ok decision=compiled speedup=0.000000 reason=
edge edge_shader_dxr_library_compile status=ok decision=compiled speedup=0.000000 reason=
edge edge_shader_small_int_pow_scan status=ok decision=already_applied speedup=0.000000 reason=small_int_pow_mentions=0

## Run 2026-05-05T16:28:10.2731052-04:00
ptbench=C:\Users\Cam\Desktop\vkPathTracer\build\d3d12-optimization\bin\ptbench.exe
scene=C:\Users\Cam\Desktop\vkPathTracer\assets\scenes\cornell_native.json resolution=64x64 spp=2 repeats=3

summary=C:\Users\Cam\Desktop\vkPathTracer\experiments\d3d12_dxr_optimization_graph\results\graph_summary.json
dot=C:\Users\Cam\Desktop\vkPathTracer\experiments\d3d12_dxr_optimization_graph\results\graph.dot
all_declared_edges_explored=True edge_count=14
run cpu_scalar status=ok mode=scalar build_ms=0.157300 render_ms=6.091700 paths_per_sec=5,569,810.547
run d3d12_compute_dynamic_off status=ok mode=d3d12-compute build_ms=3,063.046900 render_ms=1.924600 paths_per_sec=4,689,718.342
run d3d12_compute_readback_every status=ok mode=d3d12-compute build_ms=2,781.021000 render_ms=2.028100 paths_per_sec=4,802,720.291
run d3d12_compute_rpp1 status=ok mode=d3d12-compute build_ms=2,777.483600 render_ms=1.689700 paths_per_sec=5,091,044.683
run d3d12_compute_rpp2 status=ok mode=d3d12-compute build_ms=3,003.350100 render_ms=2.021200 paths_per_sec=8,302,001.520
run d3d12_compute_rpp4 status=ok mode=d3d12-compute build_ms=2,774.328500 render_ms=3.074800 paths_per_sec=13,794,729.309
run d3d12_compute_rpp8 status=ok mode=d3d12-compute build_ms=2,707.091800 render_ms=3.418000 paths_per_sec=19,742,732.339
run d3d12_dxr_build_none status=ok mode=d3d12-dxr build_ms=5,810.758600 render_ms=12.315200 paths_per_sec=1,077,497.764
run d3d12_dxr_dynamic_off status=ok mode=d3d12-dxr build_ms=5,134.581500 render_ms=7.610400 paths_per_sec=1,164,446.845
run d3d12_dxr_fast_build status=ok mode=d3d12-dxr build_ms=5,676.428600 render_ms=8.995900 paths_per_sec=1,124,548.712
run d3d12_dxr_fast_trace status=ok mode=d3d12-dxr build_ms=4,930.563700 render_ms=7.403000 paths_per_sec=1,128,996.692
edge edge_backend_cpu_to_d3d12_compute status=ok decision=take_candidate speedup=3.637872 reason=
edge edge_backend_d3d12_compute_to_dxr status=ok decision=keep_or_fallback_compute speedup=0.221761 reason=mode=d3d12-dxr
edge edge_compute_rpp_1_to_2 status=ok decision=take_candidate speedup=1.630707 reason=
edge edge_compute_rpp_1_to_4 status=ok decision=take_candidate speedup=2.709607 reason=
edge edge_compute_rpp_1_to_8 status=ok decision=take_candidate speedup=3.877933 reason=
edge edge_compute_readback_final_to_every status=ok decision=keep_final_only speedup=0.943366 reason=
edge edge_compute_dynamic_instances_on_to_off status=ok decision=keep_dynamic_on speedup=0.921170 reason=
edge edge_dxr_fast_trace_to_fast_build status=ok decision=keep_fast_trace speedup=0.868603 reason=render_speedup=0.996060235835968
edge edge_dxr_fast_trace_to_none status=ok decision=keep_fast_trace speedup=0.848523 reason=render_speedup=0.954385226495355
edge edge_dxr_dynamic_instances_on_to_off status=ok decision=keep_dynamic_on speedup=1.031400 reason=
edge edge_shader_compute_main_compile status=ok decision=compiled speedup=0.000000 reason=
edge edge_shader_compute_tonemap_compile status=ok decision=compiled speedup=0.000000 reason=
edge edge_shader_dxr_library_compile status=ok decision=compiled speedup=0.000000 reason=
edge edge_shader_small_int_pow_scan status=ok decision=already_applied speedup=0.000000 reason=small_int_pow_mentions=0
