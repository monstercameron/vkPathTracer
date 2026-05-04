// pathtrace_rt.hlsl — DXR path tracing library shader (target: lib_6_3)
// Iterative path tracer: raygen loops over depth, no shader recursion needed.
// Resource layout mirrors pathtrace_cs.hlsl so the same CPU constant buffer works.

// ---- Constants (matches PathTraceConstants in D3D12GpuPathTracer.h) --------
cbuffer PCBuf : register(b0) {
    float  camera_pos_x;  float camera_pos_y;  float camera_pos_z;  float fov_tan_half;
    float  cam_fwd_x;     float cam_fwd_y;     float cam_fwd_z;     float aspect;
    float  cam_right_x;   float cam_right_y;   float cam_right_z;   float pad0;
    float  cam_up_x;      float cam_up_y;      float cam_up_z;      uint  sample_index;
    uint   num_insts;     uint  num_mats;       uint  num_lights;    uint  width;
    uint   height;        uint  base_seed;      float env_r;         float env_g;
    float  env_b;         float max_depth_f;    uint  rays_per_pixel; float _pad1;
};

// ---- Resources -------------------------------------------------------------
RaytracingAccelerationStructure SceneTLAS : register(t0, space0);
Buffer<float>    VertBuf   : register(t1, space0);
Buffer<uint>     IndexBuf  : register(t2, space0);
Buffer<float>    MatBuf    : register(t3, space0);
Buffer<uint>     InstBuf   : register(t4, space0);   // kept for binding compat
Buffer<float>    LightBuf  : register(t5, space0);   // reserved for NEE
Buffer<uint>     TriMatBuf : register(t6, space0);
RWBuffer<float4> FilmBuf   : register(u0, space0);

// ---- Payload (64 bytes — must match MaxPayloadSizeInBytes in PSO) -----------
struct PathPayload {
    float3 radiance;     // 12
    uint   done;         //  4  → 16
    float3 throughput;   // 12
    uint   depth;        //  4  → 32
    float3 next_origin;  // 12
    uint   rng;          //  4  → 48
    float3 next_dir;     // 12
    float  _pad;         //  4  → 64
};

// ---- Forward declarations --------------------------------------------------
uint  Pcg(uint v);
float RandF(inout uint rng);
float Halton2(uint idx);
float Halton3(uint idx);

// ---- Raygen ----------------------------------------------------------------
[shader("raygeneration")]
void RayGen() {
    uint2 pix = DispatchRaysIndex().xy;
    if (pix.x >= width || pix.y >= height) return;
    uint pixel = pix.y * width + pix.x;

    float3 cam_pos   = float3(camera_pos_x, camera_pos_y, camera_pos_z);
    float3 cam_fwd   = float3(cam_fwd_x,    cam_fwd_y,    cam_fwd_z);
    float3 cam_right = float3(cam_right_x,  cam_right_y,  cam_right_z);
    float3 cam_up    = float3(cam_up_x,     cam_up_y,     cam_up_z);

    uint   rpp   = max(1u, rays_per_pixel);
    float3 total = (float3)0;

    for (uint ri = 0u; ri < rpp; ++ri) {
        uint eff = sample_index * rpp + ri;
        uint rng = Pcg(pixel ^ Pcg(eff * 1664525u ^ base_seed) ^ (ri * 2654435761u));

        float jx = Halton2(eff);
        float jy = Halton3(eff);
        float fx = ((float)pix.x + jx) / (float)width;
        float fy = ((float)pix.y + jy) / (float)height;
        float nx = (2.0f * fx - 1.0f) * aspect * fov_tan_half;
        float ny = (1.0f - 2.0f * fy) * fov_tan_half;
        float3 dir = normalize(cam_fwd + cam_right * nx + cam_up * ny);

        PathPayload payload;
        payload.radiance    = (float3)0;
        payload.throughput  = (float3)1;
        payload.done        = 0u;
        payload.depth       = 0u;
        payload.rng         = rng;
        payload.next_origin = cam_pos;
        payload.next_dir    = dir;
        payload._pad        = 0.0f;

        uint maxD = (uint)max(1.0f, max_depth_f);
        for (uint d = 0u; d < maxD && payload.done == 0u; ++d) {
            RayDesc ray;
            ray.Origin    = payload.next_origin;
            ray.Direction = payload.next_dir;
            ray.TMin      = 1e-4f;
            ray.TMax      = 1e30f;
            TraceRay(SceneTLAS,
                     RAY_FLAG_NONE,
                     /*InstanceMask*/ 0xFF,
                     /*HitGroup offset*/ 0,
                     /*Geometry multiplier*/ 1,
                     /*MissShader index*/ 0,
                     ray, payload);
        }
        total += payload.radiance;
    }

    float4 prev = FilmBuf[pixel];
    FilmBuf[pixel] = float4(prev.xyz + total, prev.w + (float)rpp);
}

