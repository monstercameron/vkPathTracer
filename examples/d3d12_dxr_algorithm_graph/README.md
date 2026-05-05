# D3D12 Path Tracing Algorithm Graph

This example benchmarks code-level D3D12 compute path tracing decisions:

- CPU BVH leaf size: `PT_D3D12_BVH_LEAF_SIZE`
- CPU BVH split strategy: `PT_D3D12_BVH_SPLIT_MODE`
- SAH bucket count: `PT_D3D12_BVH_BUCKETS`
- GPU static BVH traversal variant: `PT_D3D12_SHADER_TRAVERSAL`

Run:

```powershell
.\examples\d3d12_dxr_algorithm_graph\run_graph.ps1 -Ptbench build\d3d12-optimization\bin\ptbench.exe
```

The notebook-style scratch data goes in `NOTES.md`. Raw run artifacts and generated graph summaries go in `results/`.

Pass 3 applied the balanced default from the graph: SAH split, leaf size 16, and 16 SAH buckets. Override the environment variables above to replay any edge.
