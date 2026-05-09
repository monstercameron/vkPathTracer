// Phase 4 SKN03 vertex-skinning compute shader.
//
// One thread per vertex; threadgroup size 64. Inputs are the bind-pose
// positions/normals plus per-vertex JOINTS_0/WEIGHTS_0 and the per-frame
// skinning matrix table (joint_world_matrices[i] * inverse_bind[i]). Outputs
// are the deformed positions/normals that the BLAS refit consumes.
//
// Status: shader is authored as the canonical reference; the D3D12 backend
// dispatch wiring + BLAS_FLAG_ALLOW_UPDATE plumbing is documented as a Phase 4
// scope cut (see commit body). The shader is kept in-tree so the GPU pipeline
// only needs the dispatch wiring once per backend, not the algorithm itself.

cbuffer SkinPushConstants : register(b0) {
  uint vertex_count;
  uint _pad0;
  uint _pad1;
  uint _pad2;
};

StructuredBuffer<float3> bind_positions   : register(t0);
StructuredBuffer<float3> bind_normals     : register(t1);
StructuredBuffer<uint4>  joint_indices    : register(t2);
StructuredBuffer<float4> joint_weights    : register(t3);
StructuredBuffer<float4x4> skinning_matrices : register(t4);

RWStructuredBuffer<float3> skinned_positions : register(u0);
RWStructuredBuffer<float3> skinned_normals   : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
  const uint v = dtid.x;
  if (v >= vertex_count) {
    return;
  }

  const uint4  idx = joint_indices[v];
  const float4 w   = joint_weights[v];

  // Linear-blended skinning: weighted sum of four bone matrices.
  float4x4 skin =
      w.x * skinning_matrices[idx.x] +
      w.y * skinning_matrices[idx.y] +
      w.z * skinning_matrices[idx.z] +
      w.w * skinning_matrices[idx.w];

  const float3 bind_p = bind_positions[v];
  const float3 bind_n = bind_normals[v];

  skinned_positions[v] = mul(skin, float4(bind_p, 1.0f)).xyz;
  // Use the upper-3x3 of the skinning matrix for normals (adequate when the
  // matrix has no non-uniform scale; full inverse-transpose is a TODO if the
  // hero ever ships with non-uniform bone scaling).
  skinned_normals[v] = mul((float3x3)skin, bind_n);
}