// ---- Miss ------------------------------------------------------------------
[shader("miss")]
void Miss(inout PathPayload payload) {
    payload.radiance += payload.throughput * float3(env_r, env_g, env_b);
    payload.done = 1u;
}

// ---- ClosestHit ------------------------------------------------------------
[shader("closesthit")]
void ClosestHit(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attr) {
    uint triIdx = PrimitiveIndex();
    uint matIdx = TriMatBuf[triIdx];
    uint i0 = IndexBuf[triIdx * 3u + 0u];
    uint i1 = IndexBuf[triIdx * 3u + 1u];
    uint i2 = IndexBuf[triIdx * 3u + 2u];
    float3 v0 = float3(VertBuf[i0 * 3u], VertBuf[i0 * 3u + 1u], VertBuf[i0 * 3u + 2u]);
    float3 v1 = float3(VertBuf[i1 * 3u], VertBuf[i1 * 3u + 1u], VertBuf[i1 * 3u + 2u]);
    float3 v2 = float3(VertBuf[i2 * 3u], VertBuf[i2 * 3u + 1u], VertBuf[i2 * 3u + 2u]);
    float3 n = normalize(cross(v1 - v0, v2 - v0));
    if (dot(n, WorldRayDirection()) > 0.0f) n = -n;
    uint   mb      = matIdx * 8u;
    float3 albedo   = float3(MatBuf[mb + 0u], MatBuf[mb + 1u], MatBuf[mb + 2u]);
    float3 emissive = float3(MatBuf[mb + 3u], MatBuf[mb + 4u], MatBuf[mb + 5u]);
    float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    payload.radiance  += payload.throughput * emissive;
    payload.throughput *= albedo;
    uint rng = payload.rng;
    if (payload.depth >= 2u) {
        float q = max(payload.throughput.x, max(payload.throughput.y, payload.throughput.z));
        q = clamp(q, 0.05f, 0.95f);
        if (RandF(rng) > q) { payload.rng = rng; payload.done = 1u; return; }
        payload.throughput /= q;
    }
    float r1 = RandF(rng); float r2 = RandF(rng); payload.rng = rng;
    float phi = 6.28318530718f * r1;
    float sinTheta = sqrt(1.0f - r2); float cosTheta = sqrt(r2);
    float3 t = (abs(n.x) > 0.9f) ? float3(0,1,0) : float3(1,0,0);
    float3 bitan = normalize(cross(n, t)); float3 tan2 = cross(bitan, n);
    float3 local = cos(phi)*sinTheta*tan2 + sin(phi)*sinTheta*bitan + cosTheta*n;
    payload.next_origin = hitPos + n * 1e-4f;
    payload.next_dir    = normalize(local);
    payload.depth++;
}

// ---- PCG hash / sampling helpers -------------------------------------------
uint Pcg(uint v) {
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

float RandF(inout uint rng) {
    rng = Pcg(rng);
    return (float)rng / 4294967296.0f;
}

float Halton2(uint idx) {
    uint bits = idx;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}

float Halton3(uint idx) {
    float result = 0.0f;
    float f      = 1.0f / 3.0f;
    uint  i      = idx;
    for (uint step = 0u; step < 21u; ++step) {
        if (i == 0u) break;
        result += f * (float)(i % 3u);
        i      /= 3u;
        f      /= 3.0f;
    }
    return result;
}
