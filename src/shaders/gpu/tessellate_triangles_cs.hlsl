// Cached triangle tessellation compute pass.
//
// First implementation target:
// - uniform subdivision factor
// - deterministic output offsets computed by CPU
// - duplicated edge vertices
// - generated vertex/index buffers feed BVH or DXR BLAS builds
//
// Dispatch shape: one thread per source triangle.

cbuffer TessellationConstants : register(b0) {
    uint source_triangle_count;
    uint tessellation_factor;
    uint source_first_index;
    uint generated_first_vertex;
    uint generated_first_index;
    uint source_vertex_base;
    uint projection_mode;
    float projection_center_x;
    float projection_center_y;
    float projection_center_z;
    float projection_radius;
};

StructuredBuffer<float3> SourceVertices : register(t0);
StructuredBuffer<uint> SourceIndices : register(t1);
RWStructuredBuffer<float3> GeneratedVertices : register(u0);
RWStructuredBuffer<uint> GeneratedIndices : register(u1);

uint VerticesPerSourceTriangle(uint factor) {
    return ((factor + 1u) * (factor + 2u)) / 2u;
}

uint GridVertexIndex(uint row, uint col, uint factor) {
    // Rows shrink as they approach source vertex 2.
    // Sum_{r=0}^{row-1} (factor + 1 - r) + col.
    return row * (factor + 1u) - ((row * (row - 1u)) / 2u) + col;
}

float3 BarycentricPoint(float3 p0, float3 p1, float3 p2, uint row, uint col, uint factor) {
    const uint w2 = row;
    const uint w1 = col;
    const uint w0 = factor - w1 - w2;
    const float inv = 1.0f / max(1.0f, float(factor));
    return (float(w0) * p0 + float(w1) * p1 + float(w2) * p2) * inv;
}

float3 ProjectGeneratedPoint(float3 p) {
    if (projection_mode == 1u) {
        const float3 center = float3(projection_center_x, projection_center_y, projection_center_z);
        const float3 delta = p - center;
        const float len_sq = max(dot(delta, delta), 1.0e-12f);
        return center + delta * (max(0.0f, projection_radius) * rsqrt(len_sq));
    }
    return p;
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID) {
    const uint tri = gid.x;
    const uint factor = max(1u, tessellation_factor);
    if (tri >= source_triangle_count) {
        return;
    }

    const uint source_index = source_first_index + tri * 3u;
    const uint i0 = source_vertex_base + SourceIndices[source_index + 0u];
    const uint i1 = source_vertex_base + SourceIndices[source_index + 1u];
    const uint i2 = source_vertex_base + SourceIndices[source_index + 2u];

    const float3 p0 = SourceVertices[i0];
    const float3 p1 = SourceVertices[i1];
    const float3 p2 = SourceVertices[i2];

    const uint verts_per_tri = VerticesPerSourceTriangle(factor);
    const uint vertex_base = generated_first_vertex + tri * verts_per_tri;
    const uint index_base = generated_first_index + tri * factor * factor * 3u;

    for (uint row = 0u; row <= factor; ++row) {
        const uint cols = factor - row;
        for (uint col = 0u; col <= cols; ++col) {
            const uint out_vertex = vertex_base + GridVertexIndex(row, col, factor);
            GeneratedVertices[out_vertex] = ProjectGeneratedPoint(
                BarycentricPoint(p0, p1, p2, row, col, factor));
        }
    }

    uint out_index = index_base;
    for (uint row = 0u; row < factor; ++row) {
        const uint cols = factor - row;
        for (uint col = 0u; col < cols; ++col) {
            const uint v00 = vertex_base + GridVertexIndex(row, col, factor);
            const uint v10 = vertex_base + GridVertexIndex(row + 1u, col, factor);
            const uint v01 = vertex_base + GridVertexIndex(row, col + 1u, factor);

            GeneratedIndices[out_index + 0u] = v00;
            GeneratedIndices[out_index + 1u] = v10;
            GeneratedIndices[out_index + 2u] = v01;
            out_index += 3u;

            if (col + 1u < cols) {
                const uint v11 = vertex_base + GridVertexIndex(row + 1u, col + 1u, factor);
                GeneratedIndices[out_index + 0u] = v10;
                GeneratedIndices[out_index + 1u] = v11;
                GeneratedIndices[out_index + 2u] = v01;
                out_index += 3u;
            }
        }
    }
}
